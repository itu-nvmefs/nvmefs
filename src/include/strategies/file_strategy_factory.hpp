#pragma once

#include "file_metadata_strategy.hpp"
#include "database_file_strategy.hpp"
#include "wal_file_strategy.hpp"
#include "temporary_file_strategy.hpp"
#include "nvmefs_path_handler.hpp"

namespace duckdb {

class FileStrategyFactory {
public:
    static FileMetadataStrategy* GetStrategy(const string &filename,
                                             GlobalMetadata *metadata,
                                             atomic<idx_t> &db_location,
                                             atomic<idx_t> &wal_location,
                                             unique_ptr<TemporaryFileMetadataManager> &temp_manager) {
        NvmeFileType type = NvmePathHandler::GetFileType(filename);

        switch (type) {
        case NvmeFileType::DATABASE:
            return new DatabaseFileStrategy(metadata, db_location);
        case NvmeFileType::WAL:
            return new WALFileStrategy(metadata, wal_location);
        case NvmeFileType::TEMPORARY:
            return new TemporaryFileStrategy(metadata, temp_manager);
        default:
            throw InvalidInputException("Unknown file type for: %s", filename);
        }
    }
};

} // namespace duckdb