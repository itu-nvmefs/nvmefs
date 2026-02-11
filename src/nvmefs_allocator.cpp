#include "nvmefs_allocator.hpp"
#include "nvmefs.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/config.hpp"

#include <new>
#include <iostream>

namespace duckdb {

void NvmeAllocator::OverwriteGlobal(DatabaseInstance &instance, NvmeFileSystem *nvme_fs) {
	auto &device = static_cast<NvmeDevice &>(nvme_fs->GetDevice());

	NvmeMemoryManager *manager = device.GetMemoryManager();
	DBConfig &config = DBConfig::GetConfig(instance);

	Allocator *global_allocator = config.allocator.get();

	if (!global_allocator) {
		throw InternalException("Global allocator is not set. Cannot overwrite with NvmeAllocator.");
	}

	global_allocator->~Allocator();

	// construct the new object at the same  memory address
	// this effectively "hot-patches" the object that BufferManager is using
	new (global_allocator) Allocator(NvmeAllocator::Allocate, NvmeAllocator::Free, NvmeAllocator::Reallocate,
	                                 make_uniq<NvmeAllocatorData>(manager));
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

	if (size >= 4096 && data->manager->IsManaged(pointer, size)) {
		data->manager->Free(pointer, size);
	} else {
		free(pointer);
	}
}

data_ptr_t NvmeAllocator::Reallocate(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t old_size,
                                     idx_t size) {
	// Standard realloc pattern: Allocate New -> Copy -> Free Old
	data_ptr_t new_ptr = Allocate(private_data, size);
	if (pointer) {
		memcpy(new_ptr, pointer, std::min(old_size, size));
		Free(private_data, pointer, old_size);
	}
	return new_ptr;
}

} // namespace duckdb