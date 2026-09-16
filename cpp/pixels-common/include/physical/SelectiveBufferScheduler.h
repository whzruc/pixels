#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pixels {

// Query-scoped scheduling metadata only. The caller must hold the scan-global
// lock for every mutating operation. No buffer, reader, or io_uring crosses
// threads. Optional event tracing is accumulated in memory and written once
// after the query, so metrics never perform file I/O on the scan hot path.
class SelectiveBufferScheduler {
public:
    using Demand = std::map<uint32_t, uint64_t>;
    using Capacity = std::array<Demand, 2>;

    struct Options {
        size_t queueLimit = 1;
        bool growthGateEnabled = false;
        uint64_t growthGateMinBytes = 1;
        bool growthGateRequireHistory = true;
        bool costModelEnabled = false;
        double costQueueWeight = 1.0;
        double costQueuedBytesWeight = 1.0;
        double costLoadWeight = 0.25;
        double costSlackWeight = 0.01;
        bool adaptiveQueueEnabled = false;
        size_t adaptiveQueueMin = 0;
        size_t adaptiveQueueMax = 4;
        double adaptivePressureLow = 0.10;
        double adaptivePressureHigh = 0.30;
        size_t adaptiveWindow = 64;
        bool metricsEnabled = false;
    };

    struct Task {
        uint64_t file = 0;
        bool transferred = false;
        uint64_t demandBytes = 0;
        uint64_t enqueuedNs = 0;
        size_t sourceWorker = invalidWorker();
        static constexpr size_t invalidWorker() { return std::numeric_limits<size_t>::max(); }
    };

    struct Stats {
        uint64_t claimed = 0, transferred = 0, dequeued = 0, executed = 0;
        uint64_t localFit = 0, localGrow = 0, queueHighWater = 0;
        uint64_t observedBufferBytes = 0;
        uint64_t allocations = 0, growths = 0, growthBytes = 0;
        uint64_t gateRejected = 0, queueRejected = 0, noReceiver = 0;
        uint64_t costEvaluations = 0, adaptiveLimitChanges = 0;
        uint64_t queuedDemandBytesHighWater = 0;
        uint64_t queueWaitNs = 0, maxQueueWaitNs = 0;
    };

    explicit SelectiveBufferScheduler(std::vector<uint64_t> counts, size_t limit)
        : SelectiveBufferScheduler(std::move(counts), optionsForLimit(limit)) {}

    explicit SelectiveBufferScheduler(std::vector<uint64_t> counts, Options options)
        : counts_(std::move(counts)), cursors_(counts_.size(), 0), options_(std::move(options)),
          effectiveQueueLimit_(options_.adaptiveQueueEnabled ? options_.adaptiveQueueMin
                                                             : options_.queueLimit),
          started_(Clock::now()) {
        validateOptions();
    }

    size_t addWorker(size_t group, std::thread::id owner) {
        if (group >= counts_.size()) throw std::out_of_range("selective: storage group");
        for (size_t i = 0; i < workers_.size(); ++i) {
            auto &w = workers_[i];
            if (w.owner != owner) continue;
            if (!w.finished) throw std::runtime_error(
                "selective: overlapping scan local states share one TLS buffer pool");
            w.group = group;
            w.accepting = true;
            w.finished = false;
            trace("worker_reuse", i);
            return i;
        }
        Worker worker;
        worker.group = group;
        worker.owner = owner;
        worker.accepting = true;
        workers_.push_back(std::move(worker));
        trace("worker_add", workers_.size() - 1);
        return workers_.size() - 1;
    }

    void checkOwner(size_t worker) const {
        if (workers_.at(worker).owner != std::this_thread::get_id())
            throw std::runtime_error("selective: scan worker migrated across OS threads");
    }

    void finish(size_t worker) {
        auto &w = workers_.at(worker);
        if (w.accepting || !w.queue.empty())
            throw std::logic_error("selective: finishing a worker before queue retirement");
        w.finished = true;
        trace("worker_finish", worker);
    }

    void publish(size_t worker, Capacity capacity, bool baseline = false) {
        auto &w = workers_.at(worker);
        const auto &previous = w.capacity;
        const uint64_t oldBytes = capacityBytes(previous);
        const uint64_t newBytes = capacityBytes(capacity);
        if (!baseline) {
            for (size_t slot = 0; slot < 2; ++slot) {
                for (const auto &col : capacity[slot]) {
                    auto old = previous[slot].find(col.first);
                    if (old == previous[slot].end()) {
                        ++stats_.allocations;
                    } else if (col.second > old->second) {
                        ++stats_.growths;
                        stats_.growthBytes += col.second - old->second;
                        grownColumns_.insert(col.first);
                    }
                }
            }
        }
        w.capacity = std::move(capacity);
        w.bufferBytes = newBytes;
        w.peakBufferBytes = std::max(w.peakBufferBytes, newBytes);
        currentBytes_ = currentBytes_ - oldBytes + newBytes;
        stats_.observedBufferBytes = std::max(stats_.observedBufferBytes, currentBytes_);
        if (oldBytes != newBytes || baseline) trace("buffer_publish", worker);
    }

    bool claim(size_t worker, Task &task) {
        auto &w = workers_.at(worker);
        if (!w.accepting) return false;
        if (!w.queue.empty()) {
            task = w.queue.front();
            w.queue.pop_front();
            w.queuedDemandBytes -= task.demandBytes;
            currentQueuedDemandBytes_ -= task.demandBytes;
            const uint64_t wait = task.enqueuedNs == 0 ? 0 : nowNs() - task.enqueuedNs;
            ++stats_.dequeued;
            stats_.queueWaitNs += wait;
            stats_.maxQueueWaitNs = std::max(stats_.maxQueueWaitNs, wait);
            ++w.dequeued;
            w.queueWaitNs += wait;
            w.maxQueueWaitNs = std::max(w.maxQueueWaitNs, wait);
            trace("dequeue", worker, task.sourceWorker, task.file, task.demandBytes, wait);
            return true;
        }
        if (cursors_[w.group] < counts_[w.group]) {
            task = Task{};
            task.file = cursors_[w.group]++;
            task.sourceWorker = worker;
            ++stats_.claimed;
            ++w.rawClaimed;
            trace("claim", worker, Task::invalidWorker(), task.file);
            return true;
        }
        w.accepting = false;
        trace("worker_retire", worker);
        return false;
    }

    static bool fits(const Demand &capacity, const Demand &demand) {
        for (const auto &col : demand) {
            auto it = capacity.find(col.first);
            if (it == capacity.end() || it->second < col.second) return false;
        }
        return true;
    }

    // Both parity slots of the receiver must fit a queued file. The source only
    // checks the slot about to be used. At most one transfer is allowed.
    bool route(size_t source, const Task &task, const Demand &demand,
               const Demand &localCapacity, bool allowTransfer = true) {
        const GrowthPressure pressure = analyzeGrowth(localCapacity, demand);
        observePressure(pressure.existingGrowth);
        if (!pressure.needsCapacity) {
            ++stats_.localFit;
            trace("local_fit", source, Task::invalidWorker(), task.file, pressure.demandBytes);
            return false;
        }
        if (!allowTransfer || task.transferred || effectiveQueueLimit_ == 0) {
            ++stats_.localGrow;
            if (effectiveQueueLimit_ == 0) ++stats_.queueRejected;
            trace("local_grow", source, Task::invalidWorker(), task.file, pressure.demandBytes);
            return false;
        }
        if (options_.growthGateEnabled && !passesGrowthGate(localCapacity, demand, pressure)) {
            ++stats_.gateRejected;
            ++stats_.localGrow;
            trace("gate_reject", source, Task::invalidWorker(), task.file, pressure.demandBytes);
            return false;
        }

        size_t best = Task::invalidWorker();
        double bestCost = std::numeric_limits<double>::infinity();
        bool anyFitting = false, queueBlocked = false;
        for (size_t i = 0; i < workers_.size(); ++i) {
            const auto &w = workers_[i];
            if (i == source || !w.accepting || w.group != workers_.at(source).group ||
                !fits(w.capacity[0], demand) || !fits(w.capacity[1], demand)) continue;
            anyFitting = true;
            if (w.queue.size() >= effectiveQueueLimit_) {
                queueBlocked = true;
                continue;
            }
            double cost = static_cast<double>(w.queue.size());
            if (options_.costModelEnabled) {
                ++stats_.costEvaluations;
                cost = receiverCost(i, demand, pressure.demandBytes);
            }
            if (best == Task::invalidWorker() || cost < bestCost ||
                (cost == bestCost && i < best)) {
                best = i;
                bestCost = cost;
            }
        }
        if (best == Task::invalidWorker()) {
            ++stats_.localGrow;
            if (anyFitting && queueBlocked) ++stats_.queueRejected;
            else ++stats_.noReceiver;
            trace(anyFitting ? "queue_reject" : "no_receiver", source,
                  Task::invalidWorker(), task.file, pressure.demandBytes);
            return false;
        }

        auto &target = workers_[best];
        Task queued = task;
        queued.transferred = true;
        queued.demandBytes = pressure.demandBytes;
        queued.enqueuedNs = nowNs();
        queued.sourceWorker = source;
        target.queue.push_back(queued);
        target.queuedDemandBytes += queued.demandBytes;
        target.peakQueueDepth = std::max<uint64_t>(target.peakQueueDepth, target.queue.size());
        target.peakQueuedDemandBytes = std::max(target.peakQueuedDemandBytes, target.queuedDemandBytes);
        currentQueuedDemandBytes_ += queued.demandBytes;
        ++workers_[source].transferredOut;
        ++target.transferredIn;
        ++stats_.transferred;
        stats_.queueHighWater = std::max<uint64_t>(stats_.queueHighWater, target.queue.size());
        stats_.queuedDemandBytesHighWater = std::max(stats_.queuedDemandBytesHighWater,
                                                     currentQueuedDemandBytes_);
        // The event's worker is the receiver so its queue depth/bytes describe
        // the queue that just changed; peer is the sender.
        trace("enqueue", best, source, task.file, pressure.demandBytes);
        return true;
    }

    // Measures assignment/load immediately before I/O submission, not SQL
    // decode completion time.
    void recordExecution(size_t worker, const Task &task, const Demand &demand) {
        auto &w = workers_.at(worker);
        const uint64_t bytes = demandBytes(demand);
        ++w.executed;
        w.executedDemandBytes += bytes;
        ++stats_.executed;
        trace("execute", worker, task.sourceWorker, task.file, bytes);
    }

    const Stats &stats() const { return stats_; }
    const Options &options() const { return options_; }
    size_t effectiveQueueLimit() const { return effectiveQueueLimit_; }
    bool metricsEnabled() const { return options_.metricsEnabled; }

    void writeMetrics(const std::string &prefix) const {
        if (!options_.metricsEnabled || prefix.empty()) return;
        std::ofstream events(prefix + ".events.csv");
        if (!events) throw std::runtime_error("selective: cannot write event metrics: " + prefix);
        events << "sequence,time_ns,event,worker,peer,file,group,demand_bytes,worker_buffer_bytes,"
                  "total_buffer_bytes,queue_depth,worker_queued_demand_bytes,total_queue_depth,"
                  "total_queued_demand_bytes,effective_queue_limit,queue_wait_ns,pressure_ewma\n";
        for (const auto &e : events_) {
            events << e.sequence << ',' << e.timeNs << ',' << e.type << ',' << printable(e.worker)
                   << ',' << printable(e.peer) << ',' << e.file << ',' << e.group << ','
                   << e.demandBytes << ',' << e.workerBufferBytes << ',' << e.totalBufferBytes
                   << ',' << e.queueDepth << ',' << e.workerQueuedDemandBytes << ','
                   << e.totalQueueDepth << ',' << e.totalQueuedDemandBytes << ','
                   << e.effectiveQueueLimit << ',' << e.queueWaitNs << ',' << e.pressureEwma << '\n';
        }
        std::ofstream workers(prefix + ".workers.csv");
        if (!workers) throw std::runtime_error("selective: cannot write worker metrics: " + prefix);
        workers << "worker,group,raw_claimed,transferred_in,transferred_out,dequeued,executed,"
                   "executed_demand_bytes,final_buffer_bytes,peak_buffer_bytes,peak_queue_depth,"
                   "peak_queued_demand_bytes,queue_wait_ns,max_queue_wait_ns\n";
        for (size_t i = 0; i < workers_.size(); ++i) {
            const auto &w = workers_[i];
            workers << i << ',' << w.group << ',' << w.rawClaimed << ',' << w.transferredIn
                    << ',' << w.transferredOut << ',' << w.dequeued << ',' << w.executed << ','
                    << w.executedDemandBytes << ',' << w.bufferBytes << ',' << w.peakBufferBytes
                    << ',' << w.peakQueueDepth << ',' << w.peakQueuedDemandBytes << ','
                    << w.queueWaitNs << ',' << w.maxQueueWaitNs << '\n';
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    struct Worker {
        size_t group = 0;
        std::thread::id owner;
        bool accepting = false, finished = false;
        Capacity capacity;
        std::deque<Task> queue;
        uint64_t bufferBytes = 0, peakBufferBytes = 0, queuedDemandBytes = 0;
        uint64_t rawClaimed = 0, transferredIn = 0, transferredOut = 0;
        uint64_t dequeued = 0, executed = 0, executedDemandBytes = 0;
        uint64_t peakQueueDepth = 0, peakQueuedDemandBytes = 0;
        uint64_t queueWaitNs = 0, maxQueueWaitNs = 0;
    };
    struct Event {
        uint64_t sequence = 0, timeNs = 0;
        std::string type;
        size_t worker = Task::invalidWorker(), peer = Task::invalidWorker();
        uint64_t file = 0, group = 0, demandBytes = 0, workerBufferBytes = 0;
        uint64_t totalBufferBytes = 0, queueDepth = 0, workerQueuedDemandBytes = 0;
        uint64_t totalQueueDepth = 0, totalQueuedDemandBytes = 0, effectiveQueueLimit = 0;
        uint64_t queueWaitNs = 0;
        double pressureEwma = 0;
    };
    struct GrowthPressure {
        bool needsCapacity = false, existingGrowth = false;
        uint64_t demandBytes = 0, avoidableGrowthBytes = 0;
    };

    static Options optionsForLimit(size_t limit) {
        Options options;
        options.queueLimit = limit;
        options.adaptiveQueueMax = std::max<size_t>(limit, 1);
        return options;
    }
    void validateOptions() const {
        if (options_.queueLimit > 64 || options_.adaptiveQueueMin > 64 ||
            options_.adaptiveQueueMax > 64 || options_.adaptiveQueueMin > options_.adaptiveQueueMax)
            throw std::invalid_argument("selective: queue limits must satisfy 0 <= min <= max <= 64");
        if (!(options_.adaptivePressureLow >= 0.0 &&
              options_.adaptivePressureLow <= options_.adaptivePressureHigh &&
              options_.adaptivePressureHigh <= 1.0))
            throw std::invalid_argument("selective: adaptive thresholds must satisfy 0 <= low <= high <= 1");
        if (options_.adaptiveWindow == 0)
            throw std::invalid_argument("selective: adaptive window must be positive");
        if (options_.costQueueWeight < 0 || options_.costQueuedBytesWeight < 0 ||
            options_.costLoadWeight < 0 || options_.costSlackWeight < 0)
            throw std::invalid_argument("selective: cost weights must be nonnegative");
    }
    static uint64_t demandBytes(const Demand &demand) {
        uint64_t total = 0;
        for (const auto &col : demand) total += col.second;
        return total;
    }
    static uint64_t capacityBytes(const Capacity &capacity) {
        uint64_t total = 0;
        for (const auto &slot : capacity)
            for (const auto &col : slot) total += col.second;
        return total;
    }
    GrowthPressure analyzeGrowth(const Demand &capacity, const Demand &demand) const {
        GrowthPressure result;
        result.demandBytes = demandBytes(demand);
        for (const auto &col : demand) {
            auto it = capacity.find(col.first);
            if (it == capacity.end()) result.needsCapacity = true;
            else if (it->second < col.second) {
                result.needsCapacity = true;
                result.existingGrowth = true;
                result.avoidableGrowthBytes += col.second - it->second;
            }
        }
        return result;
    }
    bool passesGrowthGate(const Demand &capacity, const Demand &demand,
                          const GrowthPressure &pressure) const {
        if (!pressure.existingGrowth ||
            pressure.avoidableGrowthBytes < options_.growthGateMinBytes) return false;
        if (!options_.growthGateRequireHistory) return true;
        for (const auto &col : demand) {
            auto it = capacity.find(col.first);
            if (it != capacity.end() && it->second < col.second &&
                grownColumns_.count(col.first) != 0) return true;
        }
        return false;
    }
    double receiverCost(size_t worker, const Demand &demand, uint64_t bytes) const {
        const auto &w = workers_[worker];
        const double unit = static_cast<double>(std::max<uint64_t>(bytes, 1));
        const double queueCost = static_cast<double>(w.queue.size());
        const double queuedBytesCost = static_cast<double>(w.queuedDemandBytes) / unit;
        uint64_t groupLoad = 0;
        size_t groupWorkers = 0;
        for (const auto &candidate : workers_) {
            if (candidate.group == w.group && !candidate.finished) {
                groupLoad += candidate.executedDemandBytes;
                ++groupWorkers;
            }
        }
        const double averageLoad = groupWorkers == 0 ? 0.0
            : static_cast<double>(groupLoad) / static_cast<double>(groupWorkers);
        const double loadCost = averageLoad == 0.0 ? 0.0
            : static_cast<double>(w.executedDemandBytes) / averageLoad;
        uint64_t slack = 0;
        for (const auto &slot : w.capacity) {
            for (const auto &col : demand) {
                auto it = slot.find(col.first);
                if (it != slot.end() && it->second > col.second) slack += it->second - col.second;
            }
        }
        const double slackCost = static_cast<double>(slack) / unit;
        return options_.costQueueWeight * queueCost +
               options_.costQueuedBytesWeight * queuedBytesCost +
               options_.costLoadWeight * loadCost +
               options_.costSlackWeight * slackCost;
    }
    void observePressure(bool growth) {
        if (!options_.adaptiveQueueEnabled) return;
        const double alpha = 2.0 / (static_cast<double>(options_.adaptiveWindow) + 1.0);
        pressureEwma_ = alpha * (growth ? 1.0 : 0.0) + (1.0 - alpha) * pressureEwma_;
        size_t next = options_.adaptiveQueueMin;
        if (pressureEwma_ >= options_.adaptivePressureHigh) next = options_.adaptiveQueueMax;
        else if (pressureEwma_ >= options_.adaptivePressureLow)
            next = std::min(options_.adaptiveQueueMax, options_.adaptiveQueueMin + 1);
        if (next != effectiveQueueLimit_) {
            effectiveQueueLimit_ = next;
            ++stats_.adaptiveLimitChanges;
            trace("queue_limit", Task::invalidWorker());
        }
    }
    uint64_t nowNs() const {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started_).count());
    }
    uint64_t totalQueueDepth() const {
        uint64_t result = 0;
        for (const auto &w : workers_) result += w.queue.size();
        return result;
    }
    static long long printable(size_t worker) {
        return worker == Task::invalidWorker() ? -1LL : static_cast<long long>(worker);
    }
    void trace(const char *type, size_t worker, size_t peer = Task::invalidWorker(),
               uint64_t file = 0, uint64_t demand = 0, uint64_t wait = 0) {
        if (!options_.metricsEnabled) return;
        Event e;
        e.sequence = events_.size();
        e.timeNs = nowNs();
        e.type = type;
        e.worker = worker;
        e.peer = peer;
        e.file = file;
        e.demandBytes = demand;
        e.totalBufferBytes = currentBytes_;
        e.totalQueueDepth = totalQueueDepth();
        e.totalQueuedDemandBytes = currentQueuedDemandBytes_;
        e.effectiveQueueLimit = effectiveQueueLimit_;
        e.queueWaitNs = wait;
        e.pressureEwma = pressureEwma_;
        if (worker != Task::invalidWorker() && worker < workers_.size()) {
            const auto &w = workers_[worker];
            e.group = w.group;
            e.workerBufferBytes = w.bufferBytes;
            e.queueDepth = w.queue.size();
            e.workerQueuedDemandBytes = w.queuedDemandBytes;
        }
        events_.push_back(std::move(e));
    }

    std::vector<uint64_t> counts_, cursors_;
    Options options_;
    size_t effectiveQueueLimit_ = 0;
    uint64_t currentBytes_ = 0, currentQueuedDemandBytes_ = 0;
    double pressureEwma_ = 0.0;
    Clock::time_point started_;
    std::set<uint32_t> grownColumns_;
    std::vector<Worker> workers_;
    std::vector<Event> events_;
    Stats stats_;
};

} // namespace pixels
