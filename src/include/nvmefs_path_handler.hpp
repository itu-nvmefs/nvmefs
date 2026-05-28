#pragma once
#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

enum class NvmeFileType : uint8_t { DATABASE, WAL, TEMPORARY, GLOBAL_METADATA, UNKNOWN };

class NvmePathHandler {
public:
	static inline constexpr char MAGIC_BYTES[] = "NVMEFS";
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

	static string ExtractDatabaseName(const string &full_path) {
		string name = full_path;
		if (StringUtil::StartsWith(name, PATH_PREFIX)) {
			name = name.substr(PATH_PREFIX.length());
		}
		if (StringUtil::EndsWith(name, ".checkpoint")) {
			name = name.substr(0, name.length() - 11);
		}
		if (StringUtil::EndsWith(name, ".wal")) {
			name = name.substr(0, name.length() - 4);
		}
		if (StringUtil::EndsWith(name, ".db")) {
			name = name.substr(0, name.length() - 3);
		}
		return name;
	}

	static string ExtractTemporarySize(const string &full_path) {
		size_t start_size = full_path.find_last_of('_') + 1;
		size_t end_size = full_path.find('-', start_size);
		string block_size_str = full_path.substr(start_size, end_size - start_size);
		if (block_size_str == "DEFAULT") {
			return "256";
		}
		return block_size_str.substr(1, block_size_str.length() - 2);
	}
};

inline const string NvmePathHandler::PATH_PREFIX = "nvmefs://";
inline const string NvmePathHandler::TMP_DIR_PATH = "nvmefs:///tmp";
inline const string NvmePathHandler::GLOBAL_METADATA_PATH = "nvmefs://.global_metadata";

} // namespace duckdb
