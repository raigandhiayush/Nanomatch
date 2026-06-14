#include <benchmark/benchmark.h>
#include "OrderBook.hpp"
#include "AsyncLogger.hpp"

static void BM_OrderInsertion(benchmark::State& state) {
    // Instantiate background worker dependencies
    Nanomatch::AsyncLogger logger("/dev/null"); // Throw away disk I/O drag
    logger.start();

    // 1 Million pre-allocated nodes to completely avoid runtime allocations
    Nanomatch::OrderBook book(1000000, logger); 
    Nanomatch::OrderId id = 1;

    // Warm up cache lines before measuring
    for (int i = 0; i < 100; ++i) {
        book.insert_limit_order(id++, Nanomatch::Side::BUY, 100, 10);
    }

    // Benchmark critical loop
    for (auto _ : state) {
        // Measure exact execution duration of a limit order placement
        book.insert_limit_order(id++, Nanomatch::Side::BUY, 100, 10);
    }

    logger.stop();
}

// Register the benchmark and explicitly demand p50, p90, and p99 statistics
BENCHMARK(BM_OrderInsertion)
    ->ComputeStatistics("p50", [](const std::vector<double>& v) -> double {
        auto copy = v;
        std::sort(copy.begin(), copy.end());
        return copy[copy.size() * 0.50];
    })
    ->ComputeStatistics("p90", [](const std::vector<double>& v) -> double {
        auto copy = v;
        std::sort(copy.begin(), copy.end());
        return copy[copy.size() * 0.90];
    })
    ->ComputeStatistics("p99", [](const std::vector<double>& v) -> double {
        auto copy = v;
        std::sort(copy.begin(), copy.end());
        return copy[copy.size() * 0.99];
    })
    ->DisplayAggregatesOnly(false); // Keeps the main iterations visible

BENCHMARK_MAIN();