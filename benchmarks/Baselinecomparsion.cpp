#include <benchmark/benchmark.h>
#include "OrderBook.hpp"
#include "AsyncLogger.hpp"
#include "Stlbaselinebook.hpp"
#include <vector>
#include <algorithm>
#include <numeric>
#include <cstdio>

static inline uint64_t rdtsc_start() {
    uint32_t lo, hi;
    __asm__ volatile("CPUID\nRDTSC" : "=a"(lo), "=d"(hi) :: "%rbx", "%rcx");
    return (uint64_t(hi) << 32) | lo;
}
static inline uint64_t rdtsc_end() {
    uint32_t lo, hi;
    __asm__ volatile("RDTSCP\nmov %%edx,%1\nmov %%eax,%0\nCPUID"
                     : "=r"(lo), "=r"(hi) :: "%rax", "%rbx", "%rcx", "%rdx");
    return (uint64_t(hi) << 32) | lo;
}
static double calibrate_ghz() {
    timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t c1 = rdtsc_start();
    do { clock_gettime(CLOCK_MONOTONIC, &b); }
    while ((b.tv_sec - a.tv_sec) * 1'000'000'000LL + (b.tv_nsec - a.tv_nsec) < 50'000'000LL);
    uint64_t c2 = rdtsc_end();
    double el = (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
    return (c2 - c1) / el;
}

static void latency_report(const char* label, std::vector<uint64_t>& s, double ghz) {
    if (s.empty()) return;
    std::sort(s.begin(), s.end());
    auto ns  = [&](size_t i) { return s[i] / ghz; };
    auto pct = [&](double p) {
        size_t i = (size_t)(p * s.size());
        if (i >= s.size()) i = s.size() - 1;
        return ns(i);
    };
    uint64_t sum = std::accumulate(s.begin(), s.end(), uint64_t(0));
    printf("\n=== %s ===\n", label);
    printf("  samples %zu | cpu %.2f GHz\n", s.size(), ghz);
    printf("  min %.0f  p50 %.0f  p90 %.0f  p99 %.0f  p99.9 %.0f  max %.0f  mean %.0f  (ns)\n",
           ns(0), pct(.50), pct(.90), pct(.99), pct(.999),
           ns(s.size() - 1), (sum / ghz) / s.size());
}

// Identical synthetic workload shape for both implementations so the
// comparison is apples-to-apples: 40% passive bid / 40% passive ask /
// 10% aggressive buy / 10% aggressive sell, sliding window of 256 resting
// orders per side.
template <typename InsertFn, typename CancelFn>
static void run_latency_benchmark(const char* label, InsertFn insert, CancelFn cancel) {
    const double ghz = calibrate_ghz();
    const int N = 200000;
    const uint32_t MID = 10000;

    for (int i = 1; i <= 50; ++i) {
        insert(uint64_t(1000000 + i), Nanomatch::Side::BUY,  MID - (uint32_t)i, 100u);
        insert(uint64_t(2000000 + i), Nanomatch::Side::SELL, MID + (uint32_t)i, 100u);
    }

    std::vector<uint64_t> samples;
    samples.reserve(N);

    const int W = 256;
    std::vector<uint64_t> bidwin(W, 0), askwin(W, 0);
    int bh = 0, ah = 0;
    uint64_t id = 3000000;

    for (int i = 0; i < N; ++i) {
        uint64_t t0 = rdtsc_start();
        int r = i % 10;

        if (r < 4) {
            if (bidwin[bh % W]) cancel(bidwin[bh % W]);
            insert(id, Nanomatch::Side::BUY, MID - 1 - (uint32_t)(id % 20), 50u + (uint32_t)(id % 100));
            bidwin[bh % W] = id; ++bh; ++id;
        } else if (r < 8) {
            if (askwin[ah % W]) cancel(askwin[ah % W]);
            insert(id, Nanomatch::Side::SELL, MID + 1 + (uint32_t)(id % 20), 50u + (uint32_t)(id % 100));
            askwin[ah % W] = id; ++ah; ++id;
        } else if (r == 8) {
            uint64_t this_id = id++;
            insert(this_id, Nanomatch::Side::BUY, MID + 3, 30u + (uint32_t)(this_id % 50));
        } else {
            uint64_t this_id = id++;
            insert(this_id, Nanomatch::Side::SELL, MID - 3, 30u + (uint32_t)(this_id % 50));
        }

        samples.push_back(rdtsc_end() - t0);
    }

    latency_report(label, samples, ghz);
}

// ── google-benchmark throughput: optimized ──────────────────────────────
static void BM_Optimized_Insert(benchmark::State& state) {
    Nanomatch::AsyncLogger logger("/dev/null");
    logger.start();
    Nanomatch::OrderBook book(100000, logger);
    uint64_t id = 1;
    for (auto _ : state) {
        uint64_t this_id = id++;
        book.insert_limit_order(this_id, Nanomatch::Side::BUY, 100 + (this_id % 50), 10);
        benchmark::ClobberMemory();
    }
    logger.stop();
}
BENCHMARK(BM_Optimized_Insert);

// ── google-benchmark throughput: STL baseline ───────────────────────────
static void BM_StlBaseline_Insert(benchmark::State& state) {
    Nanomatch::Baseline::StlOrderBook book;
    uint64_t id = 1;
    for (auto _ : state) {
        uint64_t this_id = id++;
        book.insert_limit_order(this_id, Nanomatch::Side::BUY, 100 + (uint32_t)(this_id % 50), 10);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_StlBaseline_Insert);

int main(int argc, char** argv) {
    // Percentile latency comparison first (own rdtsc harness, no gbench overhead)
    {
        Nanomatch::AsyncLogger logger("/dev/null");
        logger.start();
        Nanomatch::OrderBook book(100000, logger);
        run_latency_benchmark(
            "OPTIMIZED OrderBook — add/cancel/match latency",
            [&](uint64_t id, Nanomatch::Side s, Nanomatch::Price p, Nanomatch::Quantity q) {
                book.insert_limit_order(id, s, p, q);
            },
            [&](uint64_t id) { book.cancel_order(id); });
        logger.stop();
    }
    {
        Nanomatch::Baseline::StlOrderBook book;
        run_latency_benchmark(
            "STL BASELINE (std::map/list/unordered_map) — add/cancel/match latency",
            [&](uint64_t id, Nanomatch::Side s, Nanomatch::Price p, Nanomatch::Quantity q) {
                book.insert_limit_order(id, s, p, q);
            },
            [&](uint64_t id) { book.cancel_order(id); });
    }

    // Then google-benchmark for raw throughput (orders/sec) numbers
    printf("\n");
    ::benchmark::Initialize(&argc, argv);
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}