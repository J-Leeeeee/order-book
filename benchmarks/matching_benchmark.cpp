#include "order_book/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#endif

using order_book::CommandResult;
using order_book::CommandStatus;
using order_book::OrderBook;
using order_book::OrderId;
using order_book::OrderType;
using order_book::Price;
using order_book::Quantity;
using order_book::RejectReason;
using order_book::Side;

namespace {

constexpr std::size_t kWarmup = 100000;
constexpr std::size_t kMeasured = 1000000;
constexpr int kTrials = 5;
constexpr std::uint64_t kSeed = 1;
constexpr std::size_t kLiveWaterline = 4096;

enum class Workload { Mixed, Insert, Cancel, Match, Modify };

std::string_view workload_name(Workload workload) {
    switch (workload) {
        case Workload::Mixed:
            return "mixed";
        case Workload::Insert:
            return "insert";
        case Workload::Cancel:
            return "cancel";
        case Workload::Match:
            return "match";
        case Workload::Modify:
            return "modify";
    }
    return "unknown";
}

struct Driver {
    OrderBook book;
    std::mt19937_64 rng;
    std::vector<OrderId> live;
    std::vector<OrderId> recycled;
    OrderId next_id{1};

    explicit Driver(std::uint64_t seed) : rng(seed) {
        live.reserve(16384);
        recycled.reserve(16384);
    }

    OrderId alloc_id() {
        if (!recycled.empty()) {
            const OrderId id = recycled.back();
            recycled.pop_back();
            return id;
        }
        return next_id++;
    }

    void recycle(OrderId id) { recycled.push_back(id); }

    void remove_live_at(std::size_t index) {
        recycle(live[index]);
        live[index] = live.back();
        live.pop_back();
    }

    void note_submit(OrderId id, OrderType type, const CommandResult& result) {
        if (result.status == CommandStatus::Accepted && type == OrderType::Limit &&
            result.unfilled_quantity > 0) {
            live.push_back(id);
        } else {
            recycle(id);
        }
    }

    void rest(Side side, Price price, Quantity quantity) {
        const OrderId id = alloc_id();
        const CommandResult result = book.submit(id, side, OrderType::Limit, price, quantity);
        note_submit(id, OrderType::Limit, result);
    }

    void submit_noncrossing() {
        if (live.size() > kLiveWaterline * 2) {
            cancel_one();
            return;
        }
        const OrderId id = alloc_id();
        const Side side = (rng() & 1U) == 0 ? Side::Buy : Side::Sell;
        const Price price = (side == Side::Buy) ? static_cast<Price>(1 + static_cast<int>(rng() % 80))
                                                : static_cast<Price>(120 + static_cast<int>(rng() % 80));
        const Quantity quantity = 1 + static_cast<Quantity>(rng() % 8);
        const CommandResult result = book.submit(id, side, OrderType::Limit, price, quantity);
        note_submit(id, OrderType::Limit, result);
    }

    void cancel_one() {
        if (live.empty()) {
            submit_noncrossing();
            return;
        }
        const std::size_t index = static_cast<std::size_t>(rng() % live.size());
        (void)book.cancel(live[index]);
        remove_live_at(index);
    }

    void seed_two_sided() {
        if (book.ask_level_count() == 0) {
            rest(Side::Sell, static_cast<Price>(100 + static_cast<int>(rng() % 10)),
                 8 + static_cast<Quantity>(rng() % 8));
        }
        if (book.bid_level_count() == 0) {
            rest(Side::Buy, static_cast<Price>(80 + static_cast<int>(rng() % 10)),
                 8 + static_cast<Quantity>(rng() % 8));
        }
    }

    void submit_crossing() {
        seed_two_sided();
        const OrderId id = alloc_id();
        const bool buy = (rng() & 1U) == 0;
        const Side side = buy ? Side::Buy : Side::Sell;
        const OrderType type = (rng() % 5 == 0) ? OrderType::Market : OrderType::Limit;
        const Price price = buy ? static_cast<Price>(110 + static_cast<int>(rng() % 20))
                                : static_cast<Price>(70 + static_cast<int>(rng() % 15));
        const Quantity quantity = 1 + static_cast<Quantity>(rng() % 6);
        const CommandResult result = book.submit(id, side, type, price, quantity);
        note_submit(id, type, result);
    }

    void modify_one() {
        if (live.size() < 64) {
            submit_noncrossing();
            return;
        }
        const std::size_t index = static_cast<std::size_t>(rng() % live.size());
        const OrderId id = live[index];
        const auto snapshot = book.active_order(id);
        if (!snapshot.has_value()) {
            remove_live_at(index);
            return;
        }
        const bool reduce = (rng() & 1U) == 0 && snapshot->remaining > 1;
        const Price price = snapshot->price;
        const Quantity quantity =
            reduce ? (1 + static_cast<Quantity>(rng() % snapshot->remaining))
                   : (snapshot->remaining + 1 + static_cast<Quantity>(rng() % 4));
        const CommandResult result = book.modify(id, price, quantity);
        if (result.status == CommandStatus::Rejected && result.reason == RejectReason::UnknownId) {
            remove_live_at(index);
        } else if (result.status == CommandStatus::Accepted && result.unfilled_quantity == 0) {
            remove_live_at(index);
        }
    }

    void mixed() {
        const int pick = static_cast<int>(rng() % 100);
        const bool crowded = live.size() > kLiveWaterline;
        if (live.empty() || (!crowded && pick < 52)) {
            const OrderId id = alloc_id();
            const Side side = (rng() & 1U) == 0 ? Side::Buy : Side::Sell;
            const bool aggressive = (rng() % 5) == 0;
            const OrderType type = (rng() % 14 == 0) ? OrderType::Market : OrderType::Limit;
            Price price = static_cast<Price>(1 + static_cast<int>(rng() % 200));
            if (aggressive && type == OrderType::Limit) {
                price = (side == Side::Buy) ? static_cast<Price>(160 + static_cast<int>(rng() % 41))
                                            : static_cast<Price>(1 + static_cast<int>(rng() % 40));
            }
            const Quantity quantity = 1 + static_cast<Quantity>(rng() % 10);
            const CommandResult result = book.submit(id, side, type, price, quantity);
            note_submit(id, type, result);
            return;
        }
        if (pick < 74 || crowded) {
            cancel_one();
            return;
        }
        modify_one();
    }

    void run(Workload workload) {
        switch (workload) {
            case Workload::Mixed:
                mixed();
                return;
            case Workload::Insert:
                submit_noncrossing();
                return;
            case Workload::Cancel:
                if (live.size() < kLiveWaterline) {
                    submit_noncrossing();
                } else {
                    cancel_one();
                }
                return;
            case Workload::Match:
                submit_crossing();
                return;
            case Workload::Modify:
                modify_one();
                return;
        }
    }
};

struct Sample {
    double throughput_eps{};
    std::uint64_t mean_ns{};
    std::uint64_t median_ns{};
    std::uint64_t p95_ns{};
    std::uint64_t p99_ns{};
    std::uint64_t max_ns{};
};

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, int percent) {
    const std::size_t index = (sorted.size() * static_cast<std::size_t>(percent)) / 100;
    return sorted[std::min(index, sorted.size() - 1)];
}

Sample median_sample(std::vector<Sample> trials) {
    Sample out{};
    const auto pick = [&](auto member) {
        std::vector<double> values;
        values.reserve(trials.size());
        for (const Sample& sample : trials) {
            values.push_back(static_cast<double>(sample.*member));
        }
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    out.throughput_eps = pick(&Sample::throughput_eps);
    out.mean_ns = static_cast<std::uint64_t>(pick(&Sample::mean_ns));
    out.median_ns = static_cast<std::uint64_t>(pick(&Sample::median_ns));
    out.p95_ns = static_cast<std::uint64_t>(pick(&Sample::p95_ns));
    out.p99_ns = static_cast<std::uint64_t>(pick(&Sample::p99_ns));
    out.max_ns = static_cast<std::uint64_t>(pick(&Sample::max_ns));
    return out;
}

Sample run_trial(Workload workload, std::uint64_t seed) {
    Driver throughput_driver(seed + 17);
    for (std::size_t i = 0; i < kWarmup; ++i) {
        throughput_driver.run(workload);
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kMeasured; ++i) {
        throughput_driver.run(workload);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    Sample sample{};
    sample.throughput_eps =
        static_cast<double>(kMeasured) / (static_cast<double>(elapsed_ns) / 1'000'000'000.0);

    Driver latency_driver(seed + 31);
    for (std::size_t i = 0; i < kWarmup; ++i) {
        latency_driver.run(workload);
    }
    std::vector<std::uint64_t> samples(kMeasured);
    for (std::size_t i = 0; i < kMeasured; ++i) {
        const auto start = std::chrono::steady_clock::now();
        latency_driver.run(workload);
        const auto stop = std::chrono::steady_clock::now();
        samples[i] = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
    }
    std::uint64_t sum = 0;
    std::uint64_t maximum = 0;
    for (std::uint64_t value : samples) {
        sum += value;
        if (value > maximum) {
            maximum = value;
        }
    }
    std::sort(samples.begin(), samples.end());
    sample.mean_ns = sum / kMeasured;
    sample.median_ns = samples[kMeasured / 2];
    sample.p95_ns = percentile(samples, 95);
    sample.p99_ns = percentile(samples, 99);
    sample.max_ns = maximum;
    return sample;
}

}  // namespace

int main() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    OrderBook probe;
    std::cout << "storage_bytes " << probe.storage_bytes() << '\n';
    std::cout << "max_orders " << probe.max_orders() << '\n';
    std::cout << "max_levels " << probe.max_levels() << '\n';
    std::cout << "warmup_events " << kWarmup << '\n';
    std::cout << "measured_events " << kMeasured << '\n';
    std::cout << "trials " << kTrials << '\n';

    const Workload workloads[] = {Workload::Mixed, Workload::Insert, Workload::Cancel, Workload::Match,
                                  Workload::Modify};

    std::cout << std::fixed << std::setprecision(2);
    for (Workload workload : workloads) {
        std::vector<Sample> trials;
        trials.reserve(kTrials);
        for (int trial = 0; trial < kTrials; ++trial) {
            const Sample sample =
                run_trial(workload, kSeed + static_cast<std::uint64_t>(trial) * 1009U);
            trials.push_back(sample);
            std::cout << "trial " << workload_name(workload) << ' ' << (trial + 1) << " throughput_eps "
                      << sample.throughput_eps << " mean_ns " << sample.mean_ns << " median_ns "
                      << sample.median_ns << " p95_ns " << sample.p95_ns << " p99_ns " << sample.p99_ns
                      << " max_ns " << sample.max_ns << '\n';
        }
        const Sample summary = median_sample(trials);
        std::cout << "median " << workload_name(workload) << " throughput_eps " << summary.throughput_eps
                  << " mean_ns " << summary.mean_ns << " median_ns " << summary.median_ns << " p95_ns "
                  << summary.p95_ns << " p99_ns " << summary.p99_ns << " max_ns " << summary.max_ns
                  << '\n';
    }
    return 0;
}
