#pragma once

#include "duckdb.hpp"
#include <libxnvme.h>
#include <mutex>
#include <vector>
#include <unordered_set>
#include <algorithm>

namespace duckdb {

static constexpr idx_t SPDK_HUGEPAGE_SIZE = 2 * 1024 * 1024;
static constexpr idx_t DUCKDB_BLOCK_SIZE = 256 * 1024;
static constexpr idx_t CHUNKS_PER_PAGE = SPDK_HUGEPAGE_SIZE / DUCKDB_BLOCK_SIZE;
static constexpr idx_t THREAD_CACHE_SIZE = 16 * 4 * 2;

class NvmeMemoryManager {
public:
	NvmeMemoryManager(xnvme_dev *device) : device(device) {
	}

	~NvmeMemoryManager() {
		std::lock_guard<std::mutex> lock(global_mutex);
		// Free fixed pool pages
		for (void *page : global_hugepages) {
			xnvme_buf_free(device, page);
		}
		// Free variable allocations
		for (void *ptr : variable_allocations) {
			xnvme_buf_free(device, ptr);
		}
		global_hugepages.clear();
		global_free_list.clear();
		variable_allocations.clear();
	}

	/**
	 * Fast Path: If the size matches the standard DuckDB block size (256KB),
	 * it pulls from a Thread-Local Cache. This is extremely fast because it
	 * avoids locking mutexes
	 *
	 * Slow Path: For non-standard sizes, it allocates directly from xNVMe/SPDK.
	 * It tracks these in variable_allocations so the manager knows which pointers
	 * belong to it. TODO: There is room for more improvement here.
	 */
	void *Allocate(idx_t size) {
		// 1. FAST PATH: Standard Block (256KB) -> Use Thread Cache
		if (size == DUCKDB_BLOCK_SIZE) {
			if (!thread_cache.empty()) {
				void *ptr = thread_cache.back();
				thread_cache.pop_back();
				return ptr;
			}
			return RefillThreadCache();
		}

		// 2. SLOW PATH: Variable Size (8KB, 16KB, 32KB...) -> Direct Hugepage Alloc
		// We must track these so we know they are ours when Free() is called.
		void *ptr = xnvme_buf_alloc(device, size);
		if (ptr) {
			std::lock_guard<std::mutex> lock(global_mutex);
			variable_allocations.insert(ptr);
		}
		return ptr;
	}

	/**
	 * If it's a standard block, it puts it back into the thread-local
	 * cache for reuse. If the cache is full, it flushes half of it back
	 * to the global pool.
	 */
	void Free(void *buffer, idx_t size) {
		if (!buffer) {
			return;
		}

		// 1. FAST PATH: Standard Block -> Return to Cache
		if (size == DUCKDB_BLOCK_SIZE) {
			if (thread_cache.size() < THREAD_CACHE_SIZE) {
				thread_cache.push_back(buffer);
				return;
			}
			ReturnToGlobal(buffer);
			return;
		}

		// 2. SLOW PATH: Variable Size -> Untrack and Free
		{
			std::lock_guard<std::mutex> lock(global_mutex);
			// Verify it is actually ours before freeing (Safety against malloc pointers)
			auto it = variable_allocations.find(buffer);
			if (it != variable_allocations.end()) {
				variable_allocations.erase(it);
				xnvme_buf_free(device, buffer);
			} else {
				// If not found in our set, it might be a fixed block freed with wrong size (bug)
				// or a malloc pointer. For safety, we do nothing here and let the Allocator handle it?
				// Actually, Allocator calls IsManaged first. If we are here, we own it.
				xnvme_buf_free(device, buffer);
			}
		}
	}

	/**
	 * Distinguishes between Hugepage Pointers (Ours) and Malloc Pointers (System)
	 *
	 * This checks if a given pointer (provided by DuckDB) lives inside
	 * the Hugepage memory pool. If it returns true, the NvmeDevice knows
	 * it can skip the memcpy and tell the hardware to read/write from
	 * that pointer directly
	 */
	bool IsManaged(void *ptr, idx_t size) {
		// Optimization: If size matches block size, check the fast pool logic first
		if (size == DUCKDB_BLOCK_SIZE) {
			return IsInFixedPool(ptr);
		}

		// Otherwise check the variable allocation set
		std::lock_guard<std::mutex> lock(global_mutex);
		return variable_allocations.count(ptr) > 0;
	}

private:
	xnvme_dev *device;
	std::mutex global_mutex;

	// Fixed Size Pool (256KB)
	std::vector<void *> global_free_list;
	std::vector<void *> global_hugepages;

	// Variable Size Tracker (8KB, 16KB...)
	std::unordered_set<void *> variable_allocations;

	static thread_local std::vector<void *> thread_cache;

	/**
	 * Performs pointer arithmetic to see if a memory address
	 * falls within the range of any allocated Hugepages.
	 */
	bool IsInFixedPool(void *ptr) {
		std::lock_guard<std::mutex> lock(global_mutex);
		uintptr_t p_ptr = (uintptr_t)ptr;

		for (void *page : global_hugepages) {
			uintptr_t p_start = (uintptr_t)page;
			uintptr_t p_end = p_start + SPDK_HUGEPAGE_SIZE;
			if (p_ptr >= p_start && p_ptr < p_end) {
				return true;
			}
		}

		return false;
	}

	/**
	 * When a thread runs out of buffers, it grabs a batch from
	 * the global pool. If the global pool is empty, it asks the
	 * OS for a new 2MB Hugepage and slices it into 256KB chunks.
	 */
	void *RefillThreadCache() {
		std::lock_guard<std::mutex> lock(global_mutex);
		if (global_free_list.empty()) {
			void *huge_page = xnvme_buf_alloc(device, SPDK_HUGEPAGE_SIZE);

			// OOM
			if (!huge_page) {
				return nullptr;
			}

			global_hugepages.push_back(huge_page);
			uint8_t *base = static_cast<uint8_t *>(huge_page);

			for (idx_t i = 0; i < CHUNKS_PER_PAGE; ++i) {
				global_free_list.push_back(base + (i * DUCKDB_BLOCK_SIZE));
			}
		}

		idx_t count = std::min((idx_t)THREAD_CACHE_SIZE, (idx_t)global_free_list.size());

		for (idx_t i = 0; i < count; i++) {
			thread_cache.push_back(global_free_list.back());
			global_free_list.pop_back();
		}

		if (thread_cache.empty()) {
			return nullptr;
		}

		void *ptr = thread_cache.back();
		thread_cache.pop_back();
		return ptr;
	}

	void ReturnToGlobal(void *overflow_buffer) {
		std::lock_guard<std::mutex> lock(global_mutex);
		global_free_list.push_back(overflow_buffer);
		idx_t flush_count = thread_cache.size() / 2;
		for (idx_t i = 0; i < flush_count; i++) {
			global_free_list.push_back(thread_cache.back());
			thread_cache.pop_back();
		}
	}
};

inline thread_local std::vector<void *> NvmeMemoryManager::thread_cache;

} // namespace duckdb