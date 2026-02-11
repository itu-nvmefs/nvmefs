#pragma once

#include "duckdb.hpp"
#include <libxnvme.h>
#include <mutex>
#include <vector>
#include <algorithm> // For std::min

namespace duckdb {

// Constants
static constexpr idx_t SPDK_HUGEPAGE_SIZE = 2 * 1024 * 1024;     // 2MB
static constexpr idx_t DUCKDB_BLOCK_SIZE = 256 * 1024;           // 256KB
static constexpr idx_t CHUNKS_PER_PAGE = SPDK_HUGEPAGE_SIZE / DUCKDB_BLOCK_SIZE; // 8
static constexpr idx_t THREAD_CACHE_SIZE = 16; // Each thread keeps up to 16 blocks locally

class NvmeMemoryManager {
public:
    NvmeMemoryManager(xnvme_dev *device) : device(device) {}

    // Destructor: Cleanup global hugepages
    ~NvmeMemoryManager() {
        std::lock_guard<std::mutex> lock(global_mutex);
        for (void* page : global_hugepages) {
            xnvme_buf_free(device, page);
        }
        global_hugepages.clear();
        global_free_list.clear();
    }

    void *Allocate(idx_t nr_bytes) {
        // Fallback for weird sizes (unlikely in DuckDB)
        if (nr_bytes > DUCKDB_BLOCK_SIZE) {
            return xnvme_buf_alloc(device, nr_bytes);
        }

        // 1. FAST PATH: Check Thread-Local Cache (NO LOCK)
        if (!thread_cache.empty()) {
            void* ptr = thread_cache.back();
            thread_cache.pop_back();
            return ptr;
        }

        // 2. SLOW PATH: Cache is empty. Go to global wholesale.
        return RefillThreadCache();
    }

    void Free(void *buffer) {
        if (!buffer) return;

        // Note: If you support mixed sizes, add a check here to xnvme_buf_free large ones directly.
        
        // 1. FAST PATH: Return to Thread-Local Cache (NO LOCK)
        if (thread_cache.size() < THREAD_CACHE_SIZE) {
            thread_cache.push_back(buffer);
            return;
        }

        // 2. SLOW PATH: Cache is full. Return bulk to global.
        ReturnToGlobal(buffer);
    }

private:
    xnvme_dev *device;
    std::mutex global_mutex;
    
    // Global Wholesale Stock
    std::vector<void*> global_free_list;
    std::vector<void*> global_hugepages; // Only for cleanup
    
    // Per-Thread Retail Stock (Declaration)
    static thread_local std::vector<void*> thread_cache;

    // Helper: Bulk fetch from global
    void* RefillThreadCache() {
        std::lock_guard<std::mutex> lock(global_mutex);

        // If global is empty, allocate a new hugepage
        if (global_free_list.empty()) {
            void* huge_page = xnvme_buf_alloc(device, SPDK_HUGEPAGE_SIZE);
            if (!huge_page) {
                throw std::runtime_error("NvmeMemoryManager: Failed to allocate xNVMe hugepage");
            }
            
            global_hugepages.push_back(huge_page);
            uint8_t* base = static_cast<uint8_t*>(huge_page);

            // Add all chunks to global list
            for (idx_t i = 0; i < CHUNKS_PER_PAGE; ++i) {
                global_free_list.push_back(base + (i * DUCKDB_BLOCK_SIZE));
            }
        }

        // Move a batch from Global -> Thread Cache
        // We take up to THREAD_CACHE_SIZE items, or whatever is available
        idx_t count = std::min((idx_t)THREAD_CACHE_SIZE, (idx_t)global_free_list.size());
        for(idx_t i = 0; i < count; i++) {
             thread_cache.push_back(global_free_list.back());
             global_free_list.pop_back();
        }

        // Return the first one immediately to the caller
        if (thread_cache.empty()) {
             // Should never happen unless allocation failed
             return nullptr; 
        }
        
        void* ptr = thread_cache.back();
        thread_cache.pop_back();
        return ptr;
    }

    // Helper: Bulk return to global
    void ReturnToGlobal(void* overflow_buffer) {
        std::lock_guard<std::mutex> lock(global_mutex);
        
        // Return the overflow buffer
        global_free_list.push_back(overflow_buffer);

        // Also flush half of the thread cache to global so we don't hit this lock again soon
        idx_t flush_count = thread_cache.size() / 2; 
        for(idx_t i = 0; i < flush_count; i++) {
            global_free_list.push_back(thread_cache.back());
            thread_cache.pop_back();
        }
    }
};

// DEFINITION: The 'inline' keyword prevents multiple definition linker errors
inline thread_local std::vector<void*> NvmeMemoryManager::thread_cache;

} // namespace duckdb