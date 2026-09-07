#pragma once

#include <arrow/io/interfaces.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace duckdb {

// Minimal Arrow RandomAccessFile backed by pread(2). This adapter is used only
// to inspect Parquet metadata before the io_uring data path takes over.
class ArrowRandomAccessFile : public arrow::io::RandomAccessFile {
public:
    ~ArrowRandomAccessFile() override;

    static arrow::Result<std::shared_ptr<ArrowRandomAccessFile>>
    Open(const std::string &path);

    arrow::Status Close() override;
    bool closed() const override;
    arrow::Result<int64_t> Tell() const override;
    arrow::Status Seek(int64_t position) override;
    arrow::Result<int64_t> Read(int64_t nbytes, void *out) override;
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t nbytes) override;
    arrow::Result<int64_t> GetSize() override;
    arrow::Result<int64_t> ReadAt(int64_t position, int64_t nbytes, void *out) override;
    arrow::Result<std::shared_ptr<arrow::Buffer>> ReadAt(int64_t position,
                                                         int64_t nbytes) override;

private:
    explicit ArrowRandomAccessFile(int fd, int64_t size);

    int fd_;
    int64_t file_size_;
    std::atomic<int64_t> pos_ {0};
    std::atomic<bool> closed_ {false};
};

} // namespace duckdb
