#pragma once
#include "Types.hpp"
#include <atomic>
#include <array>
#include <memory>

namespace Nanomatch {
    template <typename T, size_t Capacity>
    class SPSCQueue {
        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

    public:
        SPSCQueue() : head_(0), tail_(0), buffer_(std::make_unique<std::array<T, Capacity>>()) {}

        inline bool emplace(const T& item) noexcept {
            const size_t current_tail = tail_.load(std::memory_order_relaxed);
            const size_t current_head = head_.load(std::memory_order_acquire);

            if ((current_tail - current_head) == Capacity) {
                return false; 
            }

            (*buffer_)[current_tail & (Capacity - 1)] = item;
            tail_.store(current_tail + 1, std::memory_order_release);
            return true;
        }

        inline bool pop(T& item) noexcept {
            const size_t current_head = head_.load(std::memory_order_relaxed);
            const size_t current_tail = tail_.load(std::memory_order_acquire);

            if (current_head == current_tail) {
                return false;
            }

            item = (*buffer_)[current_head & (Capacity - 1)];
            head_.store(current_head + 1, std::memory_order_release);
            return true;
        }

    private:
        alignas(64) std::atomic<size_t> head_{0};
        alignas(64) std::atomic<size_t> tail_{0};
        std::unique_ptr<std::array<T, Capacity>> buffer_;
    };
}