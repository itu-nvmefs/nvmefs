#pragma once

#include "nvme_io_engine.hpp"
#include "nvme_device.hpp"
#include <future>
#include <array>

namespace duckdb {

struct PrefetchState {
	bool active = false;
	idx_t expected_lba = 0;
	idx_t expected_nr_lbas = 0;
	idx_t alloc_size = 0;
	nvme_buf_ptr buffer = nullptr;
	std::promise<void> *promise = nullptr;
	std::future<void> future;
};

class NvmeAsynPrefetchIOEngine : public NvmeIOEngine {
private:
	std::array<PrefetchState, 256> prefetches;

	void DrainPrefetch(idx_t thread_index, xnvme_queue *queue) {
		PrefetchState &prefetch = prefetches[thread_index];
		if (prefetch.active) {
			std::future_status status;
			std::chrono::milliseconds interval(0);
			do {
				xnvme_queue_poke(queue, 0);
				status = prefetch.future.wait_for(interval);
			} while (status != std::future_status::ready);

			device.FreeDeviceBuffer(prefetch.buffer, prefetch.alloc_size);
			delete prefetch.promise;
			prefetch.active = false;
		}
	}

public:
	using NvmeIOEngine::NvmeIOEngine;

	void Read(void *buffer, const NvmeCmdContext &ctx) override {
		idx_t alloc_size = ctx.nr_lbas * device.geometry.lba_size;
		uint32_t nsid = xnvme_dev_get_nsid(device.device);
		uint16_t ruh = device.GetReclaimUnitHandleOrDefault(ctx.filepath);

		idx_t thread_index = device.GetThreadIndex();
		xnvme_queue *queue = device.queues[thread_index];

		if (!queue) {
			int err = xnvme_queue_init(device.device, XNVME_QUEUE_DEPTH, 0, &device.queues[thread_index]);
			if (err) {
				device.FreeDeviceBuffer(device.AllocateDeviceBuffer(alloc_size), alloc_size);
				xnvme_cli_perr("Unable to create a queue for asynchronous IO", err);
				throw IOException("Unable to create queue");
			}
			queue = device.queues[thread_index];
		}

		PrefetchState &prefetch = prefetches[thread_index];
		bool data_fulfilled = false;
		std::chrono::milliseconds interval(0);

		if (prefetch.active) {
			std::future_status status;
			do {
				xnvme_queue_poke(queue, 0);
				status = prefetch.future.wait_for(interval);
			} while (status != std::future_status::ready);

			if (ctx.start_lba == prefetch.expected_lba && ctx.nr_lbas == prefetch.expected_nr_lbas) {
				memcpy(buffer, (char *)prefetch.buffer + ctx.offset, ctx.nr_bytes);
				data_fulfilled = true;
			}

			device.FreeDeviceBuffer(prefetch.buffer, prefetch.alloc_size);
			delete prefetch.promise;
			prefetch.active = false;
		}

		if (!data_fulfilled) {
			nvme_buf_ptr dev_buffer = device.AllocateDeviceBuffer(alloc_size);
			xnvme_cmd_ctx *xnvme_ctx = xnvme_queue_get_cmd_ctx(queue);
			device.PrepareIOCmdContext(xnvme_ctx, ctx, ruh, 0, false);

			std::promise<void> cb_notify;
			std::future<void> fut = cb_notify.get_future();

			xnvme_cmd_ctx_set_cb(xnvme_ctx, device.CommandCallback, &cb_notify);

			int err = xnvme_nvm_read(xnvme_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, dev_buffer, nullptr);
			if (err) {
				device.FreeDeviceBuffer(dev_buffer, alloc_size);
				xnvme_cli_perr("Could not submit command to queue with xnvme_nvme_read(): ", err);
				throw IOException("Encountered error when reading from NVMe device");
			}

			std::future_status status;
			do {
				xnvme_queue_poke(queue, 0);
				status = fut.wait_for(interval);
			} while (status != std::future_status::ready);

			memcpy(buffer, (char *)dev_buffer + ctx.offset, ctx.nr_bytes);
			device.FreeDeviceBuffer(dev_buffer, alloc_size);
		}

		idx_t next_lba = ctx.start_lba + ctx.nr_lbas;

		prefetch.alloc_size = alloc_size;
		prefetch.expected_lba = next_lba;
		prefetch.expected_nr_lbas = ctx.nr_lbas;
		prefetch.buffer = device.AllocateDeviceBuffer(prefetch.alloc_size);
		prefetch.promise = new std::promise<void>();
		prefetch.future = prefetch.promise->get_future();
		prefetch.active = true;

		xnvme_cmd_ctx *prefetch_ctx = xnvme_queue_get_cmd_ctx(queue);

		NvmeCmdContext next_ctx = ctx;
		next_ctx.start_lba = next_lba;
		device.PrepareIOCmdContext(prefetch_ctx, next_ctx, ruh, 0, false);

		xnvme_cmd_ctx_set_cb(prefetch_ctx, device.CommandCallback, prefetch.promise);

		int err = xnvme_nvm_read(prefetch_ctx, nsid, next_lba, ctx.nr_lbas - 1, prefetch.buffer, nullptr);
		if (err) {
			device.FreeDeviceBuffer(prefetch.buffer, prefetch.alloc_size);
			delete prefetch.promise;
			prefetch.active = false;
		}
	}

	void Write(void *buffer, const NvmeCmdContext &ctx) override {
		idx_t alloc_size = ctx.nr_lbas * device.geometry.lba_size;
		nvme_buf_ptr dev_buffer = device.AllocateDeviceBuffer(alloc_size);

		idx_t thread_index = device.GetThreadIndex();
		xnvme_queue *queue = device.queues[thread_index];

		if (!queue) {
			int err = xnvme_queue_init(device.device, XNVME_QUEUE_DEPTH, 0, &device.queues[thread_index]);
			if (err) {
				device.FreeDeviceBuffer(dev_buffer, alloc_size);
				xnvme_cli_perr("Unable to create an queue for asynchronous IO", err);
				throw IOException("Unable to create queue");
			}
			queue = device.queues[thread_index];
		}

		DrainPrefetch(thread_index, queue);

		if (ctx.offset > 0 || ctx.nr_bytes < alloc_size) {
			uint32_t nsid = xnvme_dev_get_nsid(device.device);
			xnvme_cmd_ctx read_ctx = xnvme_cmd_ctx_from_dev(device.device);
			read_ctx.cmd.common.cdw12 = ctx.nr_lbas - 1;

			int err = xnvme_nvm_read(&read_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, dev_buffer, nullptr);
			if (err) {
				device.FreeDeviceBuffer(dev_buffer, alloc_size);
				throw IOException("Read-modify-write failed in WriteAsync");
			}
		}

		memcpy((char *)dev_buffer + ctx.offset, buffer, ctx.nr_bytes);

		uint32_t nsid = xnvme_dev_get_nsid(device.device);
		uint16_t ruh = device.GetReclaimUnitHandleOrDefault(ctx.filepath);
		xnvme_cmd_ctx *xnvme_ctx = xnvme_queue_get_cmd_ctx(queue);
		device.PrepareIOCmdContext(xnvme_ctx, ctx, ruh, DATA_PLACEMENT_MODE, true);

		std::promise<void> cb_notify;
		std::future<void> fut = cb_notify.get_future();

		xnvme_cmd_ctx_set_cb(xnvme_ctx, device.CommandCallback, &cb_notify);

		std::future_status status;
		std::chrono::milliseconds interval = std::chrono::milliseconds(0);

		int err = xnvme_nvm_write(xnvme_ctx, nsid, ctx.start_lba, ctx.nr_lbas - 1, dev_buffer, nullptr);
		if (err) {
			device.FreeDeviceBuffer(dev_buffer, alloc_size);
			xnvme_cli_perr("Could not submit command to queue with xnvme_nvme_write(): ", err);
			throw IOException("Encountered error when writing to NVMe device");
		}

		do {
			xnvme_queue_poke(queue, 0);
			status = fut.wait_for(interval);
		} while (status != std::future_status::ready);

		device.FreeDeviceBuffer(dev_buffer, alloc_size);
	}
};
} // namespace duckdb
