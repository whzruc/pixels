#include "physical/SelectiveBufferScheduler.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

using Scheduler = pixels::SelectiveBufferScheduler;
using Demand = Scheduler::Demand;

static void policyTests()
{
    std::atomic<bool> stop{false};
    std::thread helper([&] { while (!stop.load()) std::this_thread::yield(); });
    Scheduler scheduler({20, 1}, 1);
    const auto source = scheduler.addWorker(0, std::this_thread::get_id());
    const auto target = scheduler.addWorker(0, helper.get_id());
    Demand small{{0, 16}, {1, 4}};
    Demand big{{0, 64}, {1, 8}};
    scheduler.publish(source, {small, small});
    scheduler.publish(target, {big, small});

    Scheduler::Task task;
    assert(scheduler.claim(source, task));
    assert(!scheduler.route(source, task, big, small));
    scheduler.publish(target, {big, big});
    assert(!scheduler.route(source, task, small, small));
    assert(scheduler.route(source, task, big, small));

    Scheduler::Task next;
    assert(scheduler.claim(source, next));
    assert(!scheduler.route(source, next, big, small));
    Scheduler::Task delivered;
    assert(scheduler.claim(target, delivered));
    assert(delivered.file == task.file && delivered.transferred);
    assert(!scheduler.route(target, delivered, {{0, 128}}, small));
    assert(!Scheduler::fits({{0, 1000}}, big));
    assert(Scheduler::fits({}, {}));
    assert(scheduler.stats().transferred == 1);
    assert(scheduler.stats().dequeued == 1);

    bool rejected = false;
    try
    {
        scheduler.addWorker(0, std::this_thread::get_id());
    }
    catch (const std::runtime_error &)
    {
        rejected = true;
    }
    assert(rejected);

    stop = true;
    helper.join();
}

static void concurrentExactlyOnce()
{
    constexpr unsigned count = 20000;
    constexpr unsigned workerCount = 8;
    Scheduler scheduler({count}, 2);
    std::mutex mutex;
    std::atomic<unsigned> ready{0};
    std::vector<unsigned> seen(count, 0);
    std::vector<std::thread> threads;

    for (unsigned index = 0; index < workerCount; ++index)
    {
        threads.emplace_back([&, index] {
            size_t worker;
            Demand capacity{{0, index % 2 ? 64U : 16U}};
            {
                std::lock_guard<std::mutex> guard(mutex);
                worker = scheduler.addWorker(0, std::this_thread::get_id());
                scheduler.publish(worker, {capacity, capacity});
            }
            ++ready;
            while (ready != workerCount) std::this_thread::yield();

            unsigned transfers = 0;
            while (true)
            {
                Scheduler::Task task;
                {
                    std::lock_guard<std::mutex> guard(mutex);
                    scheduler.checkOwner(worker);
                    if (!scheduler.claim(worker, task)) break;
                }
                std::this_thread::yield();
                {
                    std::lock_guard<std::mutex> guard(mutex);
                    Demand demand{{0, task.file % 3 ? 16U : 64U}};
                    if (scheduler.route(worker, task, demand, capacity, transfers < 2))
                    {
                        ++transfers;
                        continue;
                    }
                    ++seen.at(task.file);
                    transfers = 0;
                }
            }
        });
    }
    for (auto &thread : threads) thread.join();
    for (auto executions : seen) assert(executions == 1);
    assert(scheduler.stats().claimed == count);
    assert(scheduler.stats().transferred == scheduler.stats().dequeued);
    assert(scheduler.stats().queueHighWater <= 2);
}

static void v2PolicyTests()
{
    std::atomic<bool> stop{false};
    std::thread helper([&] { while (!stop.load()) std::this_thread::yield(); });
    Demand small{{0, 16}}, medium{{0, 32}}, big{{0, 64}};

    Scheduler::Options options;
    options.queueLimit = 2;
    options.growthGateEnabled = true;
    options.growthGateMinBytes = 8;
    options.growthGateRequireHistory = true;
    options.metricsEnabled = true;
    Scheduler gate({4}, options);
    auto source = gate.addWorker(0, std::this_thread::get_id());
    auto target = gate.addWorker(0, helper.get_id());
    gate.publish(source, {small, small}, true);
    gate.publish(target, {big, big}, true);
    Scheduler::Task task;
    assert(gate.claim(source, task));
    assert(!gate.route(source, task, big, small));
    assert(gate.stats().gateRejected == 1);
    gate.publish(source, {medium, small});
    assert(gate.route(source, task, big, small));

    Scheduler::Task delivered;
    assert(gate.claim(target, delivered));
    gate.recordExecution(target, delivered, big);
    assert(gate.stats().executed == 1);

    const std::string prefix = "/tmp/pixels-selective-unit-" +
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

    Scheduler::Options adaptive;
    adaptive.queueLimit = 4;
    adaptive.adaptiveQueueEnabled = true;
    adaptive.adaptiveQueueMin = 0;
    adaptive.adaptiveQueueMax = 4;
    adaptive.adaptivePressureLow = 0.25;
    adaptive.adaptivePressureHigh = 0.75;
    adaptive.adaptiveWindow = 1;
    Scheduler adaptiveScheduler({2}, adaptive);
    source = adaptiveScheduler.addWorker(0, std::this_thread::get_id());
    target = adaptiveScheduler.addWorker(0, helper.get_id());
    adaptiveScheduler.publish(source, {small, small}, true);
    adaptiveScheduler.publish(target, {big, big}, true);
    assert(adaptiveScheduler.claim(source, task));
    assert(adaptiveScheduler.route(source, task, big, small));
    assert(adaptiveScheduler.effectiveQueueLimit() == 4);
    assert(!adaptiveScheduler.route(source, task, small, small));
    assert(adaptiveScheduler.effectiveQueueLimit() == 0);

    stop = true;
    helper.join();
}

int main()
{
    policyTests();
    v2PolicyTests();
    for (int repeat = 0; repeat < 10; ++repeat)
    {
        concurrentExactlyOnce();
    }
    std::cout << "PASS: selective policies and 10 x 20,000 exactly-once tasks\n";
}
