#pragma once

#include "duckdb.hpp"
#include <libxnvme.h>
#include <bitset>
#include <mutex>
#include <unordered_map>

namespace duckdb {

// Constants based on the problem description
static constexpr idx_t SPDK_HUGEPAGE_SIZE = 2 * 1024 * 1024;                     // 2MB
static constexpr idx_t DUCKDB_BLOCK_SIZE = 256 * 1024;                           // 256KB
static constexpr idx_t CHUNKS_PER_SLAB = SPDK_HUGEPAGE_SIZE / DUCKDB_BLOCK_SIZE; // 8

struct Slab {
	void *raw_memory;                            // The 2MB hugepage from xnvme
	std::bitset<CHUNKS_PER_SLAB> allocation_map; // 0 = free, 1 = used
	idx_t used_count;

	Slab(void *mem) : raw_memory(mem), used_count(0) {
		allocation_map.reset();
	}
};

struct AllocationInfo {
	Slab *parent_slab;
	idx_t chunk_index;
};

class NvmeMemoryManager {
public:
	NvmeMemoryManager(xnvme_dev *device);
	~NvmeMemoryManager();

	/// @brief Allocates memory. If <= 256KB, uses slab allocator. Otherwise uses direct xnvme alloc.
	void *Allocate(idx_t nr_bytes);

	/// @brief Frees memory. Checks if it belongs to a slab or is a direct allocation.
	void Free(void *buffer);

private:
	xnvme_dev *device;
	std::recursive_mutex manager_mutex;

	// Pool of active slabs
	std::vector<unique_ptr<Slab>> slabs;

	// Lookup table to map a pointer back to its Slab metadata
	std::unordered_map<void *, AllocationInfo> pointer_map;

	// Helper to allocate a new 2MB slab from SPDK
	Slab *CreateNewSlab();
};

} // namespace duckdb