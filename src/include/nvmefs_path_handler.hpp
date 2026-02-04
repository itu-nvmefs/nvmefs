#pragma once
#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

enum class NvmeFileType : uint8_t { 
    DATABASE, 
    WAL, 
    TEMPORARY, 
    GLOBAL_METADATA, 
    UNKNOWN 
};

class NvmePathHandler {
public:
    static constexpr char MAGIC_BYTES[] = "NVMEFS";
    static constexpr idx_t GLOBAL_METADATA_LOCATION = 0;
    static const string PATH_PREFIX;
    static const string TMP_DIR_PATH;
    static const string GLOBAL_METADATA_PATH;

    static NvmeFileType GetFileType(const string &path) {
        if (path == GLOBAL_METADATA_PATH) {
            return NvmeFileType::GLOBAL_METADATA;
        }
        if (StringUtil::Contains(path, ".wal")) {
            return NvmeFileType::WAL;
        } 
        if (StringUtil::Contains(path, "/tmp")) {
            return NvmeFileType::TEMPORARY;
        } 
        if (StringUtil::Contains(path, ".db")) {
            return NvmeFileType::DATABASE;
        } 
        return NvmeFileType::UNKNOWN;
    }

    static bool IsInternalPath(const string &path) {
        return StringUtil::StartsWith(path, PATH_PREFIX);
    }
    
    static string GetStem(const string &path) {
        return StringUtil::GetFileStem(path);
    }
};

inline const string NvmePathHandler::PATH_PREFIX = "nvmefs://";
inline const string NvmePathHandler::TMP_DIR_PATH = "nvmefs:///tmp";
inline const string NvmePathHandler::GLOBAL_METADATA_PATH = "nvmefs://.global_metadata";

} // namespace duckdb