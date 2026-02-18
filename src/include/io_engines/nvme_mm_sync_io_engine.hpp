#pragma once

#include "nvme_io_engine.hpp"
#include "nvme_device.hpp"

namespace duckdb {

class NvmeMMSyncIOEngine : public NvmeIOEngine {
public:
	using NvmeIOEngine::NvmeIOEngine;

	void Read(void *buffer, const NvmeCmdContext &ctx) override {

		idx_t alloc_size = ctx.nr_lbas * device.geometry.lba_size;

		bool is_aligned = (ctx.offset == 0 && ctx.nr_bytes == alloc_size);
		bool is_dma_safe = device.memory_manager->IsManaged(buffer, ctx.nr_bytes);
		uint32_t nsid = xnvme_dev_get_nsid(device.device);
		xnvme_cmd_ctx xnvme_ctx = xnvme_cmd_ctx_from_dev(device.device);
		uint8_t plid_idx = device.GetPlacementIdentifierOrDefault(ctx.filepath);

		device.PrepareIOCmdContext(&xnvme_ctx, ctx, plid_idx, 0, false);

		if (is_aligned && is_dma_safe) {
			int err = xnvme_nvm_read(&xnvme_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, buffer, nullptr);
			if (err) {
				xnvme_cli_perr("xnvme_nvm_read failed (Zero-Copy)", err);
				throw IOException("Read failed");
			}
			return;
		}

		// SLOW PATH: Bounce Buffer
		nvme_buf_ptr dev_buffer = device.AllocateDeviceBuffer(alloc_size);

		int err = xnvme_nvm_read(&xnvme_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, dev_buffer, nullptr);
		if (err) {
			device.FreeDeviceBuffer(dev_buffer, alloc_size);
			xnvme_cli_perr("xnvme_nvm_read failed", err);
			throw IOException("Read failed");
		}

		memcpy(buffer, (char *)dev_buffer + ctx.offset, ctx.nr_bytes);
		device.FreeDeviceBuffer(dev_buffer, alloc_size);
	}

	void Write(void *buffer, const NvmeCmdContext &ctx) override {
		idx_t alloc_size = ctx.nr_lbas * device.geometry.lba_size;

		// --- ZERO COPY CHECK ---
		// 1. Is this a full block write? (No offsets, full size)
		// 2. Is the buffer one of ours? (DMA capable)
		bool is_aligned = (ctx.offset == 0 && ctx.nr_bytes == alloc_size);
		bool is_dma_safe = device.memory_manager->IsManaged(buffer, ctx.nr_bytes);

		uint32_t nsid = xnvme_dev_get_nsid(device.device);
		xnvme_cmd_ctx xnvme_ctx = xnvme_cmd_ctx_from_dev(device.device);

		if (is_aligned && is_dma_safe) {
			// FAST PATH: Zero-Copy
			uint8_t plid_idx = device.GetPlacementIdentifierOrDefault(ctx.filepath);

			device.PrepareIOCmdContext(&xnvme_ctx, ctx, plid_idx, DATA_PLACEMENT_MODE, true);

			int err = xnvme_nvm_write(&xnvme_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, buffer, nullptr);

			if (err) {
				xnvme_cli_perr("xnvme_nvm_write failed (Zero-Copy)", err);
				throw IOException("Write failed");
			}

			return;
		}
		// -----------------------

		// SLOW PATH: Bounce Buffer (Original Logic)
		nvme_buf_ptr dev_buffer = device.AllocateDeviceBuffer(alloc_size);

		if (ctx.offset > 0 || ctx.nr_bytes < alloc_size) {
			// Partial write logic (Read-Modify-Write)
			xnvme_cmd_ctx read_ctx = xnvme_cmd_ctx_from_dev(device.device);
			read_ctx.cmd.common.cdw12 = ctx.nr_lbas - 1;

			int err = xnvme_nvm_read(&read_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, dev_buffer, nullptr);
			if (err) {
				device.FreeDeviceBuffer(dev_buffer, alloc_size);
				xnvme_cli_perr("xnvme_nvm_write failed", err);
				throw IOException("Read-modify-write failed");
			}
		}

		memcpy((char *)dev_buffer + ctx.offset, buffer, ctx.nr_bytes);

		uint8_t plid_idx = device.GetPlacementIdentifierOrDefault(ctx.filepath);

		device.PrepareIOCmdContext(&xnvme_ctx, ctx, plid_idx, DATA_PLACEMENT_MODE, true);

		int err = xnvme_nvm_write(&xnvme_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, dev_buffer, nullptr);
		if (err) {
			device.FreeDeviceBuffer(dev_buffer, alloc_size);
			xnvme_cli_perr("xnvme_nvm_write failed", err);
			throw IOException("Write failed");
		}

		device.FreeDeviceBuffer(dev_buffer, alloc_size);
	}
};
} // namespace duckdb