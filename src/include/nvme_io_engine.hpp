#pragma once

namespace duckdb {

class NvmeDevice;
struct NvmeCmdContext;

class NvmeIOEngine {
public:
	explicit NvmeIOEngine(NvmeDevice &device) : device(device) {
	}
	virtual ~NvmeIOEngine() = default;

	virtual void Read(void *buffer, const NvmeCmdContext &context) = 0;
	virtual void Write(void *buffer, const NvmeCmdContext &context) = 0;

protected:
	NvmeDevice &device;
};

} // namespace duckdb