#pragma once

#include "duckdb.hpp"

namespace duckdb {

struct DeviceGeometry {
	idx_t lba_size;
	idx_t lba_count;
};

struct CmdContext {
	virtual ~CmdContext() = default;

	idx_t nr_bytes;
	idx_t nr_lbas;
	idx_t start_lba;
	idx_t offset;
};

class Device {
public:
	virtual ~Device() = default;

	virtual void Write(void *buffer, const CmdContext &context);
	virtual void Read(void *buffer, const CmdContext &context);

	virtual DeviceGeometry GetDeviceGeometry();

	virtual string GetName() const = 0;
};

} // namespace duckdb
