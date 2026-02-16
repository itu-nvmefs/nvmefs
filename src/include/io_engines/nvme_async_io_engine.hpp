#pragma once

#include "nvme_io_engine.hpp"
#include "nvme_device.hpp"

namespace duckdb {

class NvmeAsyncIOEngine : public NvmeIOEngine {
public:
	using NvmeIOEngine::NvmeIOEngine;

	void Read(void *buffer, const NvmeCmdContext &context) override {
		const NvmeCmdContext &ctx = static_cast<const NvmeCmdContext &>(context);
		D_ASSERT(ctx.nr_lbas > 0);
		// We only support offset reads within a single block
		D_ASSERT((ctx.offset == 0 && ctx.nr_lbas > 1) || (ctx.offset >= 0 && ctx.nr_lbas == 1));

		// Allocate based on LBA size (aligned), not user request size
		// Using nr_bytes causes buffer overflow if it is smaller than the full LBA size
		idx_t alloc_size = ctx.nr_lbas * device.geometry.lba_size;
		nvme_buf_ptr dev_buffer = device.AllocateDeviceBuffer(alloc_size);

		uint32_t nsid = xnvme_dev_get_nsid(device.device);
		uint8_t plid_idx = device.GetPlacementIdentifierOrDefault(ctx.filepath);

		idx_t thread_index = device.GetThreadIndex();
		xnvme_queue *queue = device.queues[thread_index];

		if (!queue) {
			int err = xnvme_queue_init(device.device, XNVME_QUEUE_DEPTH, 0, &device.queues[thread_index]);
			if (err) {
				device.FreeDeviceBuffer(dev_buffer);
				xnvme_cli_perr("Unable to create an queue for asynchronous IO", err);
				throw IOException("Unable to create queue");
			}
			queue = device.queues[thread_index];
		}

		xnvme_cmd_ctx *xnvme_ctx = xnvme_queue_get_cmd_ctx(queue);
		device.PrepareIOCmdContext(xnvme_ctx, context, plid_idx, 0, false);

		std::promise<void> cb_notify;
		std::future<void> fut = cb_notify.get_future();

		xnvme_cmd_ctx_set_cb(xnvme_ctx, device.CommandCallback, &cb_notify);

		std::future_status status;
		std::chrono::milliseconds interval = std::chrono::milliseconds(0);

		int err = xnvme_nvm_read(xnvme_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, dev_buffer, nullptr);
		if (err) {
			device.FreeDeviceBuffer(dev_buffer);
			xnvme_cli_perr("Could not submit command to queue with xnvme_nvme_read(): ", err);
			throw IOException("Encountered error when reading from NVMe device");
		}

		do {
			xnvme_queue_poke(queue, 0);
			status = fut.wait_for(interval);
		} while (status != std::future_status::ready);

		memcpy(buffer, (char *)dev_buffer + ctx.offset, ctx.nr_bytes);

		device.FreeDeviceBuffer(dev_buffer);
	}

	void Write(void *buffer, const NvmeCmdContext &context) override {
		const NvmeCmdContext &ctx = static_cast<const NvmeCmdContext &>(context);
		D_ASSERT(ctx.nr_lbas > 0);
		D_ASSERT((ctx.offset == 0 && ctx.nr_lbas > 1) || (ctx.offset >= 0 && ctx.nr_lbas == 1));

		//  Allocate based on LBA size
		idx_t alloc_size = ctx.nr_lbas * device.geometry.lba_size;
		nvme_buf_ptr dev_buffer = device.AllocateDeviceBuffer(alloc_size);

		// Read-Modify-Write logic for partial blocks (Copied from synchronous Write)
		if (ctx.offset > 0 || ctx.nr_bytes < alloc_size) {
			uint32_t nsid = xnvme_dev_get_nsid(device.device);
			xnvme_cmd_ctx read_ctx = xnvme_cmd_ctx_from_dev(device.device);
			read_ctx.cmd.common.cdw12 = ctx.nr_lbas - 1;

			// We do a synchronous read here to ensure the buffer is populated before we modify it
			int err = xnvme_nvm_read(&read_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, dev_buffer, nullptr);
			if (err) {
				device.FreeDeviceBuffer(dev_buffer);
				throw IOException("Read-modify-write failed in WriteAsync");
			}
		}

		// Copy user data to the aligned buffer at the correct offset
		memcpy((char *)dev_buffer + ctx.offset, buffer, ctx.nr_bytes);

		uint32_t nsid = xnvme_dev_get_nsid(device.device);
		uint8_t plid_idx = device.GetPlacementIdentifierOrDefault(ctx.filepath);

		idx_t thread_index = device.GetThreadIndex();
		xnvme_queue *queue = device.queues[thread_index];

		if (!queue) {
			int err = xnvme_queue_init(device.device, XNVME_QUEUE_DEPTH, 0, &device.queues[thread_index]);
			if (err) {
				device.FreeDeviceBuffer(dev_buffer);
				xnvme_cli_perr("Unable to create an queue for asynchronous IO", err);
				throw IOException("Unable to create queue");
			}
			queue = device.queues[thread_index];
		}

		xnvme_cmd_ctx *xnvme_ctx = xnvme_queue_get_cmd_ctx(queue);
		device.PrepareIOCmdContext(xnvme_ctx, context, plid_idx, DATA_PLACEMENT_MODE, true);

		std::promise<void> cb_notify;
		std::future<void> fut = cb_notify.get_future();

		xnvme_cmd_ctx_set_cb(xnvme_ctx, device.CommandCallback, &cb_notify);

		std::future_status status;
		std::chrono::milliseconds interval = std::chrono::milliseconds(0);

		int err = xnvme_nvm_write(xnvme_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, dev_buffer, nullptr);
		if (err) {
			device.FreeDeviceBuffer(dev_buffer);
			xnvme_cli_perr("Could not submit command to queue with xnvme_nvme_write(): ", err);
			throw IOException("Encountered error when writing to NVMe device");
		}

		do {
			xnvme_queue_poke(queue, 0);
			status = fut.wait_for(interval);
		} while (status != std::future_status::ready);

		device.FreeDeviceBuffer(dev_buffer);
	}
};
} // namespace duckdb