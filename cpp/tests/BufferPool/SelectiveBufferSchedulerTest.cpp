#include "physical/SelectiveBufferScheduler.h"
#include <cassert>
#include <iostream>
#include <mutex>
#include <set>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using Scheduler = pixels::SelectiveBufferScheduler;
using Demand = Scheduler::Demand;

static void policyTests() {
    // Real distinct thread identities, kept alive until registration completes.
    std::atomic<bool> stop{false};
    std::thread helper([&] { while (!stop.load()) std::this_thread::yield(); });
    Scheduler s({20, 1}, 1);
    const auto a = s.addWorker(0, std::this_thread::get_id());
    const auto b = s.addWorker(0, helper.get_id());
    Demand small{{0, 16}, {1, 4}}, big{{0, 64}, {1, 8}};
    s.publish(a, {small, small});
    s.publish(b, {big, small});
    Scheduler::Task task;
    assert(s.claim(a, task));
    assert(!s.route(a, task, big, small)); // cannot add capacities across slots
    s.publish(b, {big, big});
    assert(!s.route(a, task, small, small)); // local first
    assert(s.route(a, task, big, small));
    Scheduler::Task next;
    assert(s.claim(a, next));
    assert(!s.route(a, next, big, small)); // queue full -> local growth
    Scheduler::Task delivered;
    assert(s.claim(b, delivered));
    assert(delivered.file == task.file && delivered.transferred);
    assert(!s.route(b, delivered, {{0,128}}, small)); // one hop only
    assert(!s.route(a, next, big, small, false)); // transfer budget exhausted
    assert(!Scheduler::fits({{0,1000}}, big)); // missing column
    assert(Scheduler::fits({}, {}));
    assert(s.stats().transferred == 1 && s.stats().dequeued == 1);
    assert(s.stats().queueHighWater == 1 && s.stats().growths == 2);
    bool rejected = false;
    try { s.addWorker(0, std::this_thread::get_id()); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { s.checkOwner(b); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);

    Scheduler retired({1, 1}, 4);
    auto r0 = retired.addWorker(0, std::this_thread::get_id());
    auto r1 = retired.addWorker(0, helper.get_id());
    retired.publish(r1, {big, big});
    assert(retired.claim(r0, task));
    assert(!retired.claim(r1, next)); // retired under same lock as route
    assert(!retired.route(r0, task, big, small));
    retired.finish(r1);
    assert(retired.addWorker(0, helper.get_id()) == r1);
    assert(!retired.claim(r1, next));

    Scheduler group({1, 1}, 4);
    r0 = group.addWorker(0, std::this_thread::get_id());
    r1 = group.addWorker(1, helper.get_id());
    group.publish(r1, {big, big});
    assert(group.claim(r0, task));
    assert(!group.route(r0, task, big, small)); // no cross-group move

    Scheduler disabled({1}, 0);
    r0 = disabled.addWorker(0, std::this_thread::get_id());
    r1 = disabled.addWorker(0, helper.get_id());
    disabled.publish(r1, {big, big});
    assert(disabled.claim(r0, task));
    assert(!disabled.route(r0, task, big, small));
    stop = true;
    helper.join();
}

static void concurrentExactlyOnce() {
    constexpr unsigned count = 20000, nworkers = 8;
    Scheduler s({count}, 2);
    std::mutex mutex;
    std::atomic<unsigned> ready{0};
    std::vector<unsigned> seen(count, 0);
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < nworkers; ++i) threads.emplace_back([&, i] {
        size_t worker;
        Demand capacity{{0, i % 2 ? 64U : 16U}};
        {
            std::lock_guard<std::mutex> guard(mutex);
            worker = s.addWorker(0, std::this_thread::get_id());
            s.publish(worker, {capacity, capacity});
        }
        ++ready;
        while (ready != nworkers) std::this_thread::yield();
        unsigned transfers = 0;
        while (true) {
            Scheduler::Task task;
            {
                std::lock_guard<std::mutex> guard(mutex);
                s.checkOwner(worker);
                if (!s.claim(worker, task)) break;
            }
            std::this_thread::yield(); // metadata preparation outside lock
            {
                std::lock_guard<std::mutex> guard(mutex);
                Demand demand{{0, task.file % 3 ? 16U : 64U}};
                if (s.route(worker, task, demand, capacity, transfers < 2)) {
                    ++transfers; continue;
                }
                ++seen.at(task.file);
                transfers = 0;
            }
        }
    });
    for (auto &thread : threads) thread.join();
    for (auto executions : seen) assert(executions == 1);
    assert(s.stats().claimed == count);
    assert(s.stats().transferred == s.stats().dequeued);
    assert(s.stats().queueHighWater <= 2);
}

static void v2PolicyTests() {
    std::atomic<bool> stop{false};
    std::thread helper1([&] { while (!stop.load()) std::this_thread::yield(); });
    std::thread helper2([&] { while (!stop.load()) std::this_thread::yield(); });
    Demand small{{0, 16}}, medium{{0, 32}}, big{{0, 64}};

    Scheduler::Options gateOptions;
    gateOptions.queueLimit = 2;
    gateOptions.growthGateEnabled = true;
    gateOptions.growthGateMinBytes = 8;
    gateOptions.growthGateRequireHistory = true;
    gateOptions.metricsEnabled = true;
    Scheduler gate({4}, gateOptions);
    auto source = gate.addWorker(0, std::this_thread::get_id());
    auto target = gate.addWorker(0, helper1.get_id());
    gate.publish(source, {small, small}, true);
    gate.publish(target, {big, big}, true);
    Scheduler::Task task;
    assert(gate.claim(source, task));
    assert(!gate.route(source, task, big, small)); // no observed growth history yet
    assert(gate.stats().gateRejected == 1);
    gate.publish(source, {medium, small}); // establishes real growth pressure for col 0
    assert(gate.route(source, task, big, small));
    Scheduler::Task delivered;
    assert(gate.claim(target, delivered));
    gate.recordExecution(target, delivered, big);
    assert(gate.stats().executed == 1 && gate.stats().queueWaitNs > 0);

    const std::string prefix = "/tmp/pixels-selective-metrics-unit-" +
                               std::to_string(static_cast<long long>(getpid()));
    gate.writeMetrics(prefix);
    std::ifstream events(prefix + ".events.csv");
    std::ifstream workers(prefix + ".workers.csv");
    std::string header;
    std::getline(events, header);
    assert(header.find("worker_buffer_bytes") != std::string::npos);
    std::getline(workers, header);
    assert(header.find("executed_demand_bytes") != std::string::npos);
    events.close();
    workers.close();
    std::remove((prefix + ".events.csv").c_str());
    std::remove((prefix + ".workers.csv").c_str());

    Scheduler::Options costOptions;
    costOptions.queueLimit = 2;
    costOptions.costModelEnabled = true;
    costOptions.costLoadWeight = 10.0;
    Scheduler cost({8}, costOptions);
    source = cost.addWorker(0, std::this_thread::get_id());
    const auto lessLoaded = cost.addWorker(0, helper1.get_id());
    const auto moreLoaded = cost.addWorker(0, helper2.get_id());
    cost.publish(source, {small, small}, true);
    cost.publish(lessLoaded, {big, big}, true);
    cost.publish(moreLoaded, {big, big}, true);
    Scheduler::Task synthetic;
    for (int i = 0; i < 10; ++i) cost.recordExecution(moreLoaded, synthetic, big);
    assert(cost.claim(source, task));
    assert(cost.route(source, task, big, small));
    assert(cost.claim(lessLoaded, delivered)); // cost model avoids loaded receiver
    assert(delivered.file == task.file);
    assert(cost.stats().costEvaluations >= 2);

    Scheduler::Options adaptiveOptions;
    adaptiveOptions.queueLimit = 4;
    adaptiveOptions.adaptiveQueueEnabled = true;
    adaptiveOptions.adaptiveQueueMin = 0;
    adaptiveOptions.adaptiveQueueMax = 4;
    adaptiveOptions.adaptivePressureLow = 0.25;
    adaptiveOptions.adaptivePressureHigh = 0.75;
    adaptiveOptions.adaptiveWindow = 1;
    Scheduler adaptive({2}, adaptiveOptions);
    source = adaptive.addWorker(0, std::this_thread::get_id());
    target = adaptive.addWorker(0, helper1.get_id());
    adaptive.publish(source, {small, small}, true);
    adaptive.publish(target, {big, big}, true);
    assert(adaptive.claim(source, task));
    assert(adaptive.route(source, task, big, small));
    assert(adaptive.effectiveQueueLimit() == 4);
    assert(!adaptive.route(source, task, small, small));
    assert(adaptive.effectiveQueueLimit() == 0);
    assert(adaptive.stats().adaptiveLimitChanges == 2);

    stop = true;
    helper1.join();
    helper2.join();
}

int main() {
    policyTests();
    v2PolicyTests();
    for (int repeat = 0; repeat < 10; ++repeat) concurrentExactlyOnce();
    std::cout << "PASS: V1/V2 policy boundaries, metrics; 10 x 20,000 files exactly once across 8 workers\n";
}
