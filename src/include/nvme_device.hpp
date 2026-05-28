#pragma once

#include "duckdb/common/map.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/string_util.hpp"
#include "device.hpp"
#include "nvme_io_engine.hpp"
#include "nvmefs_config.hpp"
#include "buffer_allocators/nvme_buffer_allocator.hpp"
#include <libxnvme.h>
#include <mutex>
#include <future>
#include <chrono>

namespace duckdb {

typedef void *nvme_buf_ptr;
static constexpr idx_t XNVME_QUEUE_DEPTH = 1 << 4;
static constexpr idx_t DATA_PLACEMENT_MODE = 2;

struct NvmeDeviceGeometry : public DeviceGeometry {};
struct NvmeCmdContext : public CmdContext {
	string filepath;
};

class NvmeDevice : public Device {
	friend class NvmeAsyncIOEngine;
	friend class NvmeSyncIOEngine;
	friend class NvmeAsyncThreadPollingIOEngine;
	friend class NvmeAsynPrefetchIOEngine;
	friend class TemporaryFileStrategy;

public:
	NvmeDevice(const NvmeConfig &config);
	~NvmeDevice();

	/// @brief Writes data from the input buffer to the device at the specified LBA position
	void Write(void *buffer, const CmdContext &context) override;

	/// @brief Reads data from the device at the specified LBA position into the output buffer
	void Read(void *buffer, const CmdContext &context) override;

	/// @brief Fetches the geometry of the device
	/// @return The device geometry
	DeviceGeometry GetDeviceGeometry() override;

	/// @brief Get the name of the device
	/// @return Name of device
	string GetName() const {
		return "NvmeDevice";
	}

private:
	/// @brief Determines which placement handler should be used for the given path
	/// @param path The path of the file that will be opened
	/// @return A placement identifier
	uint8_t GetReclaimUnitHandleOrDefault(const string &path);

	/// @brief Allocates a device specific buffer. Should be freed with FreeDeviceBuffer.
	/// @param nr_bytes The number of bytes to allocate
	/// @return Pointer to allocated device buffer
	nvme_buf_ptr AllocateDeviceBuffer(idx_t nr_bytes);

	/// @brief Frees the given device buffer
	/// @param buffer The device buffer to free
	/// @param size The size of the buffer
	void FreeDeviceBuffer(nvme_buf_ptr buffer, idx_t size);

	/// @brief Loads the geometry of the device
	/// @return The device geometry
	DeviceGeometry LoadDeviceGeometry();

	/// @brief Specifies the backend and sync/async used for the device
	/// @param opts xNVMe options
	void PrepareOpts(xnvme_opts &opts);

	static void CommandCallback(struct xnvme_cmd_ctx *ctx, void *cb_args);

	void PrepareIOCmdContext(xnvme_cmd_ctx *ctx, const CmdContext &cmd_ctx, uint16_t ruh, idx_t dtype, bool write);
	bool CheckFDP();
	void InitializePlacementHandles();
	idx_t GetThreadIndex();

private:
	map<string, uint16_t> allocated_ruhs;
	map<uint16_t, idx_t> ruhs_to_phids;
	xnvme_dev *device;
	const string dev_path;
	DeviceGeometry geometry;
	const string backend;
	bool use_fdp;
	vector<xnvme_queue *> queues;
	const idx_t max_threads;
	atomic<idx_t> thread_id_counter;
	static thread_local optional_idx index;

	unique_ptr<NvmeIOEngine> io_engine;
	unique_ptr<NvmeBufferAllocator> buffer_allocator;
};

} // namespace duckdb
