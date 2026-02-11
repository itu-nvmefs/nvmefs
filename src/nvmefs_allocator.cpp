#include "nvmefs_allocator.hpp"
#include "nvmefs.hpp" 
#include "duckdb/main/database.hpp"
#include "duckdb/main/config.hpp"

#include <new>
#include <iostream>


namespace duckdb {

// Define the static wrapper functions
// We must define these here to match the declarations in the header

void NvmeAllocator::OverwriteGlobal(DatabaseInstance &instance, NvmeFileSystem* nvme_fs) {
    
    if (!nvme_fs) {
        std::cout << "NvmeAllocator: Null filesystem passed.\n";
        return;
    }
    
    // Get the device and then the manager
    // Note: Assuming GetDevice() returns a reference to NvmeDevice or Device
    auto &device_base = nvme_fs->GetDevice();
    auto &device = (NvmeDevice&)device_base; // Downcast to NvmeDevice
    
    NvmeMemoryManager* manager = device.GetMemoryManager();

    if (!manager) {
        // Should not happen if FS initialized correctly
        std::cout << "no manager found \n";
        return;
    }

    // 2. Perform the Swap (The "Magic")
    // Get the configuration object where the allocator lives
    DBConfig &config = DBConfig::GetConfig(instance);

    Allocator* global_allocator = config.allocator.get();

    if (!global_allocator) {
        std::cout << "no allocator found \n";
        return;
    }

    global_allocator->~Allocator();

    // 3. Construct the NEW object at the SAME memory address
    // This effectively "hot-patches" the object that BufferManager is using.
    new (global_allocator) Allocator(
        NvmeAllocator::Allocate,
        NvmeAllocator::Free,
        NvmeAllocator::Reallocate,
        make_uniq<NvmeAllocatorData>(manager)
    );
}

data_ptr_t NvmeAllocator::Allocate(PrivateAllocatorData *private_data, idx_t size) {
    auto *data = (NvmeAllocatorData *)private_data;
    
    if (size >= 4096) {
        return (data_ptr_t)data->manager->Allocate(size);
    }

    // Fallback to standard system malloc for metadata/strings
    return (data_ptr_t)malloc(size);
}

void NvmeAllocator::Free(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size) {
    auto *data = (NvmeAllocatorData *)private_data;

    // Hot-swap safety: Check if pointer belongs to our Hugepages
    // If it does, return it to our pool. If not (allocated before swap), system free.
    if (size >= 4096 && data->manager->IsManaged(pointer, size)) {
        data->manager->Free(pointer, size);
    } else {
        free(pointer);
    }
}

data_ptr_t NvmeAllocator::Reallocate(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t old_size, idx_t size) {
    // Standard realloc pattern: Allocate New -> Copy -> Free Old
    data_ptr_t new_ptr = Allocate(private_data, size);
    if (pointer) {
        memcpy(new_ptr, pointer, std::min(old_size, size));
        Free(private_data, pointer, old_size);
    }
    return new_ptr;
}

} // namespace duckdb