#include "ArrowRandomAccessFile.hpp"

#include <arrow/buffer.h>
#include <arrow/memory_pool.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace duckdb {

ArrowRandomAccessFile::ArrowRandomAccessFile(int fd, int64_t size)
    : fd_(fd), file_size_(size) {
}

ArrowRandomAccessFile::~ArrowRandomAccessFile() {
    if (!closed_.exchange(true)) {
        ::close(fd_);
    }
}

arrow::Result<std::shared_ptr<ArrowRandomAccessFile>>
ArrowRandomAccessFile::Open(const std::string &path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return arrow::Status::IOError("Cannot open '", path, "': ", std::strerror(errno));
    }
    struct stat st;
    if (::fstat(fd, &st) < 0) {
        ::close(fd);
        return arrow::Status::IOError("fstat failed: ", std::strerror(errno));
    }
    return std::shared_ptr<ArrowRandomAccessFile>(
        new ArrowRandomAccessFile(fd, static_cast<int64_t>(st.st_size)));
}

arrow::Status ArrowRandomAccessFile::Close() {
    if (!closed_.exchange(true) && ::close(fd_) < 0) {
        return arrow::Status::IOError("close failed: ", std::strerror(errno));
    }
    return arrow::Status::OK();
}

bool ArrowRandomAccessFile::closed() const {
    return closed_.load();
}

arrow::Result<int64_t> ArrowRandomAccessFile::Tell() const {
    return pos_.load(std::memory_order_relaxed);
}

arrow::Status ArrowRandomAccessFile::Seek(int64_t position) {
    pos_.store(position, std::memory_order_relaxed);
    return arrow::Status::OK();
}

arrow::Result<int64_t> ArrowRandomAccessFile::GetSize() {
    return file_size_;
}

arrow::Result<int64_t>
ArrowRandomAccessFile::ReadAt(int64_t position, int64_t nbytes, void *out) {
    if (nbytes == 0) {
        return 0;
    }
    ssize_t n = ::pread(fd_, out, static_cast<size_t>(nbytes), static_cast<off_t>(position));
    if (n < 0) {
        return arrow::Status::IOError("pread failed at offset ", position, ": ",
                                      std::strerror(errno));
    }
    return static_cast<int64_t>(n);
}

arrow::Result<std::shared_ptr<arrow::Buffer>>
ArrowRandomAccessFile::ReadAt(int64_t position, int64_t nbytes) {
    ARROW_ASSIGN_OR_RAISE(auto buffer,
                          arrow::AllocateResizableBuffer(nbytes, arrow::default_memory_pool()));
    ARROW_ASSIGN_OR_RAISE(int64_t bytes_read,
                          ReadAt(position, nbytes, buffer->mutable_data()));
    ARROW_RETURN_NOT_OK(buffer->Resize(bytes_read));
    return std::shared_ptr<arrow::Buffer>(std::move(buffer));
}

arrow::Result<int64_t> ArrowRandomAccessFile::Read(int64_t nbytes, void *out) {
    int64_t position = pos_.load(std::memory_order_relaxed);
    ARROW_ASSIGN_OR_RAISE(int64_t bytes_read, ReadAt(position, nbytes, out));
    pos_.fetch_add(bytes_read, std::memory_order_relaxed);
    return bytes_read;
}

arrow::Result<std::shared_ptr<arrow::Buffer>> ArrowRandomAccessFile::Read(int64_t nbytes) {
    int64_t position = pos_.load(std::memory_order_relaxed);
    ARROW_ASSIGN_OR_RAISE(auto buffer, ReadAt(position, nbytes));
    pos_.fetch_add(buffer->size(), std::memory_order_relaxed);
    return buffer;
}

} // namespace duckdb
