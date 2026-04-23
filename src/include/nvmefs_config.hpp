#pragma once

#include "duckdb.hpp"

#include <duckdb/main/secret/secret.hpp>

namespace duckdb {

struct CreateSecretInput;
class CreateSecretFunction;

struct CreateNvmefsSecretFunctions {
public:
	static void Register(ExtensionLoader &loader);
};

struct NvmeConfig {
	string device_path;
	string backend;
	string meta;
	uint64_t max_temp_size;
	uint64_t max_wal_size;
	uint64_t max_threads;
	std::unordered_map<string, uint8_t> fdp_mapping;
};

class NvmeConfigManager {
public:
	static void RegisterConfigFunctions(ExtensionLoader &loader) {
		CreateNvmefsSecretFunctions::Register(loader);
	};
	static NvmeConfig LoadConfig(DatabaseInstance &instance);

private:
	static bool IsAsynchronousBackend(const string &backend);
	static string SanatizeBackend(const string &backend);
};

} // namespace duckdb
