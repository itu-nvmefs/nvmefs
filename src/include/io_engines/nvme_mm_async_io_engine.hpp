#pragma once

#include "nvme_io_engine.hpp"
#include "nvme_device.hpp"

namespace duckdb {

class NvmeMMAsyncIOEngine : public NvmeIOEngine {
public:
	using NvmeIOEngine::NvmeIOEngine;

	void Read(void *buffer, const NvmeCmdContext &ctx) override {
		idx_t alloc_size = ctx.nr_lbas * device.geometry.lba_size;

		// --- ZERO COPY CHECK ---
		// Check if the buffer is aligned to LBA size and managed by our memory manager (DMA safe)
		bool is_aligned = (ctx.offset == 0 && ctx.nr_bytes == alloc_size);
		bool is_dma_safe = device.memory_manager->IsManaged(buffer, ctx.nr_bytes);
		bool use_zero_copy = is_aligned && is_dma_safe;

		void *io_buffer = nullptr;

		if (use_zero_copy) {
			// FAST PATH: Use user buffer directly
			io_buffer = buffer;
		} else {
			// SLOW PATH: Allocate bounce buffer
			io_buffer = device.AllocateDeviceBuffer(alloc_size);
		}

		uint32_t nsid = xnvme_dev_get_nsid(device.device);
		uint8_t plid_idx = device.GetPlacementIdentifierOrDefault(ctx.filepath);

		xnvme_queue *queue = device.GetQueue();
		xnvme_cmd_ctx *xnvme_ctx = xnvme_queue_get_cmd_ctx(queue);
		device.PrepareIOCmdContext(xnvme_ctx, ctx, plid_idx, 0, false);

		std::promise<void> cb_notify;
		std::future<void> fut = cb_notify.get_future();

		xnvme_cmd_ctx_set_cb(xnvme_ctx, device.CommandCallback, &cb_notify);

		std::future_status status;
		std::chrono::milliseconds interval = std::chrono::milliseconds(0);

		int err = xnvme_nvm_read(xnvme_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, io_buffer, nullptr);
		if (err) {
			if (!use_zero_copy) {
				device.FreeDeviceBuffer(io_buffer, alloc_size);
			}
			xnvme_cli_perr("Could not submit command to queue with xnvme_nvme_read(): ", err);
			throw IOException("Encountered error when reading from NVMe device");
		}

		do {
			xnvme_queue_poke(queue, 0);
			status = fut.wait_for(interval);
		} while (status != std::future_status::ready);

		if (!use_zero_copy) {
			// Copy from aligned bounce buffer to user buffer
			memcpy(buffer, (char *)io_buffer + ctx.offset, ctx.nr_bytes);
			device.FreeDeviceBuffer(io_buffer, alloc_size);
		}
	}

	void Write(void *buffer, const NvmeCmdContext &ctx) override {
		idx_t alloc_size = ctx.nr_lbas * device.geometry.lba_size;

		// --- ZERO COPY CHECK ---
		// Check if the buffer is aligned to LBA size and managed by our memory manager (DMA safe)
		bool is_aligned = (ctx.offset == 0 && ctx.nr_bytes == alloc_size);
		bool is_dma_safe = device.memory_manager->IsManaged(buffer, ctx.nr_bytes);
		bool use_zero_copy = is_aligned && is_dma_safe;

		void *io_buffer = nullptr;

		if (use_zero_copy) {
			// FAST PATH: Use user buffer directly
			io_buffer = buffer;
		} else {
			// SLOW PATH: Allocate bounce buffer
			io_buffer = device.AllocateDeviceBuffer(alloc_size);

			if (!io_buffer) {
				throw IOException("Failed to allocate NVMe bounce buffer");
			}

			// Read-Modify-Write logic for partial blocks
			if (ctx.offset > 0 || ctx.nr_bytes < alloc_size) {
				uint32_t nsid = xnvme_dev_get_nsid(device.device);
				xnvme_cmd_ctx read_ctx = xnvme_cmd_ctx_from_dev(device.device);
				read_ctx.cmd.common.cdw12 = ctx.nr_lbas - 1;

				// Synchronous read for RMW
				int err = xnvme_nvm_read(&read_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, io_buffer, nullptr);
				if (err) {
					device.FreeDeviceBuffer(io_buffer, alloc_size);
					throw IOException("Read-modify-write failed in Write");
				}
			}

			// Copy user data to the aligned buffer at the correct offset
			memcpy((char *)io_buffer + ctx.offset, buffer, ctx.nr_bytes);
		}

		uint32_t nsid = xnvme_dev_get_nsid(device.device);
		uint8_t plid_idx = device.GetPlacementIdentifierOrDefault(ctx.filepath);

		xnvme_queue *queue = device.GetQueue();
		xnvme_cmd_ctx *xnvme_ctx = xnvme_queue_get_cmd_ctx(queue);
		device.PrepareIOCmdContext(xnvme_ctx, ctx, plid_idx, DATA_PLACEMENT_MODE, true);

		std::promise<void> cb_notify;
		std::future<void> fut = cb_notify.get_future();

		xnvme_cmd_ctx_set_cb(xnvme_ctx, device.CommandCallback, &cb_notify);

		std::future_status status;
		std::chrono::milliseconds interval = std::chrono::milliseconds(0);

		int err = xnvme_nvm_write(xnvme_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, io_buffer, nullptr);
		if (err) {
			if (io_buffer) {
				device.FreeDeviceBuffer(io_buffer, alloc_size);
			}

			xnvme_cli_perr("Could not submit command to queue with xnvme_nvme_write(): ", err);
			throw IOException("Encountered error when writing to NVMe device");
		}

		do {
			xnvme_queue_poke(queue, 0);
			status = fut.wait_for(interval);
		} while (status != std::future_status::ready);

		if (!use_zero_copy && io_buffer) {
			device.FreeDeviceBuffer(io_buffer, alloc_size);
		}
	}
};
} // namespace duckdb