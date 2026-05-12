#include "nvmefs_config.hpp"

#include "duckdb/main/settings.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include <algorithm>
#include <cctype>

namespace duckdb {

const unordered_set<string> NVMEFS_BACKENDS = {"io_uring", "io_uring_cmd", "spdk",  "nvme", "libaio",  "io_ring",
                                               "iocp",     "iocp_th",      "posix", "emu",  "thrpool", "nil"};

static unique_ptr<BaseSecret> CreateNvmefsSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;

	if (scope.empty()) {
		scope.push_back("nvmefs://");
	}

	auto config = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);

	for (const auto &pair : input.options) {
		auto lower = StringUtil::Lower(pair.first);
		config->secret_map[lower] = pair.second;
	}

	return std::move(config);
}

void SetNvmefsSecretParameters(CreateSecretFunction &function) {
	function.named_parameters["nvme_device_path"] = LogicalType::VARCHAR;
	function.named_parameters["backend"] = LogicalType::VARCHAR;
	function.named_parameters["meta"] = LogicalType::VARCHAR;
	function.named_parameters["fdp_mapping"] = LogicalType::VARCHAR;
	function.named_parameters["db_configs"] = LogicalType::VARCHAR;
    function.named_parameters["default_db_size"] = LogicalType::VARCHAR;
}

void RegisterCreateNvmefsSecretFunciton(ExtensionLoader &loader) {
	string type = "nvmefs";

	SecretType secret_type;
	secret_type.name = type;
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";

	loader.RegisterSecretType(secret_type);

	CreateSecretFunction config_function = {type, "config", CreateNvmefsSecretFromConfig};
	SetNvmefsSecretParameters(config_function);
	loader.RegisterFunction(config_function);
}

void CreateNvmefsSecretFunctions::Register(ExtensionLoader &loader) {
	RegisterCreateNvmefsSecretFunciton(loader);
}

NvmeConfig NvmeConfigManager::LoadConfig(ClientContext &context, const string &secret_name) {
	DBConfig &config = DBConfig::GetConfig(context);

	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);

	if (!secret_entry) {
		throw InvalidInputException("Secret with name '%s' was not found", secret_name);
	}

	// 4. Verify it's the right type for your extension
	if (secret_entry->secret->GetType() != "nvmefs") {
		throw InvalidInputException("Secret '%s' is of type %s, but 'nvmefs' is required", secret_name,
		                            secret_entry->secret->GetType());
	}

	auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*secret_entry->secret);

	// TODO: ensure that we always have value here. It is possible to not have value

	idx_t max_temp_size = 200ULL << 30; // 200 GiB
	if (config.options.maximum_swap_space != DConstants::INVALID_INDEX) {
		max_temp_size = static_cast<idx_t>(config.options.maximum_swap_space);
	}
	idx_t max_wal_size = 1ULL << 30; // 1 GiB

	auto &instance = DatabaseInstance::GetDatabase(context);
	idx_t max_threads = config.GetSystemMaxThreads(instance.GetFileSystem());

	string device;
	string backend;
	string meta;
	string fdp_mapping_str;
	string db_configs_str;
	string default_db_size_str = "20GB";

	Value result;
	if (kv_secret.TryGetValue("nvme_device_path", result)) {
		device = result.GetValue<string>();
	}
	if (kv_secret.TryGetValue("backend", result)) {
		backend = result.GetValue<string>();
	}
	if (kv_secret.TryGetValue("meta", result)) {
		meta = result.GetValue<string>();
	}
	if (kv_secret.TryGetValue("fdp_mapping", result)) {
		fdp_mapping_str = result.GetValue<string>();
	}
	if (kv_secret.TryGetValue("db_configs", result)) {
    	db_configs_str = result.GetValue<string>();
	}
	if (kv_secret.TryGetValue("default_db_size", result)) {
    	default_db_size_str = result.GetValue<string>();
	}

	config.AddExtensionOption("nvme_device_path", "Path to NVMe device", {LogicalType::VARCHAR}, Value(device));
	config.AddExtensionOption("backend", "xnvme backend used for IO", {LogicalType::VARCHAR}, Value(backend));
	config.AddExtensionOption("meta", "Whether to print additional metadata about the device", {LogicalType::VARCHAR},
	                          Value(meta));
	config.AddExtensionOption("fdp_mapping", "FDP mapping", {LogicalType::VARCHAR}, Value(fdp_mapping_str));

	// Override with enviroment variables if they exist
	if (const char *env_dev = std::getenv("NVMEFS_DEVICE_PATH")) {
		duckdb::Printer::Print(StringUtil::Format("NVMEFS_DEVICE_PATH: %s", env_dev));
		device = env_dev;
	}
	if (const char *env_backend = std::getenv("NVMEFS_BACKEND")) {
		duckdb::Printer::Print(StringUtil::Format("NVMEFS_BACKEND: %s", env_backend));
		backend = env_backend;
	}
	if (const char *env_meta = std::getenv("NVMEFS_META")) {
		duckdb::Printer::Print(StringUtil::Format("NVMEFS_META: %s", env_meta));
		meta = env_meta;
	}
	if (const char *env_fdp = std::getenv("NVMEFS_FDP_MAPPING")) {
		duckdb::Printer::Print(StringUtil::Format("NVMEFS_FDP_MAPPING: %s", env_fdp));
		fdp_mapping_str = env_fdp;
	}

	backend = SanatizeBackend(backend);

	if (!device.empty()) {
		TempDirectorySetting::SetGlobal(&instance, config, Value("nvmefs:///tmp"));
	}

	// Parse FDP mapping
	std::unordered_map<string, uint8_t> fdp_mapping;
	auto pairs = StringUtil::Split(fdp_mapping_str, ",");
	for (const auto &pair : pairs) {
		auto kv = StringUtil::Split(pair, ":");
		if (kv.size() == 2) {
			fdp_mapping[kv[0]] = (uint8_t)std::stoul(kv[1]);
		}
	}

	// Parse DB config
	auto ParseSize = [](const string& size_str) -> idx_t {
    	idx_t multiplier = 1;
    	if (StringUtil::EndsWith(size_str, "GB")) multiplier = 1ULL << 30;
    	else if (StringUtil::EndsWith(size_str, "MB")) multiplier = 1ULL << 20;
    	return std::stoull(size_str) * multiplier;
	};

	auto RemoveWhitespace = [](string str) {
    str.erase(std::remove_if(str.begin(), str.end(), ::isspace), str.end());
    return str;
	};

	std::unordered_map<string, idx_t> db_configs;
	if (!db_configs_str.empty()) {
    	auto pairs = StringUtil::Split(db_configs_str, ",");
    	for (const auto &pair : pairs) {
        	auto kv = StringUtil::Split(pair, ":");
        	if (kv.size() == 2) {
            	string key = StringUtil::Lower(RemoveWhitespace(kv[0]));
            	db_configs[key] = ParseSize(RemoveWhitespace(kv[1]));
        	}
    	}
	}

	return NvmeConfig {.device_path = device,
	                   .backend = backend,
	                   .meta = meta,
	                   .max_temp_size = max_temp_size,
	                   .max_wal_size = max_wal_size,
	                   .max_threads = max_threads,
	                   .fdp_mapping = fdp_mapping,
					   .db_configs = db_configs,
    				   .default_db_size = ParseSize(RemoveWhitespace(default_db_size_str))
					};
}

string NvmeConfigManager::SanatizeBackend(const string &backend) {
	if (backend.empty() || (NVMEFS_BACKENDS.find(backend) == NVMEFS_BACKENDS.end())) {
		return "nvme";
	}

	return backend;
}

} // namespace duckdb
