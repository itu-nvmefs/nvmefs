#pragma once

#include "file_metadata_strategy.hpp"
#include "database_file_strategy.hpp"
#include "wal_file_strategy.hpp"
#include "temporary_file_strategy.hpp"
#include "nvmefs_path_handler.hpp"

namespace duckdb {

class FileStrategyFactory {
public:
	static unique_ptr<FileMetadataStrategy> GetStrategy(const string &filename, DatabaseRegion *region,
	                                                    GlobalMetadata *global, DatabaseRuntimeState *state,
	                                                    NvmeDevice &device,
	                                                    unique_ptr<TemporaryFileMetadataManager> &temp_manager) {
		NvmeFileType type = NvmePathHandler::GetFileType(filename);
		// TEMPORARY files aren't tied to a specific database region.
		if (type == NvmeFileType::TEMPORARY) {
			return make_uniq<TemporaryFileStrategy>(global, device, temp_manager);
		}
		// Everything else needs both a region and runtime state.
		if (!region || !state) {
			throw IOException("Attempted to access a file that does not exist: %s", filename.c_str());
		}
		switch (type) {
		case NvmeFileType::DATABASE:
			return make_uniq<DatabaseFileStrategy>(region, state->db_location);
		case NvmeFileType::WAL:
			return make_uniq<WALFileStrategy>(region, state->wal_location);
		default:
			throw InvalidInputException("Unknown file type for: %s", filename.c_str());
		}
	}
};

} // namespace duckdb
