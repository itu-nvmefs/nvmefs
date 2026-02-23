#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/map.hpp"

#include "device.hpp"
#include "nvme_device.hpp"
#include "nvmefs_config.hpp"
#include "temporary_file_metadata_manager.hpp"
#include "nvmefs_path_handler.hpp"
#include "strategies/file_metadata_strategy.hpp"

namespace duckdb {

struct GlobalMetadata {
	uint64_t db_path_size;
	char db_path[101];

	uint64_t db_start;
	uint64_t wal_start;
	uint64_t tmp_start;

	uint64_t db_location;
	uint64_t wal_location;
};

struct TemporaryFileMetadata {
	uint64_t block_size;
	map<idx_t, TemporaryBlock *> block_map;
};

class NvmeFileHandle : public FileHandle {

	friend class NvmeFileSystem;

public:
	NvmeFileHandle(FileSystem &file_system, string path, FileOpenFlags flags,
	               unique_ptr<FileMetadataStrategy> strategy_p);
	~NvmeFileHandle() = default;

	void Read(void *buffer, idx_t nr_bytes, idx_t location);
	void Write(void *buffer, idx_t nr_bytes, idx_t location);

	idx_t GetFileSize();
	void Sync();

	void Close() override;

	inline FileMetadataStrategy *GetStrategy() {
		return strategy.get();
	}

private:
	unique_ptr<CmdContext> PrepareCommand(idx_t nr_bytes, idx_t start_lba, idx_t offset);

	/// @brief Calculates the amount of LBAs required to store the given number of bytes
	/// @param nr_bytes The number of bytes to store
	/// @return The number of LBAs required to store the given number of bytes
	idx_t CalculateRequiredLBACount(idx_t nr_bytes);

	void SetFilePointer(idx_t location);
	idx_t GetFilePointer();

private:
	unique_ptr<FileMetadataStrategy> strategy;
	idx_t cursor_offset;
};

class NvmeFileSystem : public FileSystem {
public:
	NvmeFileSystem(NvmeConfig config);
	NvmeFileSystem(NvmeConfig config, unique_ptr<Device> device);
	~NvmeFileSystem();

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes);
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes);
	bool CanHandleFile(const string &fpath) override;
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	int64_t GetFileSize(FileHandle &handle) override;
	void FileSync(FileHandle &handle) override;
	bool OnDiskFile(FileHandle &handle) override;
	void Truncate(FileHandle &handle, int64_t new_size) override;
	bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	void Seek(FileHandle &handle, idx_t location) override;
	void Reset(FileHandle &handle);
	idx_t SeekPosition(FileHandle &handle) override;
	bool ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
	               FileOpener *opener = nullptr);
	optional_idx GetAvailableDiskSpace(const string &path);
	bool Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) override;

	Device &GetDevice();

	string GetName() const {
		return "NvmeFileSystem";
	}

private:
	bool TryLoadMetadata();
	void InitializeMetadata(const string &filename);
	unique_ptr<GlobalMetadata> ReadMetadata();
	void WriteMetadata(GlobalMetadata &global);

private:
	Allocator &allocator;
	unique_ptr<GlobalMetadata> metadata;
	unique_ptr<Device> device;
	unique_ptr<TemporaryFileMetadataManager> temp_meta_manager;
	atomic<idx_t> db_location;
	atomic<idx_t> wal_location;
	idx_t max_temp_size;
	idx_t max_wal_size;
	static std::recursive_mutex temp_lock;
};
} // namespace duckdb
