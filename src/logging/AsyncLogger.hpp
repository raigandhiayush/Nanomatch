#pragma once
#include "SPSCQueue.hpp"
#include <fstream>
#include <thread>
#include <atomic>

namespace Nanomatch {
class AsyncLogger {
public:
    AsyncLogger(const std::string& filename)
        : file_(filename, std::ios::out | std::ios::binary), running_(false) {}

    ~AsyncLogger() { stop(); }

    void start() {
        running_ = true;
        worker_thread_ = std::thread(&AsyncLogger::log_loop, this);
    }

    void stop() {
        if (!running_) return; // Prevent double-stopping
        running_ = false;

        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
    }

    inline bool enqueue_trade(const Trade& event) noexcept {
        return queue_.emplace(event);
    }

private:
    std::ofstream file_;
    std::atomic<bool> running_;
    std::thread worker_thread_;
    SPSCQueue<Trade,65536> queue_; // Assuming your ring buffer class name

    void log_loop() {
        Trade event;
        // Keep draining as long as the engine is running or items remain in the ring buffer
        while (running_ || queue_.pop(event)) {
            // Process the 'event' populated by the while statement condition check
            // file_.write(...) or your binary log serialization format goes here
        }
    }
};
}