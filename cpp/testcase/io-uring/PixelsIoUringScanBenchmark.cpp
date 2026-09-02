/* Copyright 2026 PixelsDB. Licensed under AGPL-3.0. */

#include "physical/DynamicBufferPool.h"
#include "physical/GlobalStaticBufferPool.h"
#include "physical/natives/DirectIoLib.h"
#include "physical/natives/DirectUringRandomAccessFileDynamic.h"
#include "physical/natives/DirectUringRandomAccessFileNonFixed.h"
#include "physical/natives/DirectUringRandomAccessFileStatic.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {
struct Options {
  std::string mode;
  std::string schedule = "shared";
  int threads = 12;
  uint32_t blockSize = 1024 * 1024;
  uint32_t queueDepth = 32;
  uint32_t filesPerRoot = 0;
  std::vector<fs::path> roots;
};

uint64_t ParsePositive(const std::string &text, const std::string &option) {
  size_t parsed = 0;
  auto value = std::stoull(text, &parsed);
  if (parsed != text.size() || value == 0) {
    throw std::invalid_argument(option + " must be a positive integer");
  }
  return value;
}

void PrintUsage(const char *program) {
  std::cerr << "Usage: " << program
            << " --mode non-fixed|dynamic|static --threads N"
               " [--schedule shared|device-affine]"
               " [--block-size BYTES] [--queue-depth N]"
               " [--files-per-root N] PATH...\n";
}

Options ParseOptions(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    auto requireValue = [&](const std::string &name) {
      if (++i >= argc) {
        throw std::invalid_argument(name + " requires a value");
      }
      return std::string(argv[i]);
    };
    if (argument == "--mode") {
      options.mode = requireValue(argument);
    } else if (argument == "--schedule") {
      options.schedule = requireValue(argument);
    } else if (argument == "--threads") {
      options.threads =
          static_cast<int>(ParsePositive(requireValue(argument), argument));
    } else if (argument == "--block-size") {
      options.blockSize = static_cast<uint32_t>(
          ParsePositive(requireValue(argument), argument));
    } else if (argument == "--queue-depth") {
      options.queueDepth = static_cast<uint32_t>(
          ParsePositive(requireValue(argument), argument));
    } else if (argument == "--files-per-root") {
      options.filesPerRoot = static_cast<uint32_t>(
          ParsePositive(requireValue(argument), argument));
    } else if (argument == "--help" || argument == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else if (argument.starts_with("--")) {
      throw std::invalid_argument("unknown option: " + argument);
    } else {
      options.roots.emplace_back(argument);
    }
  }
  if (options.mode != "non-fixed" && options.mode != "dynamic" &&
      options.mode != "static") {
    throw std::invalid_argument("--mode must be non-fixed, dynamic, or static");
  }
  if (options.roots.empty()) {
    throw std::invalid_argument("at least one input path is required");
  }
  if (options.schedule != "shared" && options.schedule != "device-affine") {
    throw std::invalid_argument("--schedule must be shared or device-affine");
  }
  if (options.schedule == "device-affine" &&
      static_cast<size_t>(options.threads) != options.roots.size()) {
    throw std::invalid_argument("device-affine scheduling requires --threads "
                                "to equal the number of input roots");
  }
  if (options.blockSize % 4096 != 0) {
    throw std::invalid_argument(
        "--block-size must be a multiple of 4096 for direct I/O");
  }
  if (options.mode == "static" && options.queueDepth % 2 != 0) {
    throw std::invalid_argument("static mode requires an even --queue-depth");
  }
  return options;
}

struct Workload {
  std::vector<fs::path> interleaved;
  std::vector<std::vector<fs::path>> byRoot;
};

Workload FindPixelsFiles(const std::vector<fs::path> &roots,
                         uint32_t filesPerRoot) {
  Workload workload;
  workload.byRoot.reserve(roots.size());
  for (const auto &root : roots) {
    std::vector<fs::path> rootFiles;
    if (!fs::exists(root)) {
      throw std::invalid_argument("input path does not exist: " +
                                  root.string());
    }
    if (fs::is_regular_file(root)) {
      rootFiles.push_back(root);
      workload.byRoot.emplace_back(std::move(rootFiles));
      continue;
    }
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
      if (entry.is_regular_file() && entry.path().extension() == ".pxl") {
        rootFiles.push_back(entry.path());
      }
    }
    std::sort(rootFiles.begin(), rootFiles.end());
    if (filesPerRoot > 0 && rootFiles.size() > filesPerRoot) {
      rootFiles.resize(filesPerRoot);
    }
    workload.byRoot.emplace_back(std::move(rootFiles));
  }

  // Interleave roots so adjacent workers scan different devices. Sorting all
  // paths together would drain one mount before advancing to the next.
  size_t largestRoot = 0;
  for (const auto &rootFiles : workload.byRoot) {
    largestRoot = std::max(largestRoot, rootFiles.size());
  }
  for (size_t index = 0; index < largestRoot; ++index) {
    for (const auto &rootFiles : workload.byRoot) {
      if (index < rootFiles.size()) {
        workload.interleaved.push_back(rootFiles[index]);
      }
    }
  }
  if (workload.interleaved.empty()) {
    throw std::invalid_argument("no .pxl files found in the input paths");
  }
  return workload;
}

class ScanBackend {
public:
  virtual ~ScanBackend() = default;
  virtual void Scan(const fs::path &path, uint64_t length, uint32_t blockSize,
                    uint32_t queueDepth, std::atomic<uint64_t> &requests) = 0;
};

template <typename FileType>
void ScanBatches(FileType &file, uint64_t length, uint32_t blockSize,
                 uint32_t queueDepth,
                 const std::vector<std::shared_ptr<ByteBuffer>> &buffers,
                 const std::vector<int> &bufferIndexes,
                 std::atomic<uint64_t> &requests) {
  uint64_t offset = 0;
  while (offset < length) {
    uint32_t count = 0;
    for (; count < queueDepth && offset < length; ++count) {
      auto bytes =
          static_cast<uint32_t>(std::min<uint64_t>(blockSize, length - offset));
      file.readAsync(bytes, buffers[count], bufferIndexes[count],
                     static_cast<int64_t>(offset));
      offset += bytes;
    }
    file.readAsyncSubmit(count);
    file.readAsyncComplete(count);
    requests.fetch_add(count, std::memory_order_relaxed);
  }
}

class NonFixedBackend final : public ScanBackend {
public:
  NonFixedBackend(uint32_t blockSize, uint32_t queueDepth) {
    DirectUringRandomAccessFileNonFixed::Initialize(queueDepth);
    DirectIoLib allocator(4096);
    for (uint32_t i = 0; i < queueDepth; ++i) {
      buffers.emplace_back(allocator.allocateDirectBuffer(blockSize, false));
    }
  }
  ~NonFixedBackend() override { DirectUringRandomAccessFileNonFixed::Reset(); }

  void Scan(const fs::path &path, uint64_t length, uint32_t blockSize,
            uint32_t queueDepth, std::atomic<uint64_t> &requests) override {
    DirectUringRandomAccessFileNonFixed file(path.string());
    uint64_t offset = 0;
    while (offset < length) {
      uint32_t count = 0;
      for (; count < queueDepth && offset < length; ++count) {
        auto bytes = static_cast<uint32_t>(
            std::min<uint64_t>(blockSize, length - offset));
        file.readAsync(bytes, buffers[count], static_cast<int64_t>(offset));
        offset += bytes;
      }
      file.readAsyncSubmit(count);
      file.readAsyncComplete(count);
      requests.fetch_add(count, std::memory_order_relaxed);
    }
  }

private:
  std::vector<std::shared_ptr<ByteBuffer>> buffers;
};

class DynamicBackend final : public ScanBackend {
public:
  DynamicBackend(uint32_t blockSize, uint32_t queueDepth) {
    DirectUringRandomAccessFileDynamic::Initialize(queueDepth, queueDepth);
    for (uint32_t i = 0; i < queueDepth; ++i) {
      buffers.emplace_back(DynamicBufferPool::AllocateBuffer(i, blockSize));
      indexes.emplace_back(DynamicBufferPool::GetBufferSlotIndex(i));
    }
  }
  ~DynamicBackend() override { DirectUringRandomAccessFileDynamic::Reset(); }

  void Scan(const fs::path &path, uint64_t length, uint32_t blockSize,
            uint32_t queueDepth, std::atomic<uint64_t> &requests) override {
    DirectUringRandomAccessFileDynamic file(path.string());
    ScanBatches(file, length, blockSize, queueDepth, buffers, indexes,
                requests);
  }

private:
  std::vector<std::shared_ptr<ByteBuffer>> buffers;
  std::vector<int> indexes;
};

class StaticBackend final : public ScanBackend {
public:
  explicit StaticBackend(uint32_t queueDepth) {
    auto &pool = GlobalStaticBufferPool::Instance();
    threadId = pool.AcquireThreadId();
    for (uint32_t i = 0; i < queueDepth; ++i) {
      auto name = "scan" + std::to_string(i / 2);
      buffers.emplace_back(pool.GetBuffer(name, threadId, i % 2));
      indexes.emplace_back(pool.GetBufferIndex(name, i % 2));
    }
  }

  void Scan(const fs::path &path, uint64_t length, uint32_t blockSize,
            uint32_t queueDepth, std::atomic<uint64_t> &requests) override {
    auto &pool = GlobalStaticBufferPool::Instance();
    DirectUringRandomAccessFileStatic file(path.string(),
                                           pool.GetRing(threadId));
    ScanBatches(file, length, blockSize, queueDepth, buffers, indexes,
                requests);
  }

private:
  int threadId;
  std::vector<std::shared_ptr<ByteBuffer>> buffers;
  std::vector<int> indexes;
};

std::unique_ptr<ScanBackend> MakeBackend(const Options &options) {
  if (options.mode == "non-fixed") {
    return std::make_unique<NonFixedBackend>(options.blockSize,
                                             options.queueDepth);
  }
  if (options.mode == "dynamic") {
    return std::make_unique<DynamicBackend>(options.blockSize,
                                            options.queueDepth);
  }
  return std::make_unique<StaticBackend>(options.queueDepth);
}

fs::path PrepareStaticPool(const Options &options) {
  auto path = fs::temp_directory_path() /
              ("pixels-io-scan-" +
               std::to_string(static_cast<long long>(getpid())) + ".sizes");
  std::ofstream output(path);
  for (uint32_t i = 0; i < options.queueDepth / 2; ++i) {
    output << "scan" << i << ' ' << options.blockSize << '\n';
  }
  output.close();
  GlobalStaticBufferPool::Instance().Initialize(path.string(), 4096,
                                                options.threads);
  return path;
}
} // namespace

int main(int argc, char **argv) {
  try {
    auto options = ParseOptions(argc, argv);
    auto workload = FindPixelsFiles(options.roots, options.filesPerRoot);
    uint64_t totalBytes = 0;
    for (const auto &file : workload.interleaved) {
      totalBytes += fs::file_size(file);
    }

    fs::path staticSizes;
    if (options.mode == "static") {
      staticSizes = PrepareStaticPool(options);
    }

    std::atomic<size_t> nextFile{0};
    std::atomic<uint64_t> requests{0};
    std::atomic<bool> start{false};
    std::latch ready(options.threads);
    std::latch done(options.threads);
    std::mutex errorMutex;
    std::exception_ptr error;
    std::vector<std::thread> workers;
    workers.reserve(options.threads);
    for (int thread = 0; thread < options.threads; ++thread) {
      workers.emplace_back([&, thread] {
        bool signaledReady = false;
        bool signaledDone = false;
        std::unique_ptr<ScanBackend> backend;
        try {
          backend = MakeBackend(options);
          ready.count_down();
          signaledReady = true;
          while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          if (options.schedule == "device-affine") {
            for (const auto &file : workload.byRoot.at(thread)) {
              backend->Scan(file, fs::file_size(file), options.blockSize,
                            options.queueDepth, requests);
            }
          } else {
            while (true) {
              auto index = nextFile.fetch_add(1, std::memory_order_relaxed);
              if (index >= workload.interleaved.size()) {
                break;
              }
              const auto &file = workload.interleaved[index];
              backend->Scan(file, fs::file_size(file), options.blockSize,
                            options.queueDepth, requests);
            }
          }
          done.count_down();
          signaledDone = true;
        } catch (...) {
          std::lock_guard<std::mutex> guard(errorMutex);
          if (!error) {
            error = std::current_exception();
          }
          if (!signaledReady) {
            ready.count_down();
          }
        }
        if (!signaledDone) {
          done.count_down();
        }
      });
    }

    ready.wait();
    auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    done.wait();
    auto end = std::chrono::steady_clock::now();
    for (auto &worker : workers) {
      worker.join();
    }
    if (options.mode == "static") {
      GlobalStaticBufferPool::Instance().Reset();
      fs::remove(staticSizes);
    }
    if (error) {
      std::rethrow_exception(error);
    }

    auto seconds = std::chrono::duration<double>(end - begin).count();
    auto gib = static_cast<double>(totalBytes) / (1024.0 * 1024.0 * 1024.0);
    std::cout << "mode=" << options.mode << '\n'
              << "schedule=" << options.schedule << '\n'
              << "threads=" << options.threads << '\n'
              << "files=" << workload.interleaved.size() << '\n'
              << "bytes=" << totalBytes << '\n'
              << "requests=" << requests.load() << '\n'
              << "seconds=" << seconds << '\n'
              << "gib_per_second=" << gib / seconds << '\n';
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "error: " << exception.what() << '\n';
    PrintUsage(argv[0]);
    return 1;
  }
}
