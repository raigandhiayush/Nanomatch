#include "Engine.hpp"
#include <iostream>
#include <chrono>

int main() {
    const std::string input_file = "data/market_data.bin";
    const std::string log_file   = "build/trade_report.txt";
    const size_t price_band      = 100000;   // renamed from max_orders — this is now the
                                              // OrderBook's actual price-band size (see
                                              // OrderBook.hpp ctor), not an order-count cap.

    std::cout << "[System] Initializing Ultra-Low Latency Matching Engine...\n";

    try {
        auto t0 = std::chrono::high_resolution_clock::now();
        Nanomatch::Engine engine(input_file, log_file, price_band);
        auto t1 = std::chrono::high_resolution_clock::now();

        std::cout << "[System] Processing memory-mapped execution stream...\n";
        engine.run();
        auto t2 = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double, std::milli> init_ms    = t1 - t0;
        std::chrono::duration<double, std::milli> process_ms = t2 - t1;

        std::cout << "[System] Execution successful. Output logs saved to: " << log_file << "\n";
        std::cout << "[Timing] OrderBook init: " << init_ms.count()    << " ms\n";
        std::cout << "[Timing] Stream process: " << process_ms.count() << " ms\n";
    } catch (const std::exception& e) {
        std::cerr << "[Critical Engine Halt] Error encountered: " << e.what() << "\n";
        return 1;
    }

    return 0;
}