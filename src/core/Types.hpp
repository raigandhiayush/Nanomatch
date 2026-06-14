#pragma once
#include <cstdint>

namespace Nanomatch{
    using OrderId=uint64_t;
    using Price=uint32_t; // fixed point
    using Quantity=uint32_t;

    enum class Side : uint8_t{
        BUY=0,
        SELL=1
    };
    
    enum class OrderType : uint8_t{
        LIMIT=0,
        MARKET=1
    };

    struct alignas(64) Order{
        OrderId id;
        Price price;
        Quantity qty;
        Side side;
        Order* next{nullptr};
        Order* prev{nullptr};
    };

    struct alignas(64) Trade{
        OrderId maker_id;
        OrderId taker_id;
        Price price;
        Quantity qty;
        uint64_t timestamp;
    };

    struct PriceLevel{
        Price price{0};
        Quantity total_volume{0};
        Order* head{nullptr};
        Order* tail{nullptr};

        inline bool empty() const noexcept{
            return head==nullptr;
        }
    };

    inline uint64_t rdtsc() noexcept {
        unsigned int lo, hi;
        __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
        return ((uint64_t)hi << 32) | lo;
    }

    constexpr int PRICE_BAND = 32768;
    constexpr int INVALID_IDX = -1;
}