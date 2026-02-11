#include "fake_device.hpp"

namespace duckdb {
FakeDevice::FakeDevice(idx_t lba_count, idx_t lba_size)
    : Device(), geometry(DeviceGeometry {lba_size, lba_count}), memory(new uint8_t[lba_size * lba_count]) {
}

FakeDevice::~FakeDevice() {
	delete[] memory;
}

void FakeDevice::Write(void *buffer, const CmdContext &context) {
	D_ASSERT(context.start_lba + context.nr_lbas <= geometry.lba_count);

	// Get pointer to the start of the requested memory location
	idx_t start_location_bytes = context.start_lba * geometry.lba_size + context.offset;
	uint8_t *mem_ptr = memory + start_location_bytes;

	// Write the data to in-memory device
	memcpy(mem_ptr, buffer, context.nr_bytes);
}

void FakeDevice::Read(void *buffer, const CmdContext &context) {
	D_ASSERT(context.start_lba + context.nr_lbas <= geometry.lba_count);

	// Get pointer to the start of the requested memory location
	idx_t start_location_bytes = context.start_lba * geometry.lba_size + context.offset;
	uint8_t *mem_ptr = memory + start_location_bytes;

	// Read the data from in-memory device to the buffer
	memcpy(buffer, mem_ptr, context.nr_bytes);
}

DeviceGeometry FakeDevice::GetDeviceGeometry() {
	return geometry;
}
} // namespace duckdb
