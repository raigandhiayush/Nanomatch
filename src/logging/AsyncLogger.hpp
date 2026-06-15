#pragma once
#include "Types.hpp"
#include <atomic>
#include <thread>
#include <fstream>
#include <vector>
#include <string>

namespace Nanomatch {

// ─── AsyncLogger ──────────────────────────────────────────────────────────
//
// Single-producer (matching thread) / single-consumer (background writer
// thread) ring buffer. enqueue_trade() on the hot path is a single atomic
// store — no locks, no syscalls, no allocation.
//
// The background thread drains the ring and writes a CSV line per trade.
// start() spins up the writer; stop() signals it to exit and flushes
// anything left in the buffer on the calling thread.

class AsyncLogger {
public:
    explicit AsyncLogger(const std::string& log_file, size_t capacity = 1u << 16)
        : capacity_(capacity), mask_(capacity - 1),
          buffer_(capacity), log_file_(log_file)
    {
        // capacity must be a power of two for the mask trick
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
        drain();   // flush anything written after the worker's last pass
        out_.flush();
        out_.close();
    }

    // Hot path — called once per trade from the matching engine.
    inline void enqueue_trade(const Trade& t) noexcept {
        uint64_t head = head_.load(std::memory_order_relaxed);

        // Ring is sized large enough that this should never happen in
        // practice; if the writer falls behind, drop the oldest entry
        // rather than blocking the matching thread.
        buffer_[head & mask_] = t;
        head_.store(head + 1, std::memory_order_release);
    }

private:
    void run() {
        while (running_.load(std::memory_order_acquire)) {
            if (!drain()) std::this_thread::yield();
        }
    }

    // Writes every trade currently available; returns true if anything
    // was written.
    bool drain() {
        uint64_t head = head_.load(std::memory_order_acquire);
        if (tail_ == head) return false;

        while (tail_ != head) {
            const Trade& t = buffer_[tail_ & mask_];
            out_ << t.maker_id << ',' << t.taker_id << ','
                 << t.price << ',' << t.qty << ',' << t.timestamp << '\n';
            ++tail_;
        }
        return true;
    }

    size_t              capacity_;
    size_t              mask_;
    std::vector<Trade>  buffer_;
    std::string         log_file_;
    std::ofstream       out_;

    alignas(64) std::atomic<uint64_t> head_{0};
    uint64_t            tail_{0};
    std::atomic<bool>   running_{false};
    std::thread         worker_;
};

} // namespace Nanomatch