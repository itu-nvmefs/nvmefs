#pragma once

#include "nvme_io_engine.hpp"
#include "nvme_device.hpp"

namespace duckdb {

class NvmeSyncIOEngine : public NvmeIOEngine {
public:
	using NvmeIOEngine::NvmeIOEngine;

	void Read(void *buffer, const NvmeCmdContext &ctx) override {
		idx_t alloc_size = ctx.nr_lbas * device.geometry.lba_size;
		nvme_buf_ptr dev_buffer = device.AllocateDeviceBuffer(alloc_size);

		uint32_t nsid = xnvme_dev_get_nsid(device.device);
		uint8_t plid_idx = device.GetPlacementIdentifierOrDefault(ctx.filepath);
		xnvme_cmd_ctx xnvme_ctx = xnvme_cmd_ctx_from_dev(device.device);

		device.PrepareIOCmdContext(&xnvme_ctx, ctx, plid_idx, 0, false);

		int err = xnvme_nvm_read(&xnvme_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, dev_buffer, nullptr);

		if (err) {
			device.FreeDeviceBuffer(dev_buffer, alloc_size);
			xnvme_cli_perr("Could not write to device with xnvme_nvme_write(): ", err);
			throw IOException("Encountered error when writing to NVMe device");
		}

		memcpy(buffer, (char *)dev_buffer + ctx.offset, ctx.nr_bytes);

		device.FreeDeviceBuffer(dev_buffer, alloc_size);
	}

	void Write(void *buffer, const NvmeCmdContext &ctx) override {
		// FIX: Allocate based on LBA size
		idx_t alloc_size = ctx.nr_lbas * device.geometry.lba_size;
		nvme_buf_ptr dev_buffer = device.AllocateDeviceBuffer(alloc_size);

		if (ctx.offset > 0 || ctx.nr_bytes < alloc_size) {
			// Partial block write: We must read the existing block first to preserve surrounding data
			// NOTE: We cannot use 'Read' here recursively if we want to avoid extra allocs,
			// but for simplicity, let's just ensure we fill the buffer with existing data.

			// Manual read to avoid recursion loop or just use the raw xnvme call:
			uint32_t nsid = xnvme_dev_get_nsid(device.device);
			xnvme_cmd_ctx read_ctx = xnvme_cmd_ctx_from_dev(device.device);
			// Prepare read ctx... (simplified)
			read_ctx.cmd.common.cdw12 = ctx.nr_lbas - 1;

			int err = xnvme_nvm_read(&read_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, dev_buffer, nullptr);
			if (err) {
				device.FreeDeviceBuffer(dev_buffer, alloc_size);
				throw IOException("Read-modify-write failed");
			}
		}

		// Copy user data into the aligned buffer at the correct offset
		memcpy((char *)dev_buffer + ctx.offset, buffer, ctx.nr_bytes);

		uint32_t nsid = xnvme_dev_get_nsid(device.device);
		uint8_t plid_idx = device.GetPlacementIdentifierOrDefault(ctx.filepath);
		xnvme_cmd_ctx xnvme_ctx = xnvme_cmd_ctx_from_dev(device.device);

		device.PrepareIOCmdContext(&xnvme_ctx, ctx, plid_idx, DATA_PLACEMENT_MODE, true);

		int err = xnvme_nvm_write(&xnvme_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, dev_buffer, nullptr);
		if (err) {
			device.FreeDeviceBuffer(dev_buffer, alloc_size);
			xnvme_cli_perr("Could not write to device with xnvme_nvme_write(): ", err);
			throw IOException("Encountered error when writing to NVMe device");
		}

		device.FreeDeviceBuffer(dev_buffer, alloc_size);
	}
};
} // namespace duckdb