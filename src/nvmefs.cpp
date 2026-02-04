#include "nvmefs.hpp"
#include "strategies/file_strategy_factory.hpp"

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

unique_ptr<CmdContext> NvmeFileHandle::PrepareCommand(idx_t nr_bytes, idx_t start_lba, idx_t offset) {
	unique_ptr<NvmeCmdContext> nvme_cmd_ctx = make_uniq<NvmeCmdContext>();
	nvme_cmd_ctx->nr_bytes = nr_bytes;
	nvme_cmd_ctx->filepath = path;
	nvme_cmd_ctx->offset = offset;
	nvme_cmd_ctx->start_lba = start_lba;
	nvme_cmd_ctx->nr_lbas = CalculateRequiredLBACount(nr_bytes);

	return std::move(nvme_cmd_ctx);
}

idx_t NvmeFileHandle::CalculateRequiredLBACount(idx_t nr_bytes) {
	NvmeFileSystem &nvmefs = file_system.Cast<NvmeFileSystem>();
	DeviceGeometry geo = nvmefs.GetDevice().GetDeviceGeometry();
	idx_t lba_size = geo.lba_size;
	return (nr_bytes + lba_size - 1) / lba_size;
}

void NvmeFileHandle::SetFilePointer(idx_t location) {
	cursor_offset = location;
}

idx_t NvmeFileHandle::GetFilePointer() {
	return cursor_offset;
}

////////////////////////////////////////

NvmeFileSystem::NvmeFileSystem(NvmeConfig config)
    : allocator(Allocator::DefaultAllocator()),
      device(make_uniq<NvmeDevice>(config.device_path, config.backend, config.async, config.max_threads)),
      max_temp_size(config.max_temp_size), max_wal_size(config.max_wal_size), db_location(0), wal_location(0) {
}

NvmeFileSystem::NvmeFileSystem(NvmeConfig config, unique_ptr<Device> device)
    : allocator(Allocator::DefaultAllocator()), device(std::move(device)), max_temp_size(config.max_temp_size),
      max_wal_size(config.max_wal_size), db_location(0), wal_location(0) {
}

NvmeFileSystem::~NvmeFileSystem() {
	if (metadata) {
		WriteMetadata(*metadata);
	}
	temp_meta_manager.reset();
	device.reset();
}

unique_ptr<FileHandle> NvmeFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                optional_ptr<FileOpener> opener) {
	if (path == NvmePathHandler::GLOBAL_METADATA_PATH) {
		return make_uniq<NvmeFileHandle>(*this, path, flags, unique_ptr<FileMetadataStrategy>(nullptr));
	}

	if (!TryLoadMetadata()) {
		if (NvmePathHandler::GetFileType(path) != NvmeFileType::DATABASE) {
			throw IOException("No database is attached");
		} else {
			InitializeMetadata(path);
		}
	}

	if (!metadata) {
		throw InternalException("Metadata uninitialized after loading attempt");
	}

	unique_ptr<FileMetadataStrategy> strategy(
	    FileStrategyFactory::GetStrategy(path, metadata.get(), db_location, wal_location, temp_meta_manager));

	if (flags.CreateFileIfNotExists() && NvmePathHandler::GetFileType(path) == NvmeFileType::TEMPORARY) {
		strategy->CreateFile(path);
	}

	return make_uniq<NvmeFileHandle>(*this, path, flags, std::move(strategy));
}

void NvmeFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	NvmeFileHandle &fh = handle.Cast<NvmeFileHandle>();
	DeviceGeometry geo = device->GetDeviceGeometry();

	idx_t cursor_offset = SeekPosition(handle);
	location += cursor_offset;
	idx_t nr_lbas = fh.CalculateRequiredLBACount(nr_bytes);

	FileMetadataStrategy *strategy = fh.GetStrategy();

	idx_t start_lba = strategy->GetLBA(handle.path, nr_bytes, location, nr_lbas, geo);
	idx_t in_block_offset = location % geo.lba_size;
	unique_ptr<CmdContext> cmd_ctx = fh.PrepareCommand(nr_bytes, start_lba, in_block_offset);

	if (!strategy->IsLBAInRange(handle.path, start_lba, cmd_ctx->nr_lbas, geo)) {
		throw IOException("Read out of range");
	}

	device->Read(buffer, *cmd_ctx);
}

void NvmeFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	NvmeFileHandle &fh = handle.Cast<NvmeFileHandle>();
	DeviceGeometry geo = device->GetDeviceGeometry();

	idx_t cursor_offset = SeekPosition(handle);
	location += cursor_offset;
	idx_t nr_lbas = fh.CalculateRequiredLBACount(nr_bytes);

	FileMetadataStrategy *strategy = fh.GetStrategy();

	idx_t start_lba = strategy->GetLBA(fh.path, nr_bytes, location, nr_lbas, geo);
	idx_t in_block_offset = location % geo.lba_size;
	unique_ptr<CmdContext> cmd_ctx = fh.PrepareCommand(nr_bytes, start_lba, in_block_offset);

	if (!strategy->IsLBAInRange(handle.path, start_lba, cmd_ctx->nr_lbas, geo)) {
		throw IOException("Write out of range");
	}

	device->Write(buffer, *cmd_ctx);
	strategy->UpdateMetadata(*cmd_ctx);
}

int64_t NvmeFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	Read(handle, buffer, nr_bytes, 0);
	return nr_bytes;
}

int64_t NvmeFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	Write(handle, buffer, nr_bytes, 0);
	return nr_bytes;
}

bool NvmeFileSystem::CanHandleFile(const string &fpath) {
	return NvmePathHandler::IsInternalPath(fpath);
}

bool NvmeFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	if (!TryLoadMetadata()) {
		return false;
	}

	unique_ptr<FileMetadataStrategy> strategy(
	    FileStrategyFactory::GetStrategy(filename, metadata.get(), db_location, wal_location, temp_meta_manager));

	return strategy->FileExists(filename);
}

int64_t NvmeFileSystem::GetFileSize(FileHandle &handle) {
	DeviceGeometry geo = device->GetDeviceGeometry();
	NvmeFileHandle &fh = handle.Cast<NvmeFileHandle>();

	FileMetadataStrategy *strategy = fh.GetStrategy();

	idx_t nr_lbas = strategy->GetFileSizeLBA(fh.path);
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
		TemporaryFileStrategy temp_strategy(metadata.get(), temp_meta_manager);
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

void NvmeFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	unique_ptr<FileMetadataStrategy> strategy(
	    FileStrategyFactory::GetStrategy(filename, metadata.get(), db_location, wal_location, temp_meta_manager));

	strategy->RemoveFile(filename);
}

void NvmeFileSystem::Seek(FileHandle &handle, idx_t location) {
	NvmeFileHandle &nvme_handle = handle.Cast<NvmeFileHandle>();
	DeviceGeometry geo = device->GetDeviceGeometry();

	D_ASSERT(location % geo.lba_size == 0);

	unique_ptr<FileMetadataStrategy> strategy(FileStrategyFactory::GetStrategy(
	    nvme_handle.path, metadata.get(), db_location, wal_location, temp_meta_manager));

	idx_t max_seek_bound = strategy->GetSeekBound(nvme_handle.path, geo);

	if (location >= max_seek_bound) {
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

bool NvmeFileSystem::ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
                               FileOpener *opener) {
	bool dir = false;
	if (StringUtil::Equals(directory.data(), NvmePathHandler::PATH_PREFIX.data())) {
		const string db_filename_no_ext = StringUtil::GetFileStem(metadata->db_path);
		const string db_filename_with_ext = db_filename_no_ext + ".db";
		const string db_wal = db_filename_with_ext + ".wal";
		const string db_tmp = "/tmp";

		callback(db_filename_with_ext, false);
		callback(db_tmp, true);
		callback(db_wal, false);

		dir = true;
	} else if (StringUtil::Equals(directory.data(), NvmePathHandler::TMP_DIR_PATH.data())) {
		dir = true;
		TemporaryFileStrategy temp_strategy(metadata.get(), temp_meta_manager);
		temp_strategy.ListFiles(directory, callback);
	}
	return dir;
}

optional_idx NvmeFileSystem::GetAvailableDiskSpace(const string &path) {
	DeviceGeometry geo = device->GetDeviceGeometry();

	optional_idx remaining;

	if (StringUtil::Equals(path.data(), NvmePathHandler::PATH_PREFIX.data())) {
		DatabaseFileStrategy db_strategy(metadata.get(), db_location);
		WALFileStrategy wal_strategy(metadata.get(), wal_location);
		TemporaryFileStrategy temp_strategy(metadata.get(), temp_meta_manager);

		optional_idx db_avail = db_strategy.GetAvailableSpace(geo);
		optional_idx wal_avail = wal_strategy.GetAvailableSpace(geo);
		optional_idx temp_avail = temp_strategy.GetAvailableSpace(geo);

		remaining = db_avail.GetIndex() + wal_avail.GetIndex() + temp_avail.GetIndex();
	} else if (StringUtil::Equals(path.data(), NvmePathHandler::TMP_DIR_PATH.data())) {
		TemporaryFileStrategy temp_strategy(metadata.get(), temp_meta_manager);
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

bool NvmeFileSystem::TryLoadMetadata() {
	if (metadata) {
		return true;
	}

	unique_ptr<GlobalMetadata> global = ReadMetadata();
	if (global) {
		metadata = std::move(global);
		db_location.store(metadata->db_location);
		wal_location.store(metadata->wal_location);

		DeviceGeometry geo = device->GetDeviceGeometry();
		temp_meta_manager =
		    make_uniq<TemporaryFileMetadataManager>(metadata->tmp_start, geo.lba_count - 1, geo.lba_size);
		return true;
	}

	return false;
}

void NvmeFileSystem::InitializeMetadata(const string &filename) {
	if (filename.length() > 100) {
		throw IOException("Database name is too long.");
	}

	DeviceGeometry geo = device->GetDeviceGeometry();

	idx_t temp_start = (geo.lba_count - 1) - (max_temp_size / geo.lba_size);
	idx_t wal_lba_count = max_wal_size / geo.lba_size;
	idx_t wal_start = (temp_start - 1) - wal_lba_count;

	unique_ptr<GlobalMetadata> global = make_uniq<GlobalMetadata>(GlobalMetadata {});

	global->db_start = 1;
	global->wal_start = wal_start;
	global->tmp_start = temp_start;
	global->db_location = 1;
	global->wal_location = wal_start;
	global->db_path_size = filename.length();

	strncpy(global->db_path, filename.data(), filename.length());
	global->db_path[100] = '\0';

	temp_meta_manager = make_uniq<TemporaryFileMetadataManager>(temp_start, geo.lba_count - 1, geo.lba_size);

	WriteMetadata(*global);

	db_location.store(1);
	wal_location.store(wal_start);

	metadata = std::move(global);
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

	global.db_location = db_location.load();
	global.wal_location = wal_location.load();

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

} // namespace duckdb