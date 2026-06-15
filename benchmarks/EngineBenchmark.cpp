#include <benchmark/benchmark.h>
#include "OrderBook.hpp"
#include "AsyncLogger.hpp"
#include <vector>
#include <algorithm>
#include <numeric>
#include <cstdio>

// ── rdtsc fencing (same as Amarjyoti's Clock.h) ──────────────────────────────
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

// ── google-benchmark: raw throughput (keeps your existing BM_OrderInsertion) ─
static void BM_OrderInsertion(benchmark::State& state) {
    Nanomatch::AsyncLogger logger("/dev/null");
    logger.start();
    Nanomatch::OrderBook book(100000, logger);

    uint64_t id = 1;
    for (auto _ : state) {
        book.insert_limit_order(id++, Nanomatch::Side::BUY,
                                100 + (id % 50), 10);
        benchmark::ClobberMemory();
    }
    logger.stop();
}
BENCHMARK(BM_OrderInsertion);

// ── percentile latency benchmark (matches Amarjyoti's synthetic()) ────────────
static void latency_report(const char* label,
                            std::vector<uint64_t>& s, double ghz) {
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

static void run_latency_benchmark() {
    const double ghz = calibrate_ghz();
    const int N = 200000;

    Nanomatch::AsyncLogger logger("/dev/null");
    logger.start();
    Nanomatch::OrderBook book(100000, logger);

    // Seed a realistic resting book — bids below 100000, asks above
    const uint32_t MID = 10000;
    uint64_t id = 1;
    for (int i = 1; i <= 50; ++i) {
        book.insert_limit_order(id++, Nanomatch::Side::BUY,  MID - i, 100);
        book.insert_limit_order(id++, Nanomatch::Side::SELL, MID + i, 100);
    }

    std::vector<uint64_t> samples;
    samples.reserve(N);

    // Sliding window of 256 active orders per side — keeps depth bounded
    // identical structure to Amarjyoti's synthetic()
    const int W = 256;
    std::vector<uint64_t> bidwin(W, 0), askwin(W, 0);
    int bh = 0, ah = 0;

    for (int i = 0; i < N; ++i) {
        uint64_t t0 = rdtsc_start();
        int r = i % 10;

        if (r < 4) {
            // passive bid — evict oldest from window
            if (bidwin[bh % W]) book.cancel_order(bidwin[bh % W]);
            book.insert_limit_order(id, Nanomatch::Side::BUY,
                                    MID - 1 - (id % 20), 50 + (id % 100));
            bidwin[bh % W] = id; ++bh; ++id;

        } else if (r < 8) {
            // passive ask — evict oldest from window
            if (askwin[ah % W]) book.cancel_order(askwin[ah % W]);
            book.insert_limit_order(id, Nanomatch::Side::SELL,
                                    MID + 1 + (id % 20), 50 + (id % 100));
            askwin[ah % W] = id; ++ah; ++id;

        } else if (r == 8) {
            // aggressive buy — crosses spread, triggers matching
            book.insert_limit_order(id++, Nanomatch::Side::BUY,
                                    MID + 3, 30 + (id % 50));
        } else {
            // aggressive sell — crosses spread, triggers matching
            book.insert_limit_order(id++, Nanomatch::Side::SELL,
                                    MID - 3, 30 + (id % 50));
        }

        samples.push_back(rdtsc_end() - t0);
    }

    logger.stop();
    latency_report("Nanomatch add/cancel/match latency", samples, ghz);
}

int main(int argc, char** argv) {
    // Run percentile benchmark first (no google-benchmark overhead)
    run_latency_benchmark();

    // Then run google-benchmark for the throughput number
    printf("\n");
    ::benchmark::Initialize(&argc, argv);
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}