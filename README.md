# Limit Order Book

[![CI](https://github.com/J-Leeeeee/order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/J-Leeeeee/order-book/actions/workflows/ci.yml)

A single-instrument, single-threaded matching engine in C++23. Capacities are fixed at construction, so `submit`, `cancel`, and `modify` do not allocate.

## Highlights

- Deterministic price-time priority matching at the resting order's price
- Limit and market orders with cancel and priority-aware modify operations
- Fixed-capacity pools and preallocated execution storage on the command path
- Transactional rejection when order, level, or aggregate quantity capacity is exhausted
- Invariant checks, deterministic replay, randomized stress tests, and a Release benchmark

## Requirements

- CMake 3.25 or newer
- A C++23 compiler (Apple Clang, Clang, GCC, or MSVC)

## Build and test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Example

```cpp
#include "order_book/order_book.hpp"

#include <iostream>

int main() {
    order_book::OrderBook book;
    const auto resting =
        book.submit(1, order_book::Side::Sell, order_book::OrderType::Limit, 101, 10);
    if (resting.status == order_book::CommandStatus::Rejected) {
        return 1;
    }

    const auto result =
        book.submit(2, order_book::Side::Buy, order_book::OrderType::Limit, 101, 4);
    for (const auto& execution : result.executions) {
        std::cout << execution.maker_id << " matched " << execution.taker_id << " for "
                  << execution.quantity << " at " << execution.price << '\n';
    }
}
```

The execution span is owned by the book and remains valid until its next command. See
[`SPEC.md`](SPEC.md) for the complete API and matching rules.

## Benchmark

Use a Release build. An unoptimized default configuration is not representative.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DORDER_BOOK_BUILD_BENCHMARKS=ON
cmake --build build-release
./build-release/benchmarks/order_book_benchmark
```

The harness runs five trials per workload. Each trial warms up 100,000 events, then measures 1,000,000 events. Throughput times a complete workload loop, including driver overhead. Latency percentiles use `std::chrono::steady_clock` around each event and therefore include timer overhead.

Measured 18 Aug 2026 on an Apple M5 Pro (18-core, 64 GB) with Apple Clang 21.0.0, `-O3`. Median of five trials, default capacities (65,536 orders, 4,096 levels, 10.5 MiB preallocated):

- Mixed submit/cancel/modify/cross: **19.3 million events/s**, mean **52 ns**/event from throughput, p99 **125 ns**
- Non-crossing insert: 30.5 million events/s
- Cancel (book held near 4,096 live orders): 31.0 million events/s
- Crossing/match: 26.3 million events/s
- Modify: 24.0 million events/s

Single-threaded, in-memory, one instrument. These are not colocation or production-exchange numbers.

## Capacities

Defaults are 65,536 active orders and 4,096 price levels. Both are constructor arguments. Exhausting either rejects the command and leaves the book unchanged.

Aggregate bid and ask quantities are also checked. A command that would overflow `Quantity` is rejected without changing the book.

## Layout

- `include/order_book/`: public types and `OrderBook` API
- `src/order_book.cpp`: matching engine
- `tests/`: CTest executable
- `SPEC.md`: API, matching rules, and rejections
- `DESIGN.md`: storage, indexing, and determinism

## License

MIT; see [`LICENSE`](LICENSE).
