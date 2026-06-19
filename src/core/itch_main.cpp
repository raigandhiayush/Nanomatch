#include "OrderBook.hpp"
#include "AsyncLogger.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>

namespace Nanomatch {
    // Restored the clean, single-book forward declaration signature
    void parse_itch_file(const std::string& filepath, Nanomatch::OrderBook& book);
}

// Global vector to collect latency samples without dynamic resizing penalties during the run
std::vector<uint64_t> latency_samples;

int main() {
    const std::string itch_file = "data/01302019.NASDAQ_ITCH50";
    const std::string log_file  = "build/itch_trade_report.txt";
    
    // Pre-allocate space for 50 million samples to avoid runtime vector resizing overhead
    latency_samples.reserve(50000000);

    std::cout << "[System] Initializing OrderBook & AsyncLogger...\n";
    Nanomatch::AsyncLogger logger(log_file);
    logger.start();

    // Instantiate exactly one shared order book container
    Nanomatch::OrderBook book(1048576, logger);

    std::cout << "[System] Mapping and processing ITCH data stream with latency tracking...\n";
    
    auto start_time = std::chrono::high_resolution_clock::now();

    // Fire the un-filtered parsing stream directly into our lone book
    Nanomatch::parse_itch_file(itch_file, book);

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;
    logger.stop();

    std::cout << "[Success] ITCH file parsing stream processed in " << elapsed.count() << " seconds.\n";

    std::sort(latency_samples.begin(), latency_samples.end());
    if (!latency_samples.empty()) {
        auto get_pct = [](double pct) {
            size_t idx = static_cast<size_t>(pct * latency_samples.size() / 100.0);
            return latency_samples[std::min(idx, latency_samples.size() - 1)];
        };
        std::cout << "=== Latency Percentiles (Cycles) ===\n";
        std::cout << "p50:   " << get_pct(50.0)  << "\n";
        std::cout << "p90:   " << get_pct(90.0)  << "\n";
        std::cout << "p99:   " << get_pct(99.0)  << "\n";
        std::cout << "p99.9: " << get_pct(99.9)  << "\n";
    }

    return 0;
}