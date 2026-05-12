#pragma once

#include "file_metadata_strategy.hpp"
#include "nvmefs.hpp"
#include <atomic>

namespace duckdb {

struct GlobalMetadata;

class WALFileStrategy : public FileMetadataStrategy {
public:
	WALFileStrategy(DatabaseRegion *region, atomic<idx_t> &wal_location)
	    : region(region), wal_location(wal_location) { }

	idx_t GetLBA(const string &filename, idx_t nr_bytes, idx_t location, idx_t nr_lbas, const DeviceGeometry &geo) override {
		return region->wal_start + (location / geo.lba_size);
	}

	bool FileExists(const string &filename) override {
		// WAL exists if it matches the database path
		return true; // WAL is always associated with the database
	}

	idx_t GetFileSizeLBA(const string &filename) override {
		return wal_location.load() - region->wal_start;
	}

	void Truncate(const string &filename, idx_t new_size) override {
		DeviceGeometry geo = {4096, 0};
		idx_t nr_lbas = (new_size + geo.lba_size - 1) / geo.lba_size;
		idx_t expected_location = wal_location.load();
		idx_t new_location = region->wal_start + nr_lbas;

		while (!wal_location.compare_exchange_weak(expected_location, new_location));
	}

	void RemoveFile(const string &filename) override {
		// Reset the location pointer to the start, effectively removing the WAL
		wal_location.store(region->wal_start);
	}

	idx_t GetSeekBound(const string &filename, const DeviceGeometry &geo) override {
		return (region->wal_end - region->wal_start) * geo.lba_size;
	}

	bool IsLBAInRange(const string &filename, idx_t start_lba, idx_t lba_count, const DeviceGeometry &geo) override {
		idx_t current_start = region->wal_start;
		idx_t current_end = region->wal_end - 1;

		if (start_lba < current_start) return false;
		if (lba_count > 0 && (start_lba + lba_count - 1) > current_end) return false;
		return true;
	}

	void UpdateMetadata(const CmdContext &context) override {
		const NvmeCmdContext &ctx = static_cast<const NvmeCmdContext &>(context);
		idx_t expected_location = wal_location.load();
		idx_t new_location = ctx.start_lba + ctx.nr_lbas;

		do {
			if (new_location < expected_location) break;
		} while (!wal_location.compare_exchange_weak(expected_location, new_location));
	}

	void CreateFile(const string &filename) override {
		// WAL file is created through metadata initialization
	}

	void ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback) override {
		// WAL files are listed at the filesystem level
	}

	optional_idx GetAvailableSpace(const DeviceGeometry &geo) override {
		idx_t wal_max_bytes = (region->wal_end - region->wal_start) * geo.lba_size;
		idx_t wal_used_bytes = (wal_location.load() - region->wal_start) * geo.lba_size;
		return wal_max_bytes - wal_used_bytes;
	}

private:
	DatabaseRegion *region;
	atomic<idx_t> &wal_location;
};

} // namespace duckdb