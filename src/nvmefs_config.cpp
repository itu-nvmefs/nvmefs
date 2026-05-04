#include "nvmefs_config.hpp"

#include "duckdb/main/settings.hpp"

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

NvmeConfig NvmeConfigManager::LoadConfig(DatabaseInstance &instance) {
	DBConfig &config = DBConfig::GetConfig(instance);

	KeyValueSecretReader secret_reader(instance, "nvmefs", "nvmefs://");

	string device;
	string backend;
	string meta;
	string fdp_mapping_str;

	// TODO: ensure that we always have value here. It is possible to not have value
	idx_t max_temp_size = 200ULL << 30; // 200 GiB
	if (config.options.maximum_swap_space != DConstants::INVALID_INDEX) {
		max_temp_size = static_cast<idx_t>(config.options.maximum_swap_space);
	}
	idx_t max_wal_size = 1ULL << 25; // 32 MiB

	idx_t max_threads = config.GetSystemMaxThreads(instance.GetFileSystem());

	secret_reader.TryGetSecretKeyOrSetting<string>("nvme_device_path", "nvme_device_path", device);
	secret_reader.TryGetSecretKeyOrSetting<string>("backend", "backend", backend);
	secret_reader.TryGetSecretKeyOrSetting<string>("meta", "meta", meta);
	secret_reader.TryGetSecretKeyOrSetting<string>("fdp_mapping", "fdp_mapping", fdp_mapping_str);

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

	return NvmeConfig {.device_path = device,
	                   .backend = backend,
	                   .meta = meta,
	                   .max_temp_size = max_temp_size,
	                   .max_wal_size = max_wal_size,
	                   .max_threads = max_threads,
	                   .fdp_mapping = fdp_mapping};
}

string NvmeConfigManager::SanatizeBackend(const string &backend) {
	if (backend.empty() || (NVMEFS_BACKENDS.find(backend) == NVMEFS_BACKENDS.end())) {
		return "nvme";
	}

	return backend;
}

} // namespace duckdb
