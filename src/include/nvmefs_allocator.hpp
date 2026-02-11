#pragma once

#include "duckdb.hpp"
#include "nvme_memory_manager.hpp"

namespace duckdb {

class NvmeFileSystem;

class NvmeAllocator {
public:
    // The clean entry point to swap the allocator
    static void OverwriteGlobal(DatabaseInstance &instance, NvmeFileSystem* fs);
private:
    // Internal callback wrappers required by DuckDB's C-style Allocator API
    static data_ptr_t Allocate(PrivateAllocatorData *private_data, idx_t size);
    static void Free(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size);
    static data_ptr_t Reallocate(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t old_size, idx_t size);
};

// Internal state struct to hold our manager pointer
struct NvmeAllocatorData : public PrivateAllocatorData {
    NvmeMemoryManager* manager;
    explicit NvmeAllocatorData(NvmeMemoryManager* manager) : manager(manager) {}
};

} // namespace duckdb