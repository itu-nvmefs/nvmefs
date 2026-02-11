#pragma once

#include "file_metadata_strategy.hpp"
#include "nvmefs.hpp"
#include <atomic>

namespace duckdb {

struct GlobalMetadata;

class DatabaseFileStrategy : public FileMetadataStrategy {
public:
	DatabaseFileStrategy(GlobalMetadata *metadata, atomic<idx_t> &db_location)
	    : metadata(metadata), db_location(db_location) {
		if (!metadata) {
			throw InternalException("DatabaseFileStrategy initialized with null metadata");
		}
	}

	idx_t GetLBA(const string &filename, idx_t nr_bytes, idx_t location, idx_t nr_lbas,
	             const DeviceGeometry &geo) override {
		idx_t lba_offset = location / geo.lba_size;
		return metadata->db_start + lba_offset;
	}

	bool FileExists(const string &filename) override {
		idx_t start_lba = metadata->db_start;
		idx_t location_lba = db_location.load();
		return (location_lba - start_lba) > 0;
	}

	idx_t GetFileSizeLBA(const string &filename) override {
		return db_location.load() - metadata->db_start;
	}

	void Truncate(const string &filename, idx_t new_size) override {
		DeviceGeometry geo = {4096, 0};
		idx_t nr_lbas = (new_size + geo.lba_size - 1) / geo.lba_size;
		idx_t expected_location = db_location.load();
		idx_t new_location = metadata->db_start + nr_lbas;

		while (!db_location.compare_exchange_weak(expected_location, new_location))
			;
	}

	void RemoveFile(const string &filename) override {
		// Database files cannot be removed individually
		// Only reset is supported through metadata
	}

	idx_t GetSeekBound(const string &filename, const DeviceGeometry &geo) override {
		return ((metadata->wal_start - 1) - metadata->db_start) * geo.lba_size;
	}

	bool IsLBAInRange(const string &filename, idx_t start_lba, idx_t lba_count, const DeviceGeometry &geo) override {
		idx_t current_start = metadata->db_start;
		idx_t current_end = metadata->wal_start - 1;

		if (start_lba < current_start) {
			return false;
		}

		// Fix: Subtract 1 from the sum to get the last LBA actually being written
		if (lba_count > 0 && (start_lba + lba_count - 1) > current_end) {
			return false;
		}

		return true;
	}

	void UpdateMetadata(const CmdContext &context) override {
		const NvmeCmdContext &ctx = static_cast<const NvmeCmdContext &>(context);
		idx_t expected_location = db_location.load();
		idx_t new_location = ctx.start_lba + ctx.nr_lbas;

		do {
			if (new_location < expected_location) {
				break;
			}
		} while (!db_location.compare_exchange_weak(expected_location, new_location));
	}

	void CreateFile(const string &filename) override {
		// Database file is created through metadata initialization
	}

	void ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback) override {
		// Database files are listed at the filesystem level
	}

	optional_idx GetAvailableSpace(const DeviceGeometry &geo) override {
		idx_t db_max_bytes = ((metadata->wal_start - 1) - metadata->db_start) * geo.lba_size;
		idx_t db_used_bytes = (db_location.load() - metadata->db_start) * geo.lba_size;
		return db_max_bytes - db_used_bytes;
	}

private:
	GlobalMetadata *metadata;
	atomic<idx_t> &db_location;
};

} // namespace duckdb