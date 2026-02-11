#include "nvme_device.hpp"
#include "nvme_io_engine.hpp"
#include "io_engines/nvme_async_io_engine.hpp"
#include "io_engines/nvme_sync_io_engine.hpp"

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

	memory_manager = make_uniq<NvmeMemoryManager>(device);

	fdp = CheckFDP();

	if (fdp) {
		InitializePlacementHandles();
	}

	if (StringUtil::Contains(config.meta, "sync_writer")) {
		duckdb::Printer::Print("[nvmefs] Using synchronous IO engine for writes");
		io_engine = make_uniq<NvmeSyncIOEngine>(*this);
	} else {
		duckdb::Printer::Print("[nvmefs] Using asynchronous IO engine for writes");
		io_engine = make_uniq<NvmeAsyncIOEngine>(*this);
	}

	GetThreadIndex();
	allocated_placement_identifiers["nvmefs:///tmp"] = 1;
	geometry = LoadDeviceGeometry();
}

NvmeDevice::~NvmeDevice() {
	if (memory_manager) {
        memory_manager.reset();
    }

	for (const auto &queue : queues) {
		xnvme_queue_term(queue);
	}
	
	xnvme_dev_close(device);
}

DeviceGeometry NvmeDevice::GetDeviceGeometry() {
	return geometry;
}

uint8_t NvmeDevice::GetPlacementIdentifierOrDefault(const string &path) {
	uint8_t placement_identifier = 0;
	for (const auto &kv : allocated_placement_identifiers) {
		if (StringUtil::StartsWith(path, kv.first)) {
			placement_identifier = kv.second;
		}
	}

	return placement_identifier;
}

nvme_buf_ptr NvmeDevice::AllocateDeviceBuffer(idx_t nr_bytes) {
	return memory_manager->Allocate(nr_bytes);
}

void NvmeDevice::FreeDeviceBuffer(nvme_buf_ptr buffer) {
	memory_manager->Free(buffer);
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

	// Specified by the command set specification:
	// https://nvmexpress.org/wp-content/uploads/NVM-Express-NVM-Command-Set-Specification-Revision-1.1-2024.08.05-Ratified.pdf
	// cdw12 specifies data placement (dtype) and number of lbas to write/read (0 indexed)
	// cdw13 hold placement handle id in bit range 16-31
	uint16_t nr_lbas = nvme_cmd_ctx.nr_lbas - 1;

	ctx->cmd.common.cdw12 = nr_lbas;
	if (write && fdp) {
		ctx->cmd.common.cdw12 |= dtype << 20;

		uint16_t phid = placement_handlers[plid_idx];
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

	// Retrieve number of RUHs on the device
	struct xnvme_spec_ruhs header;
	uint32_t header_bytes = sizeof(header);
	xnvme_nvm_mgmt_recv(&xnvme_ctx, nsid, XNVME_SPEC_IO_MGMT_RECV_RUHS, 0, &header, header_bytes);
	uint16_t max_placement_handles = header.nruhsd - 1;

	// Retrieve information about reclaim unit handles
	struct xnvme_spec_ruhs *ruhs = nullptr;
	uint32_t ruhs_nbytes = sizeof(*ruhs) + max_placement_handles * sizeof(struct xnvme_spec_ruhs_desc);
	ruhs = (struct xnvme_spec_ruhs *)xnvme_buf_alloc(device, ruhs_nbytes);
	memset(ruhs, 0, ruhs_nbytes);
	xnvme_nvm_mgmt_recv(&xnvme_ctx, nsid, XNVME_SPEC_IO_MGMT_RECV_RUHS, 0, ruhs, ruhs_nbytes);

	for (int i = 0; i < max_placement_handles; ++i) {
		placement_handlers.emplace_back(ruhs->desc[i].pi);
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
