#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <exception>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace acr_load {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

inline std::int64_t thread_cpu_ns()
{
    timespec now{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) {
        throw std::runtime_error("CLOCK_THREAD_CPUTIME_ID failed");
    }
    return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

inline std::int64_t ns_since(Clock::time_point origin, Clock::time_point time)
{
    return std::chrono::duration_cast<Nanoseconds>(time - origin).count();
}

struct Scenario {
    std::string name;
    unsigned workers;
    unsigned requests;
    unsigned rate;
    unsigned capacity;
};

struct Decision {
    unsigned planned{};
    unsigned proposals{};
    unsigned candidate_checks{};
    unsigned refill_nodes{};
    unsigned state_expansions{};
    unsigned verified_natural{};
    unsigned natural_replays{};
    unsigned cache_hits{};
    bool used_witness{};
    bool fallback{};
    bool deadline_capped{};
    bool work_capped{};
    bool verified{};
};

struct Item {
    unsigned index{};
    std::int64_t due_ns{};
    std::int64_t enqueued_ns{};
};

struct Record {
    bool admitted{};
    unsigned worker{};
    std::int64_t due_ns{};
    std::int64_t enqueued_ns{};
    std::int64_t start_ns{};
    std::int64_t finish_ns{};
    std::int64_t cpu_ns{};
    Decision decision{};
};

struct Run {
    Scenario scenario;
    std::vector<Record> records;
    std::vector<unsigned> high_water;
    std::int64_t duration_ns{};
};

struct Queue {
    std::deque<Item> items;
    std::mutex mutex;
    std::condition_variable_any ready;
    bool closed{};
    unsigned high_water{};
};

template <class Factory>
Run run(const Scenario& scenario, Factory factory)
{
    if (scenario.workers == 0 || scenario.requests == 0 || scenario.rate == 0 || scenario.capacity == 0) {
        throw std::invalid_argument("all scenario sizes must be positive");
    }
    Run result{scenario, std::vector<Record>(scenario.requests), std::vector<unsigned>(scenario.workers), 0};
    std::vector<std::unique_ptr<Queue>> queues;
    for (unsigned worker = 0; worker < scenario.workers; ++worker) {
        queues.push_back(std::make_unique<Queue>());
    }
    std::latch initialized(scenario.workers);
    std::vector<std::exception_ptr> errors(scenario.workers);
    Clock::time_point origin{};
    std::vector<std::jthread> threads;
    for (unsigned worker = 0; worker < scenario.workers; ++worker) {
        threads.emplace_back([&, worker](std::stop_token stop) {
            bool announced = false;
            try {
                auto operation = factory(worker);
                auto& queue = *queues[worker];
                initialized.count_down();
                announced = true;
                for (;;) {
                    Item item;
                    {
                        std::unique_lock lock(queue.mutex);
                        queue.ready.wait(lock, stop, [&] { return queue.closed || !queue.items.empty(); });
                        if (queue.items.empty()) {
                            return;
                        }
                        item = queue.items.front();
                        queue.items.pop_front();
                    }
                    Record record;
                    record.admitted = true;
                    record.worker = worker;
                    record.due_ns = item.due_ns;
                    record.enqueued_ns = item.enqueued_ns;
                    record.start_ns = ns_since(origin, Clock::now());
                    const auto cpu_start = thread_cpu_ns();
                    record.decision = operation(item.index);
                    record.cpu_ns = thread_cpu_ns() - cpu_start;
                    record.finish_ns = ns_since(origin, Clock::now());
                    result.records[item.index] = record;
                }
            } catch (...) {
                errors[worker] = std::current_exception();
                if (!announced) {
                    initialized.count_down();
                }
            }
        });
    }

    initialized.wait();
    origin = Clock::now();
    // Schedule in fixed 100 µs buckets so release pacing is independent of
    // per-request service completion. Retain individual due times to expose
    // the pacing jitter separately from queue residence.
    constexpr std::int64_t tick_ns = 100'000;
    std::int64_t previous_tick = -1;
    for (unsigned index = 0; index < scenario.requests; ++index) {
        const auto due = 1'000'000'000LL * index / scenario.rate;
        const auto tick = (due + tick_ns - 1) / tick_ns;
        if (tick != previous_tick) {
            std::this_thread::sleep_until(origin + Nanoseconds(tick * tick_ns));
            previous_tick = tick;
        }
        const auto worker = index % scenario.workers;
        auto& queue = *queues[worker];
        {
            std::lock_guard lock(queue.mutex);
            const auto stamp = ns_since(origin, Clock::now());
            if (queue.items.size() >= scenario.capacity) {
                result.records[index] = Record{false, worker, due, stamp};
                continue;
            }
            queue.items.push_back(Item{index, due, stamp});
            queue.high_water = std::max(queue.high_water, static_cast<unsigned>(queue.items.size()));
        }
        queue.ready.notify_one();
    }

    for (auto& queue : queues) {
        {
            std::lock_guard lock(queue->mutex);
            queue->closed = true;
        }
        queue->ready.notify_all();
    }
    for (auto& thread : threads) {
        thread.join();
    }
    result.duration_ns = ns_since(origin, Clock::now());
    for (unsigned worker = 0; worker < scenario.workers; ++worker) {
        if (errors[worker]) {
            std::rethrow_exception(errors[worker]);
        }
        result.high_water[worker] = queues[worker]->high_water;
    }
    return result;
}

inline double percentile(const std::vector<std::int64_t>& values, double fraction)
{
    if (values.empty()) {
        return 0.0;
    }
    auto ordered = values;
    std::sort(ordered.begin(), ordered.end());
    const auto position = static_cast<std::size_t>(std::ceil(fraction * ordered.size()));
    return ordered.at(position - 1) / 1000.0;
}

} // namespace acr_load
