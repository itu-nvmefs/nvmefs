#pragma once

#include "duckdb.hpp"
#include "device.hpp"

namespace duckdb {

class FileMetadataStrategy {
public:
	virtual ~FileMetadataStrategy() = default;

	/// @brief Gets the LBA (Logical Block Address) for a file operation
	/// @param filename The file name
	/// @param nr_bytes Number of bytes to read/write
	/// @param location Offset in the file
	/// @param nr_lbas Number of LBAs needed
	/// @return The starting LBA
	virtual idx_t GetLBA(const string &filename, idx_t nr_bytes, idx_t location, idx_t nr_lbas,
	                     const DeviceGeometry &geo) = 0;

	/// @brief Checks if the file exists
	/// @param filename The file name
	/// @return True if the file exists
	virtual bool FileExists(const string &filename) = 0;

	/// @brief Gets the file size in LBAs
	/// @param filename The file name
	/// @return File size in LBAs
	virtual idx_t GetFileSizeLBA(const string &filename) = 0;

	/// @brief Truncates the file to a new size
	/// @param filename The file name
	/// @param new_size New size in bytes
	virtual void Truncate(const string &filename, idx_t new_size) = 0;

	/// @brief Removes the file
	/// @param filename The file name
	virtual void RemoveFile(const string &filename) = 0;

	/// @brief Gets the maximum seek bound for the file
	/// @param filename The file name
	/// @param geo Device geometry
	/// @return Maximum seek position in bytes
	virtual idx_t GetSeekBound(const string &filename, const DeviceGeometry &geo) = 0;

	/// @brief Checks if LBA range is valid for this file type
	/// @param filename The file name
	/// @param start_lba Starting LBA
	/// @param lba_count Number of LBAs
	/// @param geo Device geometry
	/// @return True if range is valid
	virtual bool IsLBAInRange(const string &filename, idx_t start_lba, idx_t lba_count, const DeviceGeometry &geo) = 0;

	/// @brief Updates metadata after a write operation
	/// @param context Command context with write information
	virtual void UpdateMetadata(const CmdContext &context) = 0;

	/// @brief Creates a file if needed
	/// @param filename The file name
	virtual void CreateFile(const string &filename) = 0;

	/// @brief Lists files in the directory
	/// @param directory Directory path
	/// @param callback Callback function for each file
	virtual void ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback) = 0;

	/// @brief Gets available disk space for this file type
	/// @param geo Device geometry
	/// @return Available space in bytes
	virtual optional_idx GetAvailableSpace(const DeviceGeometry &geo) = 0;
};

} // namespace duckdb