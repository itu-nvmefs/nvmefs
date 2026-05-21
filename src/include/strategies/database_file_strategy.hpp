#pragma once

#include "file_metadata_strategy.hpp"
#include "nvmefs.hpp"
#include <atomic>

namespace duckdb {

struct GlobalMetadata;

class DatabaseFileStrategy : public FileMetadataStrategy {
public:
	DatabaseFileStrategy(DatabaseRegion *region, atomic<idx_t> &db_location)
	    : region(region), db_location(db_location) {
	}

	idx_t GetLBA(const string &filename, idx_t nr_bytes, idx_t location, idx_t nr_lbas,
	             const DeviceGeometry &geo) override {
		return region->db_start + (location / geo.lba_size);
	}

	bool FileExists(const string &filename) override {
		return (db_location.load() - region->db_start) > 0;
	}

	idx_t GetFileSizeLBA(const string &filename) override {
		return db_location.load() - region->db_start;
	}

	void Truncate(const string &filename, idx_t new_size) override {
		DeviceGeometry geo = {4096, 0};
		idx_t nr_lbas = (new_size + geo.lba_size - 1) / geo.lba_size;
		idx_t expected_location = db_location.load();
		idx_t new_location = region->db_start + nr_lbas;

		if (new_location > region->wal_start) {
            throw IOException("DB Truncate failed: Exceeded into WAL region.");
        }

		while (!db_location.compare_exchange_weak(expected_location, new_location))
			;
	}

	void RemoveFile(const string &filename) override {
		// Database files cannot be removed individually
		// Only reset is supported through metadata
	}

	idx_t GetSeekBound(const string &filename, const DeviceGeometry &geo) override {
		return (region->wal_start - region->db_start) * geo.lba_size;
	}

	bool IsLBAInRange(const string &filename, idx_t start_lba, idx_t lba_count, const DeviceGeometry &geo) override {
		idx_t current_start = region->db_start;
		idx_t current_end = region->wal_start - 1;

		if (start_lba < current_start)
			return false;
		if (lba_count > 0 && (start_lba + lba_count - 1) > current_end)
			return false;
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
		idx_t db_max_bytes = (region->wal_start - region->db_start) * geo.lba_size;
		idx_t db_used_bytes = (db_location.load() - region->db_start) * geo.lba_size;
		return db_max_bytes - db_used_bytes;
	}

private:
	DatabaseRegion *region;
	atomic<idx_t> &db_location;
};

} // namespace duckdb