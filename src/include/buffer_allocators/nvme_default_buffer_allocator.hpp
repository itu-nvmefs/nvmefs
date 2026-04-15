#pragma once

#include "nvme_buffer_allocator.hpp"

namespace duckdb {

class NvmeDefaultBufferAllocator : public NvmeBufferAllocator {
public:
	using NvmeBufferAllocator::NvmeBufferAllocator;

	nvme_buf_ptr Allocate(idx_t size) override {
		return xnvme_buf_alloc(device, size);
	}

	void Free(nvme_buf_ptr buffer, idx_t size) override {
		xnvme_buf_free(device, buffer);
	}
};

} // namespace duckdb