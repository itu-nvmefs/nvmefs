#include "nvme_device.hpp"
#include "nvme_io_engine.hpp"
#include "io_engines/nvme_async_io_engine.hpp"
#include "io_engines/nvme_sync_io_engine.hpp"
#include "io_engines/nvme_async_prefetch_io_engine.hpp"
#include "buffer_allocators/nvme_default_buffer_allocator.hpp"
#include "buffer_allocators/nvme_cached_buffer_allocator.hpp"
#include "nvmefs_path_handler.hpp"

namespace duckdb {
thread_local optional_idx NvmeDevice::index = optional_idx();
NvmeDevice::NvmeDevice(const NvmeConfig &config)
    : dev_path(config.device_path), backend(config.backend), max_threads(config.max_threads) {
	xnvme_opts opts = xnvme_opts_default();
	PrepareOpts(opts);
	device = xnvme_dev_open(dev_path.c_str(), &opts);
	if (!device) {
		xnvme_cli_perr("xnvme_dev_open()", errno);
		throw InternalException("Unable to open device");
	}

	// Initialize the xnvme queue for asynchronous IO
	// Set the callback function for completed commands. No callback arguments, hence last argument equal to NULL
	queues = vector<xnvme_queue *>(max_threads, nullptr);

	use_fdp = config.use_fdp;
	bool fdp_capable = CheckFDP();
	if (use_fdp && !fdp_capable) {
		throw IOException("[nvmefs] FDP Requested but device does not support it");
	}

	if (use_fdp) {
		Printer::Print("[nvmefs] FDP enabled. Initializing placement handles...");
		InitializePlacementHandles();
		Printer::Print("[nvmefs] FDP placement handles initialized");
	} else {
		Printer::Print("[nvmefs] FDP disabled. Writes will use regular placement");
	}

	if (StringUtil::Contains(config.meta, "use_sync_writer")) {
		duckdb::Printer::Print("[nvmefs] Using synchronous IO engine");
		io_engine = make_uniq<NvmeSyncIOEngine>(*this);
	} else if (StringUtil::Contains(config.meta, "use_async_prefetch")) {
		duckdb::Printer::Print("[nvmefs] Using Async Prefetch IO engine");
		io_engine = make_uniq<NvmeAsynPrefetchIOEngine>(*this);
	} else {
		duckdb::Printer::Print("[nvmefs] Using asynchronous IO engine");
		io_engine = make_uniq<NvmeAsyncIOEngine>(*this);
	}

	if (StringUtil::Contains(config.meta, "use_cached_allocator")) {
		duckdb::Printer::Print("[nvmefs] Using cached buffer allocator");
		buffer_allocator = make_uniq<NvmeCachedBufferAllocator>(device);
	} else {
		duckdb::Printer::Print("[nvmefs] Using default buffer allocator");
		buffer_allocator = make_uniq<NvmeDefaultBufferAllocator>(device);
	}

	GetThreadIndex();

	allocated_placement_identifiers = std::map<string, uint8_t>(config.fdp_mapping.begin(), config.fdp_mapping.end());
	geometry = LoadDeviceGeometry();
}

NvmeDevice::~NvmeDevice() {
	for (const auto &queue : queues) {
		xnvme_queue_term(queue);
	}

	xnvme_dev_close(device);
}

DeviceGeometry NvmeDevice::GetDeviceGeometry() {
	return geometry;
}

uint8_t NvmeDevice::GetPlacementIdentifierOrDefault(const string &path) {
	NvmeFileType type = NvmePathHandler::GetFileType(path);

	string suffix;
	switch (type) {
	case NvmeFileType::DATABASE:
		suffix = ".db";
		break;
	case NvmeFileType::WAL:
		suffix = ".wal";
		break;
	case NvmeFileType::TEMPORARY:
		suffix = ".tmp";
		break;
	default:
		return 0;
	}

	// Use database name with suffix
	if (type != NvmeFileType::TEMPORARY) {
		string db_name = NvmePathHandler::ExtractDatabaseName(path);
		if (!db_name.empty()) {
			auto it = allocated_placement_identifiers.find(db_name + suffix);
			if (it != allocated_placement_identifiers.end()) {
				return it->second;
			}
		}
	}

	// Fallback to global suffix
	auto it = allocated_placement_identifiers.find(suffix);
	if (it != allocated_placement_identifiers.end()) {
		return it->second;
	}

	// Default fallback RUH
	return 0;
}

nvme_buf_ptr NvmeDevice::AllocateDeviceBuffer(idx_t nr_bytes) {
	return buffer_allocator->Allocate(nr_bytes);
}

void NvmeDevice::FreeDeviceBuffer(nvme_buf_ptr buffer, idx_t size) {
	buffer_allocator->Free(buffer, size);
}

DeviceGeometry NvmeDevice::LoadDeviceGeometry() {
	NvmeDeviceGeometry geometry {};

	const xnvme_geo *geo = xnvme_dev_get_geo(device);
	const xnvme_spec_idfy_ns *nsgeo = xnvme_dev_get_ns(device);

	geometry.lba_size = geo->lba_nbytes;
	geometry.lba_count = nsgeo->nsze;

	return geometry;
}

void NvmeDevice::PrepareOpts(xnvme_opts &opts) {
	// SPDK backend handles async/sync internally - don't set opts.async or opts.sync
	if (StringUtil::Contains(this->backend, "spdk")) {
		opts.be = "spdk";
		// Don't set opts.async or opts.sync for SPDK - it manages this internally
		return;
	}

	opts.async = this->backend.data();
	if (StringUtil::Equals(this->backend.data(), "io_uring_cmd")) {
		opts.sync = "nvme";
	}
}

void NvmeDevice::CommandCallback(struct xnvme_cmd_ctx *ctx, void *cb_args) {
	// Cast callback args into defined callback struct
	std::promise<void> *notifier = (std::promise<void> *)cb_args;

	// Check status
	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		xnvme_cli_pinf("Command did not complete successfully");
		xnvme_cmd_ctx_pr(ctx, XNVME_PR_DEF);
	}

	// Put command context back to queue, and notify the future
	xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
	notifier->set_value();
}

void NvmeDevice::Read(void *buffer, const CmdContext &context) {
	const NvmeCmdContext &ctx = static_cast<const NvmeCmdContext &>(context);
	D_ASSERT(ctx.nr_lbas > 0);
	// We only support offset reads within a single block
	D_ASSERT((ctx.offset == 0 && ctx.nr_lbas > 1) || (ctx.offset >= 0 && ctx.nr_lbas == 1));

	io_engine->Read(buffer, ctx);
}

void NvmeDevice::Write(void *buffer, const CmdContext &context) {
	const NvmeCmdContext &ctx = static_cast<const NvmeCmdContext &>(context);
	D_ASSERT(ctx.nr_lbas > 0);
	D_ASSERT((ctx.offset == 0 && ctx.nr_lbas > 1) || (ctx.offset >= 0 && ctx.nr_lbas == 1));

	io_engine->Write(buffer, ctx);
}

void NvmeDevice::PrepareIOCmdContext(xnvme_cmd_ctx *ctx, const CmdContext &cmd_ctx, idx_t plid_idx, idx_t dtype,
                                     bool write) {
	const NvmeCmdContext &nvme_cmd_ctx = static_cast<const NvmeCmdContext &>(cmd_ctx);

	uint16_t nr_lbas = nvme_cmd_ctx.nr_lbas - 1;
	ctx->cmd.common.cdw12 = nr_lbas;

	if (write && use_fdp) {
		ctx->cmd.common.cdw12 |= dtype << 20;

		uint16_t phid = placement_handlers.empty() ? 0 : placement_handlers[0]; // Fallback

		// FIX: Match the requested ID directly to the physical Placement Identifier
		for (uint16_t pi : placement_handlers) {
			if (pi == plid_idx) {
				phid = pi;
				break;
			}
		}

		// --- DEBUG PRINT (Limited to 10 lines to avoid terminal flooding) ---
		static std::atomic<int> dbg_prints {0};
		if (dbg_prints.fetch_add(1) < 10) {
			duckdb::Printer::Print(StringUtil::Format(
			    "[FDP Debug IO] DuckDB configured FDP ID: %d -> Sending to NVMe PI: %d", plid_idx, phid));
		}

		ctx->cmd.common.cdw13 = phid << 16;
	}
}

bool NvmeDevice::CheckFDP() {
	const xnvme_spec_idfy_ctrlr *ctrlr = xnvme_dev_get_ctrlr(device);
	if (!ctrlr) {
		xnvme_cli_perr("xnvme_dev_get_ctrlr()", errno);
		return false;
	}
	return ctrlr->ctratt.flexible_data_placement;
}

void NvmeDevice::InitializePlacementHandles() {
	uint32_t nsid = xnvme_dev_get_nsid(device);
	xnvme_cmd_ctx xnvme_ctx = xnvme_cmd_ctx_from_dev(device);

	struct xnvme_spec_ruhs header;
	uint32_t header_bytes = sizeof(header);
	xnvme_nvm_mgmt_recv(&xnvme_ctx, nsid, XNVME_SPEC_IO_MGMT_RECV_RUHS, 0, &header, header_bytes);

	// --- DEBUG PRINT: What does the NVMe controller actually report? ---
	duckdb::Printer::Print(StringUtil::Format("[FDP Debug] Hardware reported nruhsd (0-based) = %d", header.nruhsd));

	// Fix: +1 to get the true count for the loop limit
	uint16_t max_placement_handles = header.nruhsd;
	duckdb::Printer::Print(StringUtil::Format("[FDP Debug] Parsing %d handles...", max_placement_handles));

	struct xnvme_spec_ruhs *ruhs = nullptr;
	uint32_t ruhs_nbytes = sizeof(*ruhs) + max_placement_handles * sizeof(struct xnvme_spec_ruhs_desc);
	ruhs = (struct xnvme_spec_ruhs *)xnvme_buf_alloc(device, ruhs_nbytes);
	memset(ruhs, 0, ruhs_nbytes);
	xnvme_nvm_mgmt_recv(&xnvme_ctx, nsid, XNVME_SPEC_IO_MGMT_RECV_RUHS, 0, ruhs, ruhs_nbytes);

	for (int i = 0; i < max_placement_handles; ++i) {
		placement_handlers.emplace_back(ruhs->desc[i].pi);
		duckdb::Printer::Print(StringUtil::Format("[FDP Debug] placement_handlers[%d] = PI %d", i, ruhs->desc[i].pi));
	}

	xnvme_buf_free(device, ruhs);
}

idx_t NvmeDevice::GetThreadIndex() {
	if (!index.IsValid()) {
		index = thread_id_counter++ % max_threads;
	}

	return index.GetIndex();
}
} // namespace duckdb
