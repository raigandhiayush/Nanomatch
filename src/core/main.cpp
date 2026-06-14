#include "Engine.hpp"
#include <iostream>

int main() {
    const std::string input_file = "data/market_data.bin";
    const std::string log_file = "build/trade_report.txt";
    const size_t max_orders = 100000; 

    std::cout << "[System] Initializing Ultra-Low Latency Matching Engine...\n";
    
    try {
        Nanomatch::Engine engine(input_file, log_file, max_orders);
        
        std::cout << "[System] Processing memory-mapped execution stream...\n";
        engine.run();
        
        std::cout << "[System] Execution successful. Output logs saved to: " << log_file << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[Critical Engine Halt] Error encountered: " << e.what() << "\n";
        return 1;
    }

    return 0;
}