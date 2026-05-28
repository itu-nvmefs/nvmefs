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

static std::shared_ptr<NvmeMetricsState> global_metrics = std::make_shared<NvmeMetricsState>();

struct NvmeMetricsBindData : public TableFunctionData {
	std::shared_ptr<NvmeMetricsState> metrics;
	bool finished = false;

	explicit NvmeMetricsBindData(std::shared_ptr<NvmeMetricsState> m) : metrics(std::move(m)) {
	}
};

static void EmitRow(DataChunk &output, idx_t &row, const string &name, uint64_t value) {
	output.SetValue(0, row, Value(name));
	output.SetValue(1, row, Value::UBIGINT(value));
	row++;
}

static void EmitRow(DataChunk &output, idx_t &row, const string &name, int64_t value) {
	output.SetValue(0, row, Value(name));
	output.SetValue(1, row, Value::BIGINT(value));
	row++;
}

static unique_ptr<FunctionData> MetricsBind(ClientContext &ctx, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("Setting");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("Value");
	return_types.emplace_back(LogicalType::UBIGINT);

	auto result = make_uniq<NvmeMetricsBindData>(global_metrics);
	return std::move(result);
}

extern std::atomic<uint64_t> nvmefs_active_temp_bytes;
extern std::atomic<uint64_t> nvmefs_peak_temp_bytes;
extern std::atomic<int64_t> nvmefs_active_temp_files;
extern std::atomic<int64_t> nvmefs_peak_temp_files;

static void MetricsPrint(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<NvmeMetricsBindData>();
	if (data.finished)
		return;

	auto metrics = data.metrics;
	idx_t row = 0;

	EmitRow(output, row, "shared_temp.total_spill_bytes", metrics->total_spill_bytes.load(std::memory_order_relaxed));
	EmitRow(output, row, "shared_temp.active_temp_bytes", nvmefs_active_temp_bytes.load(std::memory_order_relaxed));
	EmitRow(output, row, "shared_temp.peak_temp_bytes", nvmefs_peak_temp_bytes.load(std::memory_order_relaxed));
	EmitRow(output, row, "shared_temp.active_temp_files", nvmefs_active_temp_files.load(std::memory_order_relaxed));
	EmitRow(output, row, "shared_temp.peak_temp_files", nvmefs_peak_temp_files.load(std::memory_order_relaxed));

	std::lock_guard<std::mutex> lock(metrics->db_lock);
	for (auto &kv : metrics->per_db) {
		auto &db_name = kv.first;
		auto &m = *kv.second;

		EmitRow(output, row, db_name + ".total_wal_bytes", m.total_wal_bytes.load(std::memory_order_relaxed));
		EmitRow(output, row, db_name + ".current_wal_bytes", m.current_wal_bytes.load(std::memory_order_relaxed));
		EmitRow(output, row, db_name + ".peak_wal_bytes", m.peak_wal_bytes.load(std::memory_order_relaxed));

		EmitRow(output, row, db_name + ".total_db_bytes", m.total_db_bytes.load(std::memory_order_relaxed));
		EmitRow(output, row, db_name + ".current_db_bytes", m.current_db_bytes.load(std::memory_order_relaxed));
		EmitRow(output, row, db_name + ".peak_db_bytes", m.peak_db_bytes.load(std::memory_order_relaxed));
	}
	output.SetCardinality(row);
	data.finished = true;
}

static void ActivatePragma(ClientContext &context, const FunctionParameters &parameters) {
	auto secret_name = parameters.values[0].ToString();

	auto &instance = DatabaseInstance::GetDatabase(context);
	NvmeConfig nvmeConfig = NvmeConfigManager::LoadConfig(context, secret_name);

	// Add extension options
	if (!nvmeConfig.device_path.empty()) {
		auto &fs = instance.GetFileSystem();
		auto nvmefs_ptr = make_uniq<NvmeFileSystem>(nvmeConfig, global_metrics);

		fs.RegisterSubSystem(std::move(nvmefs_ptr));
	} else {
		duckdb::Printer::Print("[nvmefs] Extension activated but no nvme_device_path specified. NvmeFileSystem "
		                       "will not be registered.");
		duckdb::Printer::Print(
		    duckdb::StringUtil::Format("[nvmefs] To use the NvmeFileSystem, set the nvme_device_path configuration "
		                               "option in the '%s' secret to "
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

	TableFunction metrics_print_function("print_nvmefs_metrics", {}, MetricsPrint, MetricsBind);
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
