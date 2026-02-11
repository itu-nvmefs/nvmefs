#include "nvme_memory_manager.hpp"

namespace duckdb {

NvmeMemoryManager::NvmeMemoryManager(xnvme_dev *device) : device(device) {
}

NvmeMemoryManager::~NvmeMemoryManager() {
	std::lock_guard<std::recursive_mutex> lock(manager_mutex);

	// Cleanup all slabs
	for (auto &slab : slabs) {
		if (slab->raw_memory) {
			xnvme_buf_free(device, slab->raw_memory);
		}
	}
	slabs.clear();
	pointer_map.clear();
}

Slab *NvmeMemoryManager::CreateNewSlab() {
	// Allocate 2MB hugepage
	void *mem = xnvme_buf_alloc(device, SPDK_HUGEPAGE_SIZE);
	if (!mem) {
		throw std::runtime_error("Failed to allocate xNVMe hugepage buffer");
	}

	slabs.push_back(make_uniq<Slab>(mem));
	return slabs.back().get();
}

void *NvmeMemoryManager::Allocate(idx_t nr_bytes) {
	std::lock_guard<std::recursive_mutex> lock(manager_mutex);

	// 1. Fallback: Request is too large for our chunks (unlikely given DuckDB constraints, but safe)
	if (nr_bytes > DUCKDB_BLOCK_SIZE) {
		return xnvme_buf_alloc(device, nr_bytes);
	}

	// 2. Try to find a free chunk in existing slabs
	for (auto &slab : slabs) {
		if (slab->used_count < CHUNKS_PER_SLAB) {
			// Find the first unset bit (free chunk)
			for (size_t i = 0; i < CHUNKS_PER_SLAB; ++i) {
				if (!slab->allocation_map.test(i)) {
					// Mark used
					slab->allocation_map.set(i);
					slab->used_count++;

					// Calculate pointer offset
					uint8_t *base = static_cast<uint8_t *>(slab->raw_memory);
					void *chunk_ptr = base + (i * DUCKDB_BLOCK_SIZE);

					// Register metadata for Free()
					pointer_map[chunk_ptr] = {slab.get(), i};

					return chunk_ptr;
				}
			}
		}
	}

	// 3. No free chunks found, create a new Slab
	Slab *new_slab = CreateNewSlab();

	// Allocate the first chunk (index 0)
	new_slab->allocation_map.set(0);
	new_slab->used_count++;

	void *chunk_ptr = new_slab->raw_memory;
	pointer_map[chunk_ptr] = {new_slab, 0};

	return chunk_ptr;
}

void NvmeMemoryManager::Free(void *buffer) {
	if (!buffer)
		return;

	std::lock_guard<std::recursive_mutex> lock(manager_mutex);

	// 1. Check if this pointer belongs to our Slab allocator
	auto it = pointer_map.find(buffer);
	if (it == pointer_map.end()) {
		// It's not in our map, so it must be a direct xnvme allocation (oversized)
		xnvme_buf_free(device, buffer);
		return;
	}

	// 2. It is a slab allocation
	AllocationInfo info = it->second;
	Slab *slab = info.parent_slab;

	// Sanity check
	if (!slab->allocation_map.test(info.chunk_index)) {
		// Double free or logic error
		return;
	}

	// Mark free
	slab->allocation_map.reset(info.chunk_index);
	slab->used_count--;

	// Remove from map
	pointer_map.erase(it);

	// 3. Optimization: If Slab is completely empty, we *could* free the hugepage
	// to return memory to the OS/SPDK pool.
	// However, we often keep it to avoid allocation thrashing.
	// Let's implement a simple heuristic: if we have too many free slabs, trim.
	// For now, we will simply keep them active for performance.
}

} // namespace duckdb