#pragma once

#include "nvme_buffer_allocator.hpp"

namespace duckdb {

static constexpr idx_t BUFFER_CACHE_DEPTH = 32;
static constexpr idx_t DUCKDB_BLOCK_SIZE = 256 * 1024;

struct ThreadLocalCache {
	void *cache[BUFFER_CACHE_DEPTH];
	idx_t count = 0;
};

class NvmeCachedBufferAllocator : public NvmeBufferAllocator {
public:
	using NvmeBufferAllocator::NvmeBufferAllocator;

	nvme_buf_ptr Allocate(idx_t size) override {
		if (size == DUCKDB_BLOCK_SIZE) {
			auto &tls = GetTLS();
			if (tls.count > 0) {
				return tls.cache[--tls.count];
			}
		}

		return xnvme_buf_alloc(device, size);
	}

	void Free(nvme_buf_ptr buffer, idx_t size) override {
		if (!buffer)
			return;

		if (size == DUCKDB_BLOCK_SIZE) {
			auto &tls = GetTLS();
			if (tls.count < BUFFER_CACHE_DEPTH) {
				tls.cache[tls.count++] = buffer;
				return;
			}
		}

		xnvme_buf_free(device, buffer);
	}

	void Flush() override {
		auto &tls = GetTLS();
		while (tls.count > 0) {
			xnvme_buf_free(device, tls.cache[--tls.count]);
		}
	}

private:
	static ThreadLocalCache &GetTLS() {
		static thread_local ThreadLocalCache tls;
		return tls;
	}
};

} // namespace duckdb