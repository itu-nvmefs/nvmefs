#pragma once

#include "duckdb.hpp"
#include "nvme_memory_manager.hpp"

namespace duckdb {

class NvmeFileSystem;

class NvmeAllocator {
public:
	// The clean entry point to swap the allocator
	static void OverwriteGlobal(DatabaseInstance &instance, NvmeFileSystem *fs);

private:
	// Internal callback wrappers required by DuckDB's C-style Allocator API

	/**
	 * The callback DuckDB calls whenever it needs memory. If the request
	 * is >= 4KB (typical for data blocks), it asks the NvmeMemoryManager
	 * for DMA-safe memory. If it's small (metadata/strings), it falls
	 * back to standard malloc.
	 */
	static data_ptr_t Allocate(PrivateAllocatorData *private_data, idx_t size);

	/**
	 * Checks if the pointer is one of ours using IsManaged.
	 * If so, it returns it to our pool; otherwise, it calls standard free().
	 */
	static void Free(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size);

	/**
	 * Handles resizing memory blocks by allocating a new DMA-safe block,
	 * copying the data, and freeing the old one.
	 */
	static data_ptr_t Reallocate(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t old_size, idx_t size);
};

// Internal state struct to hold our manager pointer
struct NvmeAllocatorData : public PrivateAllocatorData {
	NvmeMemoryManager *manager;
	explicit NvmeAllocatorData(NvmeMemoryManager *manager) : manager(manager) {
	}
};

} // namespace duckdb