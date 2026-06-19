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

    struct Order {
        OrderId  id;        // 8  — 8-byte fields first
        uint32_t next_idx;  // 4  — then 4-byte fields together
        uint32_t prev_idx;  // 4
        Price    price;     // 4
        Quantity qty;       // 4
        Side     side;      // 1  — 1-byte fields last
        uint8_t  _pad[39];  // fill to 64
    };                    // = 64 bytes exactly
    static_assert(sizeof(Order) == 64);

    struct alignas(32) Trade {  // 32-byte align: 2 fit per cache line
        OrderId  maker_id;   // 8
        OrderId  taker_id;   // 8
        Price    price;      // 4
        Quantity qty;        // 4
        uint64_t timestamp;  // 8
    };                       // = 32 bytes, clean
    static_assert(sizeof(Trade) == 32);

    struct PriceLevel {
        Price    price        {0};
        Quantity total_volume {0};
        uint32_t head_idx     {UINT32_MAX}; 
        uint32_t tail_idx     {UINT32_MAX};
        uint32_t order_count  {0};
        uint8_t  _pad[8]; 

        bool empty() const noexcept { return head_idx == UINT32_MAX; }
    };

    inline uint64_t rdtsc() noexcept {
        __asm__ __volatile__ ("lfence" ::: "memory");
        unsigned int lo, hi;
        __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
        return ((uint64_t)hi << 32) | lo;
    }

    constexpr uint32_t PRICE_BAND = 16'000'000;
    constexpr int INVALID_IDX = -1;
}