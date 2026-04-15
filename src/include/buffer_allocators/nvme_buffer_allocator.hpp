#pragma once

#include "duckdb.hpp"
#include <libxnvme.h>

namespace duckdb {

typedef void *nvme_buf_ptr;

class NvmeBufferAllocator {
public:
	explicit NvmeBufferAllocator(xnvme_dev *device) : device(device) {
	}
	virtual ~NvmeBufferAllocator() = default;

	virtual nvme_buf_ptr Allocate(idx_t size) = 0;
	virtual void Free(nvme_buf_ptr buffer, idx_t size) = 0;
	virtual void Flush() {
	}

protected:
	xnvme_dev *device;
};

} // namespace duckdb