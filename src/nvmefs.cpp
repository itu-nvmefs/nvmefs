#include "nvmefs.hpp"
#include "strategies/file_strategy_factory.hpp"
#include "nvmefs_path_handler.hpp"
#include <atomic>

namespace duckdb {

NvmeFileHandle::NvmeFileHandle(FileSystem &file_system, string path, FileOpenFlags flags,
                               unique_ptr<FileMetadataStrategy> strategy_p)
    : FileHandle(file_system, path, flags), cursor_offset(0), strategy(std::move(strategy_p)) {
}

void NvmeFileHandle::Read(void *buffer, idx_t nr_bytes, idx_t location) {
	file_system.Read(*this, buffer, nr_bytes, location);
}

void NvmeFileHandle::Write(void *buffer, idx_t nr_bytes, idx_t location) {
	file_system.Write(*this, buffer, nr_bytes, location);
}

idx_t NvmeFileHandle::GetFileSize() {
	return file_system.GetFileSize(*this);
}

void NvmeFileHandle::Sync() {
	file_system.FileSync(*this);
}

void NvmeFileHandle::Close() {
}

std::atomic<uint64_t> nvmefs_current_wal_bytes {0};
std::atomic<uint64_t> nvmefs_peak_wal_bytes {0};

std::atomic<uint64_t> nvmefs_total_spill_bytes {0};
std::atomic<uint64_t> nvmefs_total_wal_bytes {0};

std::atomic<uint64_t> nvmefs_total_db_bytes {0};
std::atomic<uint64_t> nvmefs_current_db_bytes {0};
std::atomic<uint64_t> nvmefs_peak_db_bytes {0};

unique_ptr<CmdContext> NvmeFileHandle::PrepareCommand(idx_t nr_bytes, idx_t start_lba, idx_t offset) {
	unique_ptr<NvmeCmdContext> nvme_cmd_ctx = make_uniq<NvmeCmdContext>();
	nvme_cmd_ctx->nr_bytes = nr_bytes;
	nvme_cmd_ctx->filepath = path;
	nvme_cmd_ctx->offset = offset;
	nvme_cmd_ctx->start_lba = start_lba;
	nvme_cmd_ctx->nr_lbas = CalculateRequiredLBACount(nr_bytes, offset);

	return std::move(nvme_cmd_ctx);
}

idx_t NvmeFileHandle::CalculateRequiredLBACount(idx_t nr_bytes, idx_t offset) {
	NvmeFileSystem &nvmefs = file_system.Cast<NvmeFileSystem>();
	DeviceGeometry geo = nvmefs.GetDevice().GetDeviceGeometry();
	return (offset + nr_bytes + geo.lba_size - 1) / geo.lba_size;
}

void NvmeFileHandle::SetFilePointer(idx_t location) {
	cursor_offset = location;
}

idx_t NvmeFileHandle::GetFilePointer() {
	return cursor_offset;
}

bool NvmeFileSystem::CanSeek() {
	return true;
}

////////////////////////////////////////

NvmeFileSystem::NvmeFileSystem(NvmeConfig config_p)
    : allocator(Allocator::DefaultAllocator()), device(make_uniq<NvmeDevice>(config_p)), config(std::move(config_p)) {
}

NvmeFileSystem::NvmeFileSystem(NvmeConfig config_p, unique_ptr<Device> device)
    : allocator(Allocator::DefaultAllocator()), device(std::move(device)), config(std::move(config_p)) {
}

NvmeFileSystem::~NvmeFileSystem() {
	if (metadata) {
		WriteMetadata(*metadata);
	}
	temp_meta_manager.reset();
	device.reset();
}

void NvmeFileSystem::AllocateNewDatabaseRegion(const string &db_name) {
	if (metadata->active_databases >= MAX_NVME_DATABASES)
		throw IOException("Max NVMe databases reached");

	DeviceGeometry geo = device->GetDeviceGeometry();
	idx_t requested_bytes = config.default_db_size;
	if (config.db_configs.count(db_name))
		requested_bytes = config.db_configs[db_name];

	idx_t requested_lbas = requested_bytes / geo.lba_size;
	idx_t wal_lbas = config.max_wal_size / geo.lba_size;
	idx_t highest_lba = 1;

	for (uint32_t i = 0; i < MAX_NVME_DATABASES; i++) {
		if (metadata->databases[i].is_active) {
			if (metadata->databases[i].wal_end > highest_lba)
				highest_lba = metadata->databases[i].wal_end;
		}
	}

	if (highest_lba + requested_lbas + wal_lbas >= metadata->tmp_start) {
		throw IOException("Insufficient NVMe disk space");
	}

	for (uint32_t i = 0; i < MAX_NVME_DATABASES; i++) {
		if (!metadata->databases[i].is_active) {
			DatabaseRegion &region = metadata->databases[i];
			region.is_active = true;
			region.db_start = highest_lba;
			region.wal_start = highest_lba + requested_lbas;
			region.wal_end = region.wal_start + wal_lbas;
			region.db_location = region.db_start;
			region.wal_location = region.wal_start;
			strncpy(region.db_path, db_name.c_str(), 100);

			metadata->active_databases++;
			active_dbs[db_name] = make_uniq<DatabaseRuntimeState>(region.db_start, region.wal_start);
			WriteMetadata(*metadata);
			return;
		}
	}
}

unique_ptr<FileHandle> NvmeFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                optional_ptr<FileOpener> opener) {
	if (path == NvmePathHandler::GLOBAL_METADATA_PATH) {
		return make_uniq<NvmeFileHandle>(*this, path, flags, nullptr);
	}

	string db_name = NvmePathHandler::ExtractDatabaseName(path);
	NvmeFileType type = NvmePathHandler::GetFileType(path);

	if (!TryLoadMetadata()) {
		if (type != NvmeFileType::DATABASE) {
			throw IOException("No database is attached");
		}
		InitializeMetadata(db_name);
	}

	DatabaseRegion *region = GetRegionForPath(db_name);
	if (!region && flags.CreateFileIfNotExists()) {
		AllocateNewDatabaseRegion(db_name);
		region = GetRegionForPath(db_name);
	}

    auto &nvme_device = GetNvmeDevice();
    auto strategy = FileStrategyFactory::GetStrategy(path, region, metadata.get(), GetRuntimeState(db_name), nvme_device, temp_meta_manager);

    if (flags.CreateFileIfNotExists() && type == NvmeFileType::TEMPORARY) {
        strategy->CreateFile(path);
    }

	return make_uniq<NvmeFileHandle>(*this, path, flags, std::move(strategy));
}

void NvmeFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	NvmeFileHandle &fh = handle.Cast<NvmeFileHandle>();
	DeviceGeometry geo = device->GetDeviceGeometry();

	idx_t in_block_offset = location % geo.lba_size;
	idx_t nr_lbas = fh.CalculateRequiredLBACount(nr_bytes, in_block_offset);
	FileMetadataStrategy *strategy = fh.GetStrategy();

	idx_t start_lba = strategy->GetLBA(handle.path, nr_bytes, location, nr_lbas, geo);
	unique_ptr<CmdContext> cmd_ctx = fh.PrepareCommand(nr_bytes, start_lba, in_block_offset);

	if (!strategy->IsLBAInRange(handle.path, start_lba, cmd_ctx->nr_lbas, geo)) {
		throw IOException("Read out of range");
	}

	device->Read(buffer, *cmd_ctx);
}

void NvmeFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	NvmeFileHandle &fh = handle.Cast<NvmeFileHandle>();
	DeviceGeometry geo = device->GetDeviceGeometry();

	idx_t in_block_offset = location % geo.lba_size;
	idx_t nr_lbas = fh.CalculateRequiredLBACount(nr_bytes, in_block_offset);
	FileMetadataStrategy *strategy = fh.GetStrategy();

	idx_t start_lba = strategy->GetLBA(fh.GetPath(), nr_bytes, location, nr_lbas, geo);
	auto cmd_ctx = fh.PrepareCommand(nr_bytes, start_lba, in_block_offset);

	if (!strategy->IsLBAInRange(handle.path, start_lba, cmd_ctx->nr_lbas, geo)) {
		throw IOException("Write out of range");
	}

	device->Write(buffer, *cmd_ctx);
	strategy->UpdateMetadata(*cmd_ctx);

	NvmeFileType file_type = NvmePathHandler::GetFileType(fh.GetPath());

	if (file_type == NvmeFileType::TEMPORARY) {
		nvmefs_total_spill_bytes += nr_bytes;
	} else if (file_type == NvmeFileType::WAL) {
		nvmefs_total_wal_bytes += nr_bytes;
		uint64_t true_size = GetFileSize(handle);
		nvmefs_current_wal_bytes.store(true_size);

		uint64_t peak = nvmefs_peak_wal_bytes.load();
		while (true_size > peak && !nvmefs_peak_wal_bytes.compare_exchange_weak(peak, true_size)) {
		}
	} else if (file_type == NvmeFileType::DATABASE) {
		nvmefs_total_db_bytes += nr_bytes;

		uint64_t true_size = GetFileSize(handle);
		nvmefs_current_db_bytes.store(true_size);

		uint64_t peak = nvmefs_peak_db_bytes.load();
		while (true_size > peak && !nvmefs_peak_db_bytes.compare_exchange_weak(peak, true_size)) {
		}
	}
}

int64_t NvmeFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &fh = handle.Cast<NvmeFileHandle>();
	idx_t location = fh.GetFilePointer();

	Read(handle, buffer, nr_bytes, location);
	fh.SetFilePointer(location + nr_bytes);

	return nr_bytes;
}

int64_t NvmeFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &fh = handle.Cast<NvmeFileHandle>();
	idx_t location = fh.GetFilePointer();

	Write(handle, buffer, nr_bytes, location);
	fh.SetFilePointer(location + nr_bytes);

	return nr_bytes;
}

bool NvmeFileSystem::CanHandleFile(const string &fpath) {
	return NvmePathHandler::IsInternalPath(fpath);
}

bool NvmeFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
    if (!TryLoadMetadata()) return false;
    
    string db_name = ExtractDatabaseName(filename);
    auto &nvme_device = GetNvmeDevice();
    auto strategy = FileStrategyFactory::GetStrategy(filename, GetRegionForPath(db_name), metadata.get(), GetRuntimeState(db_name), nvme_device, temp_meta_manager);

	string db_name = NvmePathHandler::ExtractDatabaseName(filename);
	auto strategy = FileStrategyFactory::GetStrategy(filename, GetRegionForPath(db_name), metadata.get(),
	                                                 GetRuntimeState(db_name), temp_meta_manager);

	return strategy ? strategy->FileExists(filename) : false;
}

int64_t NvmeFileSystem::GetFileSize(FileHandle &handle) {
	DeviceGeometry geo = device->GetDeviceGeometry();
	NvmeFileHandle &fh = handle.Cast<NvmeFileHandle>();

	FileMetadataStrategy *strategy = fh.GetStrategy();

	idx_t nr_lbas = strategy->GetFileSizeLBA(fh.GetPath());
	return nr_lbas * geo.lba_size;
}

void NvmeFileSystem::FileSync(FileHandle &handle) {
	WriteMetadata(*metadata);
}

bool NvmeFileSystem::OnDiskFile(FileHandle &handle) {
	return true;
}

void NvmeFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	NvmeFileHandle &nvme_handle = handle.Cast<NvmeFileHandle>();
	int64_t current_size = GetFileSize(nvme_handle);

	if (new_size > current_size) {
		throw InvalidInputException("new_size is bigger than the current file size.");
	}

	FileMetadataStrategy *strategy = nvme_handle.GetStrategy();
	strategy->Truncate(nvme_handle.path, new_size);

	NvmeFileType file_type = NvmePathHandler::GetFileType(nvme_handle.GetPath());

	if (file_type == NvmeFileType::WAL) {
		nvmefs_current_wal_bytes.store(new_size);
	} else if (file_type == NvmeFileType::DATABASE) {
		nvmefs_current_db_bytes.store(new_size);
	}
}

bool NvmeFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	if (TryLoadMetadata()) {
		return true;
	}
	return false;
}

void NvmeFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	NvmeFileType type = NvmePathHandler::GetFileType(directory);

	if (type == NvmeFileType::TEMPORARY) {
		auto &nvme_device = GetNvmeDevice();
		TemporaryFileStrategy temp_strategy(metadata.get(), nvme_device, temp_meta_manager);
		temp_strategy.ClearAll();
	} else {
		throw IOException("Cannot delete unknown directory");
	}
}

void NvmeFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	if (!TryLoadMetadata()) {
		throw IOException("No directories can exist when there is no metadata");
	}
}

void NvmeFileSystem::CreateDirectoriesRecursive(const string &path, optional_ptr<FileOpener> opener) {
	if (!TryLoadMetadata()) {
		throw IOException("No directories can exist when there is no metadata");
	}
}

void NvmeFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	if (!TryLoadMetadata())
		return;

    string db_name = ExtractDatabaseName(filename);
    auto &nvme_device = GetNvmeDevice();
    auto strategy = FileStrategyFactory::GetStrategy(
        filename, GetRegionForPath(db_name), metadata.get(), GetRuntimeState(db_name), nvme_device, temp_meta_manager);

	if (strategy)
		strategy->RemoveFile(filename);
}

bool NvmeFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	NvmeFileSystem::RemoveFile(filename, opener);
	return true;
}

void NvmeFileSystem::RemoveFiles(const vector<string> &filenames, optional_ptr<FileOpener> opener) {
	for (auto &filename : filenames) {
		NvmeFileSystem::RemoveFile(filename, opener);
	}
}

void NvmeFileSystem::Seek(FileHandle &handle, idx_t location) {
	NvmeFileHandle &nvme_handle = handle.Cast<NvmeFileHandle>();
	DeviceGeometry geo = device->GetDeviceGeometry();

	D_ASSERT(location % geo.lba_size == 0);

    string db_name = ExtractDatabaseName(nvme_handle.path);
    auto &nvme_device = GetNvmeDevice();
    auto strategy = FileStrategyFactory::GetStrategy(nvme_handle.path, GetRegionForPath(db_name), metadata.get(), GetRuntimeState(db_name), nvme_device, temp_meta_manager);
    if (!strategy) {
        throw IOException("Cannot seek: database not attached or runtime state missing for: " + db_name);
    }
    if (location >= strategy->GetSeekBound(nvme_handle.path, geo)) {
        throw IOException("Seek location is out of bounds");
    }
    nvme_handle.SetFilePointer(location);
}

void NvmeFileSystem::Reset(FileHandle &handle) {
	NvmeFileHandle &fh = handle.Cast<NvmeFileHandle>();
	fh.SetFilePointer(0);
}

idx_t NvmeFileSystem::SeekPosition(FileHandle &handle) {
	return handle.Cast<NvmeFileHandle>().GetFilePointer();
}

bool NvmeFileSystem::ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback, FileOpener *opener) {
    bool dir = false;
    if (StringUtil::Equals(directory.data(), NvmePathHandler::PATH_PREFIX.data())) {
        if (metadata) {
            for (uint32_t i = 0; i < MAX_NVME_DATABASES; i++) {
                if (metadata->databases[i].is_active) {
                    string db_name = metadata->databases[i].db_path;
                    callback(db_name + ".db", false);
                    callback(db_name + ".db.wal", false);
                }
            }
        }
        callback("/tmp", true);
        dir = true;
    } else if (StringUtil::Equals(directory.data(), NvmePathHandler::TMP_DIR_PATH.data())) {
        dir = true;
        auto &nvme_device = GetNvmeDevice();
        TemporaryFileStrategy temp_strategy(metadata.get(), nvme_device, temp_meta_manager);
        temp_strategy.ListFiles(directory, callback);
    }
    return dir;
}

optional_idx NvmeFileSystem::GetAvailableDiskSpace(const string &path) {
	DeviceGeometry geo = device->GetDeviceGeometry();
	optional_idx remaining;

    if (StringUtil::Equals(path.data(), NvmePathHandler::PATH_PREFIX.data())) {
        idx_t total_avail = 0;
        if (metadata) {
            for (uint32_t i = 0; i < MAX_NVME_DATABASES; i++) {
                if (!metadata->databases[i].is_active) continue;
                auto *state = GetRuntimeState(metadata->databases[i].db_path);
                if (!state) continue;
                DatabaseFileStrategy db_strategy(&metadata->databases[i], state->db_location);
                WALFileStrategy wal_strategy(&metadata->databases[i], state->wal_location);
                total_avail += db_strategy.GetAvailableSpace(geo).GetIndex();
                total_avail += wal_strategy.GetAvailableSpace(geo).GetIndex();
            }
            auto &nvme_device = GetNvmeDevice();
            TemporaryFileStrategy temp_strategy(metadata.get(), nvme_device, temp_meta_manager);
            total_avail += temp_strategy.GetAvailableSpace(geo).GetIndex();
        }
        remaining = total_avail;
    } else if (StringUtil::Equals(path.data(), NvmePathHandler::TMP_DIR_PATH.data())) {
        auto &nvme_device = GetNvmeDevice();
        TemporaryFileStrategy temp_strategy(metadata.get(), nvme_device, temp_meta_manager);
        remaining = temp_strategy.GetAvailableSpace(geo);
    }
    return remaining;
}
Device &NvmeFileSystem::GetDevice() {
	return *device;
}

bool NvmeFileSystem::Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) {
	data_ptr_t data = allocator.AllocateData(length_bytes);

	memset(data, 0, length_bytes);
	Write(handle, data, length_bytes, offset_bytes);

	allocator.FreeData(data, length_bytes);
	return true;
}

string NvmeFileSystem::CanonicalizePath(const string &path, optional_ptr<FileOpener> opener) {
	return path;
}

DatabaseRuntimeState *NvmeFileSystem::GetRuntimeState(const string &db_name) {
	auto it = active_dbs.find(db_name);
	return it != active_dbs.end() ? it->second.get() : nullptr;
}

DatabaseRegion *NvmeFileSystem::GetRegionForPath(const string &db_name) {
	if (!metadata)
		return nullptr;
	for (uint32_t i = 0; i < MAX_NVME_DATABASES; i++) {
		if (metadata->databases[i].is_active && string(metadata->databases[i].db_path) == db_name) {
			return &metadata->databases[i];
		}
	}
	return nullptr;
}

bool NvmeFileSystem::TryLoadMetadata() {
	if (metadata)
		return true;

	unique_ptr<GlobalMetadata> global = ReadMetadata();
	if (global) {
		metadata = std::move(global);
		for (uint32_t i = 0; i < MAX_NVME_DATABASES; i++) {
			if (metadata->databases[i].is_active) {
				string name = metadata->databases[i].db_path;
				active_dbs[name] = make_uniq<DatabaseRuntimeState>(metadata->databases[i].db_location,
				                                                   metadata->databases[i].wal_location);
			}
		}
		DeviceGeometry geo = device->GetDeviceGeometry();
		temp_meta_manager =
		    make_uniq<TemporaryFileMetadataManager>(metadata->tmp_start, geo.lba_count - 1, geo.lba_size);
		return true;
	}
	return false;
}

void NvmeFileSystem::InitializeMetadata(const string &first_db_name) {
	DeviceGeometry geo = device->GetDeviceGeometry();
	unique_ptr<GlobalMetadata> global = make_uniq<GlobalMetadata>();
	memset(global.get(), 0, sizeof(GlobalMetadata));

	global->tmp_start = geo.lba_count - (config.max_temp_size / geo.lba_size);
	idx_t wal_lbas = config.max_wal_size / geo.lba_size;
	idx_t current_lba = 1;
	uint32_t db_idx = 0;

	auto RegisterDB = [&](const string &name, idx_t requested_lbas) {
		if (db_idx >= MAX_NVME_DATABASES)
			throw IOException("Max NVMe databases reached.");
		if (current_lba + requested_lbas + wal_lbas >= global->tmp_start) {
			throw IOException("Insufficient NVMe disk space.");
		}

		DatabaseRegion &region = global->databases[db_idx++];
		region.is_active = true;
		strncpy(region.db_path, name.c_str(), 100);
		region.db_start = current_lba;
		region.wal_start = current_lba + requested_lbas;
		region.wal_end = region.wal_start + wal_lbas;
		region.db_location = region.db_start;
		region.wal_location = region.wal_start;
		current_lba = region.wal_end;
	};

	if (config.db_configs.empty()) {
		idx_t usable_lbas = global->tmp_start - current_lba - wal_lbas;
		RegisterDB(first_db_name, usable_lbas);
	} else {
		for (const auto &kv : config.db_configs)
			RegisterDB(kv.first, kv.second / geo.lba_size);

		bool first_allocated = false;
		for (uint32_t i = 0; i < db_idx; i++) {
			if (string(global->databases[i].db_path) == first_db_name)
				first_allocated = true;
		}
		if (!first_allocated)
			RegisterDB(first_db_name, config.default_db_size / geo.lba_size);
	}

	global->active_databases = db_idx;
	metadata = std::move(global);

	for (uint32_t i = 0; i < db_idx; i++) {
		string name = metadata->databases[i].db_path;
		active_dbs[name] =
		    make_uniq<DatabaseRuntimeState>(metadata->databases[i].db_start, metadata->databases[i].wal_start);
	}

	temp_meta_manager = make_uniq<TemporaryFileMetadataManager>(metadata->tmp_start, geo.lba_count - 1, geo.lba_size);

	WriteMetadata(*metadata);
}

unique_ptr<GlobalMetadata> NvmeFileSystem::ReadMetadata() {
	idx_t nr_bytes_magic = sizeof(NvmePathHandler::MAGIC_BYTES);
	idx_t nr_bytes_global = sizeof(GlobalMetadata);
	idx_t bytes_to_read = nr_bytes_magic + nr_bytes_global;

	data_ptr_t buffer = allocator.AllocateData(bytes_to_read);
	unique_ptr<GlobalMetadata> global = nullptr;

	FileOpenFlags flags = FileOpenFlags::FILE_FLAGS_READ;
	unique_ptr<FileHandle> fh = OpenFile(NvmePathHandler::GLOBAL_METADATA_PATH, flags);
	unique_ptr<CmdContext> cmd_ctx =
	    fh->Cast<NvmeFileHandle>().PrepareCommand(bytes_to_read, NvmePathHandler::GLOBAL_METADATA_LOCATION, 0);

	device->Read(buffer, *cmd_ctx);

	if (memcmp(buffer, NvmePathHandler::MAGIC_BYTES, nr_bytes_magic) == 0) {
		DeviceGeometry geo = device->GetDeviceGeometry();
		global = make_uniq<GlobalMetadata>(GlobalMetadata {});
		memcpy(global.get(), buffer + nr_bytes_magic, nr_bytes_global);
		temp_meta_manager = make_uniq<TemporaryFileMetadataManager>(global->tmp_start, geo.lba_count - 1, geo.lba_size);
	}

	allocator.FreeData(buffer, bytes_to_read);

	return std::move(global);
}

void NvmeFileSystem::WriteMetadata(GlobalMetadata &global) {
	idx_t nr_bytes_magic = sizeof(NvmePathHandler::MAGIC_BYTES);
	idx_t nr_bytes_global = sizeof(GlobalMetadata);
	idx_t bytes_to_write = nr_bytes_magic + nr_bytes_global;

	for (uint32_t i = 0; i < MAX_NVME_DATABASES; i++) {
		if (global.databases[i].is_active) {
			string db_name = global.databases[i].db_path;
			if (active_dbs.count(db_name)) {
				global.databases[i].db_location = active_dbs[db_name]->db_location.load();
				global.databases[i].wal_location = active_dbs[db_name]->wal_location.load();
			}
		}
	}

	data_ptr_t buffer = allocator.AllocateData(bytes_to_write);
	memcpy(buffer, NvmePathHandler::MAGIC_BYTES, nr_bytes_magic);
	memcpy(buffer + nr_bytes_magic, &global, nr_bytes_global);

	FileOpenFlags flags = FileOpenFlags::FILE_FLAGS_WRITE;
	unique_ptr<FileHandle> fh = OpenFile(NvmePathHandler::GLOBAL_METADATA_PATH, flags);
	unique_ptr<CmdContext> cmd_ctx =
	    fh->Cast<NvmeFileHandle>().PrepareCommand(bytes_to_write, NvmePathHandler::GLOBAL_METADATA_LOCATION, 0);

	device->Write(buffer, *cmd_ctx);
	allocator.FreeData(buffer, bytes_to_write);
}

NvmeDevice &NvmeFileSystem::GetNvmeDevice() {
	NvmeDevice *raw_nvme_device = dynamic_cast<NvmeDevice *>(device.get());

	if (raw_nvme_device) {
		return *raw_nvme_device;
	} else {
		throw InternalException("Value of field device is not of type NvmeDevice");
	}
}

} // namespace duckdb
