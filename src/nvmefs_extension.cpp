#define DUCKDB_EXTENSION_MAIN

#include "nvmefs_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
struct ConfigPrintFunctionData : public TableFunctionData {
	ConfigPrintFunctionData() {
	}

	bool finished = false;
};

static void ConfigPrint(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<ConfigPrintFunctionData>();

	if (data.finished) {
		return;
	}

	vector<string> settings {"nvme_device_path", "temp_directory", "backend", "worker_threads", "meta"};
	idx_t chunk_count = 0;

	for (string &setting : settings) {
		Value current_value;
		context.TryGetCurrentSetting(setting, current_value);
		output.SetValue(0, chunk_count, Value(setting));
		output.SetValue(1, chunk_count, current_value);
		chunk_count++;
	}

	output.SetCardinality(chunk_count);

	data.finished = true;
}

static unique_ptr<FunctionData> ConfigPrintBind(ClientContext &ctx, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("Setting");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("Value");
	return_types.emplace_back(LogicalType::VARCHAR);

	auto result = make_uniq<ConfigPrintFunctionData>();
	result->finished = false;

	return std::move(result);
}

// From nvmefs.cpp
extern const std::atomic<uint64_t> nvmefs_total_spill_bytes;
extern const std::atomic<uint64_t> nvmefs_total_wal_bytes;
extern const std::atomic<uint64_t> nvmefs_current_wal_bytes;
extern const std::atomic<uint64_t> nvmefs_peak_wal_bytes;
extern const std::atomic<uint64_t> nvmefs_total_db_bytes;
extern const std::atomic<uint64_t> nvmefs_current_db_bytes;
extern const std::atomic<uint64_t> nvmefs_peak_db_bytes;

// From temporary_file_metadata_manager.cpp
extern const std::atomic<int64_t> nvmefs_active_temp_files;
extern const std::atomic<int64_t> nvmefs_peak_temp_files;

// From nvmefs_temporary_block_manager.cpp
extern const std::atomic<uint64_t> nvmefs_active_temp_bytes;
extern const std::atomic<uint64_t> nvmefs_peak_temp_bytes;

static void MetricsPrint(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<ConfigPrintFunctionData>();
	if (data.finished) {
		return;
	}

	idx_t row_idx = 0;

	output.SetValue(0, row_idx, Value("total_spill_bytes"));
	output.SetValue(1, row_idx, Value::UBIGINT(nvmefs_total_spill_bytes.load()));
	row_idx++;

	output.SetValue(0, row_idx, Value("total_wal_bytes"));
	output.SetValue(1, row_idx, Value::UBIGINT(nvmefs_total_wal_bytes.load()));
	row_idx++;

	output.SetValue(0, row_idx, Value("current_wal_bytes"));
	output.SetValue(1, row_idx, Value::UBIGINT(nvmefs_current_wal_bytes.load()));
	row_idx++;

	output.SetValue(0, row_idx, Value("peak_wal_bytes"));
	output.SetValue(1, row_idx, Value::UBIGINT(nvmefs_peak_wal_bytes.load()));
	row_idx++;

	output.SetValue(0, row_idx, Value("active_temp_files"));
	output.SetValue(1, row_idx, Value::BIGINT(nvmefs_active_temp_files.load()));
	row_idx++;

	output.SetValue(0, row_idx, Value("peak_temp_files"));
	output.SetValue(1, row_idx, Value::BIGINT(nvmefs_peak_temp_files.load()));
	row_idx++;

	output.SetValue(0, row_idx, Value("active_temp_bytes"));
	output.SetValue(1, row_idx, Value::UBIGINT(nvmefs_active_temp_bytes.load()));
	row_idx++;

	output.SetValue(0, row_idx, Value("peak_temp_bytes"));
	output.SetValue(1, row_idx, Value::UBIGINT(nvmefs_peak_temp_bytes.load()));
	row_idx++;

	output.SetValue(0, row_idx, Value("total_db_bytes"));
	output.SetValue(1, row_idx, Value::UBIGINT(nvmefs_total_db_bytes.load()));
	row_idx++;

	output.SetValue(0, row_idx, Value("current_db_bytes"));
	output.SetValue(1, row_idx, Value::UBIGINT(nvmefs_current_db_bytes.load()));
	row_idx++;

	output.SetValue(0, row_idx, Value("peak_db_bytes"));
	output.SetValue(1, row_idx, Value::UBIGINT(nvmefs_peak_db_bytes.load()));
	row_idx++;

	output.SetCardinality(row_idx);
	data.finished = true;
}

static void ActivatePragma(ClientContext &context, const FunctionParameters &parameters) {
	auto secret_name = parameters.values[0].ToString();

	auto &instance = DatabaseInstance::GetDatabase(context);
	NvmeConfig nvmeConfig = NvmeConfigManager::LoadConfig(context, secret_name);

	// Add extension options
	if (!nvmeConfig.device_path.empty()) {
		auto &fs = instance.GetFileSystem();
		auto nvmefs_ptr = make_uniq<NvmeFileSystem>(nvmeConfig);

		fs.RegisterSubSystem(std::move(nvmefs_ptr));
	} else {
		duckdb::Printer::Print(
		    "[nvmefs] Extension activated but no nvme_device_path specified. NvmeFileSystem will not be registered.");
		duckdb::Printer::Print(duckdb::StringUtil::Format(
		    "[nvmefs] To use the NvmeFileSystem, set the nvme_device_path configuration option in the '%s' secret to "
		    "the path of the NVMe device and restart the database.",
		    secret_name));
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	NvmeConfigManager::RegisterConfigFunctions(loader);

	auto pragma = PragmaFunction::PragmaCall("activate_nvmefs", ActivatePragma, {LogicalType::VARCHAR});
	loader.RegisterFunction(pragma);

	TableFunction config_print_function("print_config", {}, ConfigPrint, ConfigPrintBind);
	loader.RegisterFunction(config_print_function);

	TableFunction metrics_print_function("print_nvmefs_metrics", {}, MetricsPrint, ConfigPrintBind);
	loader.RegisterFunction(metrics_print_function);
}

void NvmefsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string NvmefsExtension::Name() {
	return "nvmefs";
}

std::string NvmefsExtension::Version() const {
#ifdef EXT_VERSION_NVMEFS
	return EXT_VERSION_NVMEFS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(nvmefs, loader) {
	duckdb::LoadInternal(loader);
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
