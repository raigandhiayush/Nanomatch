#pragma once
#include "Types.hpp"
#include <atomic>
#include <thread>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>

namespace Nanomatch {

class AsyncLogger {
public:
    // Expanded baseline capacity to 1u << 22 (~4 million slots) to handle market sweeps safely
    explicit AsyncLogger(const std::string& log_file, size_t capacity = 1u << 22)
        : capacity_(capacity), mask_(capacity - 1),
          buffer_(capacity), log_file_(log_file)
    {
    }

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
    }

    // Hot path — completely non-blocking, but overwrite-safe
    inline void enqueue_trade(const Trade& t) noexcept {
        uint64_t head = head_.load(std::memory_order_relaxed);
        
        // Check if ring buffer is full. If so, drop to protect core matching latency.
        // In production, you would handle drops via an off-path error counter.
        if (__builtin_expect((head - tail_cached_) >= capacity_, 0)) {
            tail_cached_ = tail_atomic_.load(std::memory_order_acquire);
            if ((head - tail_cached_) >= capacity_) {
                return; // Buffer full, drop trade to preserve matching loops
            }
        }

        buffer_[head & mask_] = t;
        head_.store(head + 1, std::memory_order_release);
    }

private:
    void run() {
        while (running_.load(std::memory_order_acquire)) {
            // Drop core stress by sleeping for 10 microseconds when queue is empty
            if (!drain()) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    }

    bool drain() {
        uint64_t head = head_.load(std::memory_order_acquire);
        uint64_t tail = tail_atomic_.load(std::memory_order_relaxed);
        if (tail == head) return false;

        while (tail != head) {
            const Trade& t = buffer_[tail & mask_];
            out_ << t.maker_id << ',' << t.taker_id << ','
                 << t.price << ',' << t.qty << ',' << t.timestamp << '\n';
            ++tail;
        }

        tail_atomic_.store(tail, std::memory_order_release);
        return true;
    }

    size_t              capacity_;
    size_t              mask_;
    std::vector<Trade>  buffer_;
    std::string         log_file_;
    std::ofstream       out_;

    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_atomic_{0}; 
    
    // Thread-local cache of the tail position to minimize cross-thread atomic contention
    alignas(64) uint64_t tail_cached_{0}; 
    
    std::atomic<bool>   running_{false};
    std::thread         worker_;
};

} // namespace Nanomatch