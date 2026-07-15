#pragma once
#include "Types.hpp"
#include <atomic>
#include <array>
#include <memory>
#include <type_traits>

namespace Nanomatch {
    template <typename T, size_t Capacity>
    class SPSCQueue {
        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
        static_assert(std::is_nothrow_copy_assignable_v<T>,
                      "T must be nothrow copy-assignable for the noexcept hot path");

    public:
        SPSCQueue() : buffer_(std::make_unique<std::array<T, Capacity>>()) {}

        SPSCQueue(const SPSCQueue&)            = delete;
        SPSCQueue& operator=(const SPSCQueue&) = delete;

        // Producer side — single writer thread only.
        inline bool emplace(const T& item) noexcept {
            const size_t tail = tail_cached_;

            if (__builtin_expect((tail - head_cached_) >= Capacity, 0)) {
                head_cached_ = head_.load(std::memory_order_acquire); // refresh, cheap in the common case
                if ((tail - head_cached_) >= Capacity) {
                    return false; // genuinely full
                }
            }

            (*buffer_)[tail & (Capacity - 1)] = item;
            tail_.store(tail + 1, std::memory_order_release);
            tail_cached_ = tail + 1;
            return true;
        }

        // Consumer side — single reader thread only.
        inline bool pop(T& item) noexcept {
            const size_t head = head_cached_consumer_;

            if (head == tail_cached_consumer_) {
                tail_cached_consumer_ = tail_.load(std::memory_order_acquire);
                if (head == tail_cached_consumer_) {
                    return false; // genuinely empty
                }
            }

            item = (*buffer_)[head & (Capacity - 1)];
            head_.store(head + 1, std::memory_order_release);
            head_cached_consumer_ = head + 1;
            return true;
        }

        static constexpr size_t capacity() noexcept { return Capacity; }

    private:
        alignas(64) std::atomic<size_t> head_{0};
        alignas(64) std::atomic<size_t> tail_{0};

        // Producer-local cache — touched only by the writer thread.
        size_t head_cached_{0};
        size_t tail_cached_{0};

        // Consumer-local cache — touched only by the reader thread.
        size_t tail_cached_consumer_{0};
        size_t head_cached_consumer_{0};

        std::unique_ptr<std::array<T, Capacity>> buffer_;
    };
}