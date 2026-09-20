#include "SelectivePixelsScan.hpp"

#include "PixelsScanFunction.hpp"
#include "reader/PixelsRecordReaderImpl.h"
#include "physical/DynamicBufferPool.h"
#include "physical/StorageFactory.h"
#include "utils/ConfigFactory.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace duckdb::selective_scan {

namespace {

std::string OptionalProperty(const std::string &key, const std::string &fallback)
{
    try
    {
        return ConfigFactory::Instance().getProperty(key);
    }
    catch (...)
    {
        return fallback;
    }
}

int IntegerProperty(const std::string &key, int fallback, int minimum, int maximum)
{
    const auto value = OptionalProperty(key, std::to_string(fallback));
    size_t parsed = 0;
    int result;
    try
    {
        result = std::stoi(value, &parsed);
    }
    catch (...)
    {
        throw InvalidArgumentException(key + " must be an integer");
    }
    if (parsed != value.size() || result < minimum || result > maximum)
    {
        throw InvalidArgumentException(key + " is outside the supported range");
    }
    return result;
}

double DoubleProperty(const std::string &key, double fallback, double minimum, double maximum)
{
    const auto value = OptionalProperty(key, std::to_string(fallback));
    size_t parsed = 0;
    double result;
    try
    {
        result = std::stod(value, &parsed);
    }
    catch (...)
    {
        throw InvalidArgumentException(key + " must be numeric");
    }
    if (parsed != value.size() || !std::isfinite(result) || result < minimum || result > maximum)
    {
        throw InvalidArgumentException(key + " is outside the supported range");
    }
    return result;
}

void PrintSummary(PixelsReadGlobalState &state)
{
    bool expected = false;
    if (!state.selective ||
        !state.selectiveSummaryPrinted.compare_exchange_strong(expected, true))
    {
        return;
    }

    const auto &s = state.selective->stats();
    std::cerr << "[SelectiveBuffer] claimed=" << s.claimed
              << " transferred=" << s.transferred
              << " dequeued=" << s.dequeued
              << " local_fit=" << s.localFit
              << " local_grow=" << s.localGrow
              << " queue_high_water=" << s.queueHighWater
              << " queued_demand_bytes_high_water=" << s.queuedDemandBytesHighWater
              << " queue_wait_ns=" << s.queueWaitNs
              << " max_queue_wait_ns=" << s.maxQueueWaitNs
              << " gate_rejected=" << s.gateRejected
              << " queue_rejected=" << s.queueRejected
              << " no_receiver=" << s.noReceiver
              << " cost_evaluations=" << s.costEvaluations
              << " adaptive_limit_changes=" << s.adaptiveLimitChanges
              << " executed=" << s.executed
              << " observed_buffer_bytes=" << s.observedBufferBytes << '\n';
    std::cerr << "[SelectiveBufferGrowth] allocations=" << s.allocations
              << " growths=" << s.growths
              << " growth_bytes=" << s.growthBytes << '\n';

    if (!state.selective->metricsEnabled())
    {
        return;
    }
    const char *prefix = std::getenv("PIXELS_SELECTIVE_METRICS_PREFIX");
    if (prefix == nullptr || prefix[0] == '\0')
    {
        std::cerr << "[SelectiveBufferMetrics] error=PIXELS_SELECTIVE_METRICS_PREFIX_not_set\n";
        return;
    }
    try
    {
        state.selective->writeMetrics(prefix);
        std::cerr << "[SelectiveBufferMetrics] prefix=" << prefix << '\n';
    }
    catch (const std::exception &error)
    {
        std::cerr << "[SelectiveBufferMetrics] error=" << error.what() << '\n';
    }
}

bool FinishWorker(ClientContext &context, PixelsReadBindData &bind_data,
                  PixelsReadLocalState &local, PixelsReadGlobalState &global,
                  bool is_init_state)
{
    local.cfgSelective = false;
    local.next_file_index = global.storageArrayScheduler->getFileSum(local.deviceID);
    const bool result = PixelsScanFunction::PixelsParallelStateNext(
        context, bind_data, local, global, is_init_state);
    local.cfgSelective = true;
    if (!result && local.shouldPrintProfileSummary)
    {
        PrintSummary(global);
    }
    return result;
}

} // namespace

void InitializeGlobal(PixelsReadGlobalState &state)
{
    auto &config = ConfigFactory::Instance();
    if (!config.getBoolProperty("pixels.dynamic.selective.enabled", false))
    {
        return;
    }

    if (!config.getBoolProperty("pixels.enable.dynamic.buffer", false) ||
        !config.getBoolProperty("pixels.doublebuffer", false) ||
        !config.getBoolProperty("localfs.enable.async.io", false) ||
        OptionalProperty("localfs.async.lib", "") != "iouring" ||
        config.getBoolProperty("pixel.enable.globalStaticBytebuffer", false))
    {
        throw InvalidArgumentException(
            "selective requires dynamic + doublebuffer + io_uring and excludes static mode");
    }

    pixels::SelectiveBufferScheduler::Options options;
    options.queueLimit = IntegerProperty(
        "pixels.dynamic.selective.queue.limit", 1, 0, 64);
    options.growthGateEnabled = config.getBoolProperty(
        "pixels.dynamic.selective.growth_gate.enabled", false);
    options.growthGateMinBytes = static_cast<uint64_t>(IntegerProperty(
        "pixels.dynamic.selective.growth_gate.min_bytes", 1, 0, 1 << 30));
    options.growthGateRequireHistory = config.getBoolProperty(
        "pixels.dynamic.selective.growth_gate.require_history", true);
    options.costModelEnabled = config.getBoolProperty(
        "pixels.dynamic.selective.cost.enabled", false);
    options.costQueueWeight = DoubleProperty(
        "pixels.dynamic.selective.cost.queue_weight", 1.0, 0.0, 1000.0);
    options.costQueuedBytesWeight = DoubleProperty(
        "pixels.dynamic.selective.cost.queued_bytes_weight", 1.0, 0.0, 1000.0);
    options.costLoadWeight = DoubleProperty(
        "pixels.dynamic.selective.cost.load_weight", 0.25, 0.0, 1000.0);
    options.costSlackWeight = DoubleProperty(
        "pixels.dynamic.selective.cost.slack_weight", 0.01, 0.0, 1000.0);
    options.adaptiveQueueEnabled = config.getBoolProperty(
        "pixels.dynamic.selective.adaptive.enabled", false);
    options.adaptiveQueueMin = IntegerProperty(
        "pixels.dynamic.selective.adaptive.queue_min", 0, 0, 64);
    options.adaptiveQueueMax = IntegerProperty(
        "pixels.dynamic.selective.adaptive.queue_max", 4, 0, 64);
    options.adaptivePressureLow = DoubleProperty(
        "pixels.dynamic.selective.adaptive.pressure_low", 0.10, 0.0, 1.0);
    options.adaptivePressureHigh = DoubleProperty(
        "pixels.dynamic.selective.adaptive.pressure_high", 0.30, 0.0, 1.0);
    options.adaptiveWindow = IntegerProperty(
        "pixels.dynamic.selective.adaptive.window", 64, 1, 1000000);
    options.metricsEnabled = config.getBoolProperty(
        "pixels.dynamic.selective.metrics.enabled", false);

    std::vector<uint64_t> counts;
    for (int group = 0; group < state.storageArrayScheduler->getDeviceSum(); ++group)
    {
        counts.push_back(state.storageArrayScheduler->getFileSum(group));
    }
    state.selective = std::make_unique<pixels::SelectiveBufferScheduler>(
        std::move(counts), options);
}

void InitializeLocal(PixelsReadGlobalState &global, PixelsReadLocalState &local)
{
    if (!global.selective)
    {
        return;
    }
    local.cfgSelective = true;
    unique_lock<mutex> guard(global.lock);
    local.selectiveWorker = global.selective->addWorker(
        local.deviceID, std::this_thread::get_id());
    global.selective->publish(
        local.selectiveWorker, ::DynamicBufferPool::GetCapacities(), true);
}

bool StateNext(ClientContext &context, PixelsReadBindData &bind_data,
               PixelsReadLocalState &local, PixelsReadGlobalState &global,
               bool is_init_state)
{
    auto &storage = global.storageArrayScheduler;
    unique_lock<mutex> lock(global.lock);
    if (global.error_opening_file)
    {
        throw InvalidArgumentException("PixelsScanInitLocal: file open error.");
    }
    global.selective->checkOwner(local.selectiveWorker);

    if (!is_init_state && local.nextReader == nullptr)
    {
        global.selective->finish(local.selectiveWorker);
        lock.unlock();
        return FinishWorker(context, bind_data, local, global, false);
    }

    bind_data.curFileId++;
    local.curr_file_index = local.next_file_index;
    local.curr_batch_index = local.next_batch_index;
    local.curr_file_name = local.next_file_name;

    bool claimed = global.selective->claim(local.selectiveWorker, local.selectiveTask);
    local.next_file_index = claimed
        ? local.selectiveTask.file
        : storage->getFileSum(local.deviceID);
    local.next_batch_index = storage->getBatchID(local.deviceID, local.next_file_index);
    lock.unlock();

    if (local.currReader != nullptr)
    {
        local.currReader->close();
    }

    // Selective is validated as Dynamic + double-buffer, so only the original
    // Aug 25 Dynamic switch is used here.
    ::DynamicBufferPool::Switch();

    local.currReader = local.nextReader;
    local.currPixelsRecordReader = local.nextPixelsRecordReader;
    if (local.currPixelsRecordReader != nullptr)
    {
        auto current = std::static_pointer_cast<PixelsRecordReaderImpl>(
            local.currPixelsRecordReader);
        current->asyncReadComplete(static_cast<int>(local.column_names.size()));
    }

    unsigned transfers = 0;
    while (claimed)
    {
        auto builder = std::make_shared<PixelsReaderBuilder>();
        std::shared_ptr<::Storage> file_storage =
            StorageFactory::getInstance()->getStorage(::Storage::file);
        local.next_file_name = storage->getFileName(
            local.deviceID, local.next_file_index);
        // Aug 25's direct-I/O readFully returns a non-owning ByteBuffer view.
        // A transferred task can reopen this file after its first reader
        // closes, so a shared footer cache would retain a dangling pointer.
        // Keep metadata reader-local in the Selective path.
        local.nextReader = builder->setPath(local.next_file_name)
                                  ->setStorage(file_storage)
                                  ->build();

        PixelsReaderOption option = PixelsScanFunction::GetPixelsReaderOption(local, global);
        local.nextPixelsRecordReader = local.nextReader->read(option);
        auto next = std::static_pointer_cast<PixelsRecordReaderImpl>(
            local.nextPixelsRecordReader);

        const auto demand = next->prepareBufferDemand();
        const auto capacities = ::DynamicBufferPool::GetCapacities();
        bool transferred;
        {
            unique_lock<mutex> route_lock(global.lock);
            global.selective->publish(local.selectiveWorker, capacities);
            transferred = global.selective->route(
                local.selectiveWorker, local.selectiveTask, demand,
                capacities[::DynamicBufferPool::GetAllocationBufferIdx()],
                transfers < 2);
            if (!transferred)
            {
                global.selective->recordExecution(
                    local.selectiveWorker, local.selectiveTask, demand);
            }
        }

        if (transferred)
        {
            local.nextPixelsRecordReader.reset();
            local.nextReader->close();
            local.nextReader.reset();
            ++transfers;

            unique_lock<mutex> claim_lock(global.lock);
            claimed = global.selective->claim(
                local.selectiveWorker, local.selectiveTask);
            local.next_file_index = claimed
                ? local.selectiveTask.file
                : storage->getFileSum(local.deviceID);
            local.next_batch_index = storage->getBatchID(
                local.deviceID, local.next_file_index);
            continue;
        }

        next->read();
        {
            unique_lock<mutex> publish_lock(global.lock);
            global.selective->publish(
                local.selectiveWorker, ::DynamicBufferPool::GetCapacities());
        }
        break;
    }

    if (!claimed)
    {
        local.nextReader = nullptr;
        local.nextPixelsRecordReader = nullptr;
    }
    return true;
}

} // namespace duckdb::selective_scan
