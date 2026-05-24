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
	bool use_fdp;
	std::unordered_map<string, uint16_t> fdp_mapping;
	std::unordered_map<string, idx_t> db_configs;
	idx_t default_db_size;
};

class NvmeConfigManager {
public:
	static void RegisterConfigFunctions(ExtensionLoader &loader) {
		CreateNvmefsSecretFunctions::Register(loader);
	};
	static NvmeConfig LoadConfig(ClientContext &context, const string &secret_name);

private:
	static bool IsAsynchronousBackend(const string &backend);
	static string SanatizeBackend(const string &backend);
};

} // namespace duckdb
