#pragma once

#include "file_metadata_strategy.hpp"
#include "temporary_file_metadata_manager.hpp"
#include "nvmefs.hpp"

namespace duckdb {

struct GlobalMetadata;

class TemporaryFileStrategy : public FileMetadataStrategy {
public:
    TemporaryFileStrategy(GlobalMetadata *metadata, unique_ptr<TemporaryFileMetadataManager> &temp_manager)
        : metadata(metadata), temp_manager(temp_manager) {}

    idx_t GetLBA(const string &filename, idx_t nr_bytes, idx_t location, idx_t nr_lbas, const DeviceGeometry &geo) override {
        return temp_manager->GetLBA(filename, location, nr_lbas);
    }

    bool FileExists(const string &filename) override {
        return temp_manager->FileExists(filename);
    }

    idx_t GetFileSizeLBA(const string &filename) override {
        return temp_manager->GetFileSizeLBA(filename);
    }

    void Truncate(const string &filename, idx_t new_size) override {
        temp_manager->TruncateFile(filename, new_size);
    }

    void RemoveFile(const string &filename) override {
        temp_manager->DeleteFile(filename);
    }

    idx_t GetSeekBound(const string &filename, const DeviceGeometry &geo) override {
        return temp_manager->GetFileSizeLBA(filename) * geo.lba_size;
    }

    bool IsLBAInRange(const string &filename, idx_t start_lba, idx_t lba_count, const DeviceGeometry &geo) override {
        idx_t current_start = metadata->tmp_start;
        idx_t current_end = geo.lba_count - 1;

        if (start_lba < current_start) return false;
            
        // Fix: Subtract 1
        if (lba_count > 0 && (start_lba + lba_count - 1) > current_end) {
            return false;
        }

        return true;
    }

    void UpdateMetadata(const CmdContext &context) override {
        const NvmeCmdContext &ctx = static_cast<const NvmeCmdContext &>(context);
        temp_manager->MoveLBALocation(ctx.filepath, ctx.start_lba + ctx.nr_lbas);
    }

    void CreateFile(const string &filename) override {
        temp_manager->CreateFile(filename);
    }

    void ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback) override {
        temp_manager->ListFiles(directory, callback);
    }

    optional_idx GetAvailableSpace(const DeviceGeometry &geo) override {
        return temp_manager->GetAvailableSpace(geo.lba_count, metadata->tmp_start);
    }

    void ClearAll() {
        temp_manager->Clear();
    }

private:
    GlobalMetadata *metadata;
    unique_ptr<TemporaryFileMetadataManager> &temp_manager;
};

} // namespace duckdb