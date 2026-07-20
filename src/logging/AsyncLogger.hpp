#pragma once
#include "Types.hpp"
#include "SPSCQueue.hpp"
#include <atomic>
#include <thread>
#include <fstream>
#include <iostream>
#include <string>
#include <chrono>

namespace Nanomatch {

class AsyncLogger {
public:
    static constexpr size_t kCapacity = 1u << 22; // ~4M slots

    explicit AsyncLogger(const std::string& log_file) : log_file_(log_file) {}

    ~AsyncLogger() { stop(); }

    AsyncLogger(const AsyncLogger&)            = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void start() {
        if (running_.exchange(true, std::memory_order_acq_rel)) return;
        out_.open(log_file_, std::ios::out | std::ios::trunc);
        out_ << "maker_id,taker_id,price,qty,timestamp\n";
        worker_ = std::thread([this] { run(); });
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (worker_.joinable()) worker_.join();
        drain();
        out_.flush();
        out_.close();
        if (dropped_.load(std::memory_order_relaxed) > 0) {
            std::cerr << "[AsyncLogger] WARNING: " << dropped_.load()
                      << " trades dropped due to full ring buffer\n";
        }
    }

    // Hot path — completely non-blocking, overwrite-safe.
    inline void enqueue_trade(const Trade& t) noexcept {
        if (!queue_.emplace(t)) {
            dropped_.fetch_add(1, std::memory_order_relaxed); // drop, don't stall matching
        }
    }

    uint64_t dropped_count() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    void run() {
        while (running_.load(std::memory_order_acquire)) {
            if (!drain()) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    }

    bool drain() {
        Trade t;
        bool any = false;
        while (queue_.pop(t)) {
            out_ << t.maker_id << ',' << t.taker_id << ','
                 << t.price << ',' << t.qty << ',' << t.timestamp << '\n';
            any = true;
        }
        return any;
    }

    SPSCQueue<Trade, kCapacity> queue_;
    std::string                 log_file_;
    std::ofstream               out_;

    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool>     running_{false};
    std::thread           worker_;
};

} // namespace Nanomatch