#include <benchmark/benchmark.h>
#include <cmath>
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

// Custom Helper: Extracts any arbitrary percentile from the raw sample vector
auto GetPercentile = [](const std::vector<double>& v, double percentile) -> double {
    auto copy = v;
    std::sort(copy.begin(), copy.end());
    size_t idx = static_cast<size_t>(std::round(copy.size() * percentile));
    if (idx >= copy.size()) idx = copy.size() - 1;
    return copy[idx];
};

// Register your benchmark with the full suite of rigorous stats
BENCHMARK(BM_OrderInsertion)
    ->ComputeStatistics("p50_median", [](const std::vector<double>& v) { return GetPercentile(v, 0.50); })
    ->ComputeStatistics("p90",        [](const std::vector<double>& v) { return GetPercentile(v, 0.90); })
    ->ComputeStatistics("p99",        [](const std::vector<double>& v) { return GetPercentile(v, 0.99); })
    ->ComputeStatistics("p99.9",      [](const std::vector<double>& v) { return GetPercentile(v, 0.999); })
    ->ComputeStatistics("max",        [](const std::vector<double>& v) { return *std::max_element(v.begin(), v.end()); })
    ->DisplayAggregatesOnly(true); // Set to true if you only want to see the stats summary

BENCHMARK_MAIN();