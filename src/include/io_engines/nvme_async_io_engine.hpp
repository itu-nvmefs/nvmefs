#pragma once

#include "nvme_io_engine.hpp"
#include "nvme_device.hpp"
#include <chrono>
#include <future>
#include <vector>

namespace duckdb {

class NvmeAsyncIOEngine : public NvmeIOEngine {
public:
	using NvmeIOEngine::NvmeIOEngine;

	void Read(void *buffer, const NvmeCmdContext &ctx) override {
		uint32_t nsid = xnvme_dev_get_nsid(device.device);
		idx_t alloc_size = ctx.nr_lbas * device.geometry.lba_size;

		void *dev_buffer = device.AllocateDeviceBuffer(alloc_size);
		if (!dev_buffer) {
			throw IOException("Failed to allocate NVMe DMA buffer");
		}

		// Calculate MDTS/4K-clamped chunk size to stay within controller limits
		const xnvme_geo *geo = xnvme_dev_get_geo(device.device);
		uint32_t max_lbas = geo->mdts_nbytes > 0 ? geo->mdts_nbytes / device.geometry.lba_size : 127;
		uint32_t lbas_per_4k = 4096 / device.geometry.lba_size;
		if (lbas_per_4k > 0) {
			max_lbas = (max_lbas / lbas_per_4k) * lbas_per_4k;
		}
		if (max_lbas == 0) {
			max_lbas = lbas_per_4k;
		}

		uint16_t ruh = device.GetReclaimUnitHandleOrDefault(ctx.filepath);
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

		uint32_t lbas_left = ctx.nr_lbas;
		uint64_t slba = ctx.start_lba;
		uint32_t buf_offset = 0;

		size_t num_chunks = (ctx.nr_lbas + max_lbas - 1) / max_lbas;
		std::vector<std::promise<void>> promises(num_chunks);
		std::vector<std::future<void>> futures;
		futures.reserve(num_chunks);
		for (auto &p : promises) {
			futures.push_back(p.get_future());
		}

		size_t chunk_idx = 0;
		while (lbas_left > 0) {
			uint32_t chunk = std::min(lbas_left, max_lbas);

			xnvme_cmd_ctx *xnvme_ctx = xnvme_queue_get_cmd_ctx(queue);
			device.PrepareIOCmdContext(xnvme_ctx, ctx, ruh, 0, false);
			xnvme_cmd_ctx_set_cb(xnvme_ctx, device.CommandCallback, &promises[chunk_idx]);

			int err = xnvme_nvm_read(xnvme_ctx, nsid, slba, chunk - 1, (char *)dev_buffer + buf_offset, nullptr);
			if (err) {
				device.FreeDeviceBuffer(dev_buffer, alloc_size);
				xnvme_cli_perr("Could not submit command to queue with xnvme_nvme_read(): ", err);
				throw IOException("Encountered error when reading from NVMe device");
			}

			lbas_left -= chunk;
			slba += chunk;
			buf_offset += (chunk * device.geometry.lba_size);
			chunk_idx++;
		}

		std::chrono::milliseconds interval = std::chrono::milliseconds(0);
		for (auto &fut : futures) {
			std::future_status status;
			do {
				xnvme_queue_poke(queue, 0);
				status = fut.wait_for(interval);
			} while (status != std::future_status::ready);
		}

		memcpy(buffer, (char *)dev_buffer + ctx.offset, ctx.nr_bytes);
		device.FreeDeviceBuffer(dev_buffer, alloc_size);
	}

	void Write(void *buffer, const NvmeCmdContext &ctx) override {
		uint32_t nsid = xnvme_dev_get_nsid(device.device);
		idx_t alloc_size = ctx.nr_lbas * device.geometry.lba_size;

		void *dev_buffer = device.AllocateDeviceBuffer(alloc_size);
		if (!dev_buffer) {
			throw IOException("Failed to allocate NVMe DMA buffer");
		}

		const xnvme_geo *geo = xnvme_dev_get_geo(device.device);
		uint32_t max_lbas = geo->mdts_nbytes > 0 ? geo->mdts_nbytes / device.geometry.lba_size : 127;
		uint32_t lbas_per_4k = 4096 / device.geometry.lba_size;
		if (lbas_per_4k > 0) {
			max_lbas = (max_lbas / lbas_per_4k) * lbas_per_4k;
		}
		if (max_lbas == 0) {
			max_lbas = lbas_per_4k;
		}

		bool needs_rmw = ctx.offset > 0 || ctx.nr_bytes < alloc_size;
		if (needs_rmw) {
			uint32_t lbas_left = ctx.nr_lbas;
			uint64_t slba = ctx.start_lba;
			uint32_t buf_offset = 0;

			while (lbas_left > 0) {
				uint32_t chunk = std::min(lbas_left, max_lbas);
				xnvme_cmd_ctx sync_ctx = xnvme_cmd_ctx_from_dev(device.device);

				int err = xnvme_nvm_read(&sync_ctx, nsid, slba, chunk - 1, (char *)dev_buffer + buf_offset, nullptr);
				if (err) {
					device.FreeDeviceBuffer(dev_buffer, alloc_size);
					throw IOException("Read-modify-write chunk failed");
				}
				lbas_left -= chunk;
				slba += chunk;
				buf_offset += (chunk * device.geometry.lba_size);
			}
		}

		memcpy((char *)dev_buffer + ctx.offset, buffer, ctx.nr_bytes);

		uint16_t ruh = device.GetReclaimUnitHandleOrDefault(ctx.filepath);
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

		uint32_t lbas_left = ctx.nr_lbas;
		uint64_t slba = ctx.start_lba;
		uint32_t buf_offset = 0;

		size_t num_chunks = (ctx.nr_lbas + max_lbas - 1) / max_lbas;
		std::vector<std::promise<void>> promises(num_chunks);
		std::vector<std::future<void>> futures;
		futures.reserve(num_chunks);
		for (auto &p : promises) {
			futures.push_back(p.get_future());
		}

		size_t chunk_idx = 0;
		while (lbas_left > 0) {
			uint32_t chunk = std::min(lbas_left, max_lbas);

			xnvme_cmd_ctx *xnvme_ctx = xnvme_queue_get_cmd_ctx(queue);
			device.PrepareIOCmdContext(xnvme_ctx, ctx, ruh, DATA_PLACEMENT_MODE, true);
			xnvme_cmd_ctx_set_cb(xnvme_ctx, device.CommandCallback, &promises[chunk_idx]);

			int err = xnvme_nvm_write(xnvme_ctx, nsid, slba, chunk - 1, (char *)dev_buffer + buf_offset, nullptr);
			if (err) {
				device.FreeDeviceBuffer(dev_buffer, alloc_size);
				xnvme_cli_perr("Could not submit command to queue with xnvme_nvme_write(): ", err);
				throw IOException("Encountered error when writing to NVMe device");
			}

			lbas_left -= chunk;
			slba += chunk;
			buf_offset += (chunk * device.geometry.lba_size);
			chunk_idx++;
		}

		std::chrono::milliseconds interval = std::chrono::milliseconds(0);
		for (auto &fut : futures) {
			std::future_status status;
			do {
				xnvme_queue_poke(queue, 0);
				status = fut.wait_for(interval);
			} while (status != std::future_status::ready);
		}

		device.FreeDeviceBuffer(dev_buffer, alloc_size);
	}
};
} // namespace duckdb
