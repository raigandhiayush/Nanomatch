# Nanomatch

![C++](https://img.shields.io/badge/C%2B%2B-20-blue)
![Build](https://img.shields.io/badge/build-CMake-success)
![License](https://img.shields.io/badge/license-MIT-green)

> A high-performance, cache-aware limit order book and matching engine
> implemented in modern C++20, designed to explore low-latency trading
> system techniques.

------------------------------------------------------------------------

# Features

-   Cache-aware contiguous data structures
-   Custom `OrderPool` allocator
-   Intrusive FIFO order queues
-   Sliding-window price indexing
-   Memory-mapped market-data ingestion
-   Lock-free SPSC asynchronous logger
-   Google Benchmark performance suite
-   STL baseline comparison
-   Unit tests
-   Linux `perf` + FlameGraph profiling

------------------------------------------------------------------------

# Architecture

``` text
              Market Data
                   │
          Memory-Mapped Parser
                   │
          +------------------+
          | Matching Engine  |
          +------------------+
             │          │
             │          ├─────────────┐
             ▼                        ▼
       Order Pool              Price Levels
             │                        │
             └────────────┬───────────┘
                          ▼
                    Matching Logic
                          │
                          ▼
                   Async Trade Logger
```

------------------------------------------------------------------------

# Order Book Design

The engine implements a **price-time priority** (FIFO) matching
algorithm.

## Core Components

### OrderBook

Responsible for:

-   inserting limit orders
-   cancelling orders
-   matching incoming orders
-   maintaining best bid/ask

### Price Levels

Each price level stores:

-   head order
-   tail order
-   aggregate volume

FIFO is maintained using intrusive linked lists.

### OrderPool

Instead of allocating each order individually using `new`, all orders
are allocated from a preallocated contiguous memory pool.

Advantages:

-   O(1) allocation
-   O(1) deallocation
-   excellent cache locality
-   avoids allocator overhead
-   deterministic latency

### ID Lookup Table

Maps

    Order ID → Order

allowing cancellation in constant time.

### Sliding Price Window

Instead of storing the entire theoretical price range, the engine
maintains a moving price window around the active market.

Benefits:

-   dramatically lower memory usage
-   improved cache locality

### Memory-Mapped Parser

Market data is read using `mmap()`.

Advantages:

-   zero-copy reads
-   reduced syscall overhead
-   sequential cache-friendly access

### Async Logger

Trade logging is separated from the matching thread using an SPSC queue.

Benefits:

-   matching thread never blocks on disk I/O
-   deterministic latency

------------------------------------------------------------------------

# Algorithms

## Insert

1.  Locate price level.
2.  Match against opposite side if marketable.
3.  Otherwise append to FIFO queue.
4.  Update best bid/ask.

## Cancel

1.  Lookup order using ID map.
2.  Remove from intrusive list.
3.  Return slot to pool.

## Match

Orders are matched according to:

1.  Best price
2.  Earliest arrival (FIFO)

------------------------------------------------------------------------

# Complexity

  Operation         Complexity
  ----------- ----------------
  Insert        O(1) amortized
  Cancel                  O(1)
  Lookup                  O(1)
  Match                   O(k)

------------------------------------------------------------------------

# Cache-Aware Optimizations

-   Contiguous storage
-   Object pool allocator
-   Sliding price window
-   Reduced pointer chasing
-   Intrusive linked lists
-   Asynchronous logging

------------------------------------------------------------------------

# Benchmarking

Google Benchmark is used for microbenchmarks.

Example:

  Benchmark               Latency
  ------------------ ------------
  Optimized Insert      **≈4 ns**
  STL Baseline         **≈94 ns**
  Speedup                **≈23×**

> Replace these values with your latest measurements if they change.

------------------------------------------------------------------------

# Tail Latency

Include results such as:

  Metric      Value
  -------- --------
  p50        126 ns
  p90        152 ns
  p99        454 ns
  p99.9      552 ns
  Mean       130 ns

------------------------------------------------------------------------

# Profiling

Tools used:

-   Google Benchmark
-   Linux perf
-   FlameGraph

Example:

``` bash
perf stat ./build/benchmarks/engine_benchmark
perf record --call-graph fp ./build/benchmarks/engine_benchmark
perf script | ./FlameGraph/stackcollapse-perf.pl | ./FlameGraph/flamegraph.pl > flamegraph.svg
```

Include in `docs/`:

-   `flamegraph.svg`
-   `perf_stat.txt`
-   `cache_stats.txt`

------------------------------------------------------------------------

# Suggested Repository Layout

``` text
docs/
├── architecture.png
├── benchmark_results.png
├── flamegraph.svg
├── perf_stat.txt
├── cache_stats.txt
└── latency_distribution.png
```

------------------------------------------------------------------------

# Building

``` bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

------------------------------------------------------------------------

# Running

``` bash
./nanomatch ../data/market_data.bin
```

Tests:

``` bash
./engine_tests
```

Benchmarks:

``` bash
./benchmarks/engine_benchmark
./benchmarks/stlbaseline_benchmark
```

------------------------------------------------------------------------

# Future Work

-   Multi-symbol matching
-   NUMA-aware memory layout
-   Lock-free inbound feed handling
-   SIMD optimizations
-   Persistent order book snapshots
-   ITCH replay improvements
-   Multi-threaded ingestion

------------------------------------------------------------------------

# Results to Include

Add screenshots for:

-   Google Benchmark output
-   `perf stat`
-   Flame graph
-   Latency percentile output
-   Architecture diagram

------------------------------------------------------------------------

# References

-   NASDAQ TotalView-ITCH Specification
-   Google Benchmark
-   Brendan Gregg's FlameGraph
-   Linux `perf`

------------------------------------------------------------------------

# License

MIT License

------------------------------------------------------------------------

# Resume Summary

> Built a cache-aware C++20 limit order book featuring custom memory
> pools, intrusive FIFO queues, memory-mapped ingestion, asynchronous
> logging, and benchmarked \~23× faster insertion latency than an
> STL-based implementation using Google Benchmark and Linux perf
> profiling.
