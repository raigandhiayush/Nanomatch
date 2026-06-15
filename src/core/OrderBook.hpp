#pragma once
#include "Types.hpp"
#include "AsyncLogger.hpp"
#include <vector>
#include <cstring>
#include <sys/mman.h>

namespace Nanomatch {

// ─── constants ───────────────────────────────────────────────────────────────
static constexpr uint32_t MAX_ORDERS   = 1u << 20;   // 1 048 576 slots
static constexpr uint32_t NULL_IDX     = UINT32_MAX;  // sentinel "no order"

// ─── flat order pool ─────────────────────────────────────────────────────────
//
// Every Order lives at a stable index in a contiguous array.
// next_idx / prev_idx are uint32_t offsets into that array — no pointer
// chasing, hardware prefetcher can predict strides.
//
// Free-list reuse: deallocated slots are pushed onto a singly-linked
// free list (reusing next_idx as the free-list link).

class OrderPool {
public:
    explicit OrderPool(uint32_t capacity = MAX_ORDERS)
        : capacity_(capacity)
    {
        // MAP_POPULATE pre-faults every page → zero page-fault latency
        // during trading.  mlock prevents the OS from swapping these pages.
        void* raw = mmap(nullptr,
                         capacity_ * sizeof(Order),
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                         -1, 0);
        if (raw == MAP_FAILED) throw std::bad_alloc{};
        pool_ = static_cast<Order*>(raw);
        mlock(pool_, capacity_ * sizeof(Order));

        // Build free list: slot i → slot i+1
        for (uint32_t i = 0; i < capacity_ - 1; ++i)
            pool_[i].next_idx = i + 1;
        pool_[capacity_ - 1].next_idx = NULL_IDX;
        free_head_ = 0;
    }

    ~OrderPool() { munmap(pool_, capacity_ * sizeof(Order)); }

    OrderPool(const OrderPool&)            = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    [[nodiscard]] uint32_t allocate() noexcept {
        if (__builtin_expect(free_head_ == NULL_IDX, 0)) return NULL_IDX;
        uint32_t idx = free_head_;
        free_head_   = pool_[idx].next_idx;
        pool_[idx].next_idx = NULL_IDX;
        pool_[idx].prev_idx = NULL_IDX;
        return idx;
    }

    void deallocate(uint32_t idx) noexcept {
        pool_[idx].next_idx = free_head_;
        free_head_ = idx;
    }

    Order&       operator[](uint32_t idx)       noexcept { return pool_[idx]; }
    const Order& operator[](uint32_t idx) const noexcept { return pool_[idx]; }

private:
    Order*   pool_      {nullptr};
    uint32_t free_head_ {0};
    uint32_t capacity_  {0};
};

// ─── top-of-book cache ───────────────────────────────────────────────────────
//
// Kept on its own cache line so the hot matching path touches exactly
// one line to decide whether a new order crosses.

struct alignas(64) TopOfBook {
    Price    best_bid {0};
    Price    best_ask {UINT32_MAX};
    Quantity bid_qty  {0};
    Quantity ask_qty  {0};
};

// ─── id → pool index map ─────────────────────────────────────────────────────
//
// Replaces std::unordered_map<OrderId, Order*>.
// Robin-hood open-addressing, power-of-2 capacity → & instead of %.
// Stores uint32_t indices (4 B) not pointers (8 B).

class IdMap {
public:
    static constexpr uint32_t EMPTY = NULL_IDX;

    explicit IdMap(uint32_t cap = MAX_ORDERS * 2)
        : cap_(cap), mask_(cap - 1)
    {
        // cap must be a power of 2
        keys_.assign(cap_, 0);
        vals_.assign(cap_, EMPTY);
    }

    void insert(OrderId key, uint32_t val) noexcept {
        uint32_t slot = hash(key) & mask_;
        while (vals_[slot] != EMPTY && keys_[slot] != key)
            slot = (slot + 1) & mask_;
        keys_[slot] = key;
        vals_[slot] = val;
    }

    // Returns NULL_IDX if not found
    [[nodiscard]] uint32_t find(OrderId key) const noexcept {
        uint32_t slot = hash(key) & mask_;
        while (vals_[slot] != EMPTY) {
            if (keys_[slot] == key) return vals_[slot];
            slot = (slot + 1) & mask_;
        }
        return EMPTY;
    }

    void erase(OrderId key) noexcept {
        uint32_t slot = hash(key) & mask_;
        while (vals_[slot] != EMPTY && keys_[slot] != key)
            slot = (slot + 1) & mask_;
        if (vals_[slot] == EMPTY) return;
        // Backshift deletion to preserve probe chains
        vals_[slot] = EMPTY;
        uint32_t cur = slot;
        while (true) {
            uint32_t nxt = (cur + 1) & mask_;
            if (vals_[nxt] == EMPTY) break;
            uint32_t nat = hash(keys_[nxt]) & mask_;
            // Move nxt into cur if nat is not between nxt and cur
            if (((nxt - nat) & mask_) > ((nxt - cur) & mask_)) {
                keys_[cur] = keys_[nxt];
                vals_[cur] = vals_[nxt];
                vals_[nxt] = EMPTY;
                cur = nxt;
            } else {
                break;
            }
        }
    }

private:
    static uint32_t hash(OrderId key) noexcept {
        // Murmur finalizer — good distribution for sequential IDs
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        return static_cast<uint32_t>(key);
    }

    uint32_t              cap_  {0};
    uint32_t              mask_ {0};
    std::vector<OrderId>  keys_;
    std::vector<uint32_t> vals_;
};

// ─── OrderBook ───────────────────────────────────────────────────────────────

class OrderBook {
public:
    explicit OrderBook(size_t /*max_orders_unused*/, AsyncLogger& logger);

    // Hot path — called on every message
    void insert_limit_order(OrderId id, Side side, Price price, Quantity qty) noexcept;
    void cancel_order      (OrderId id)                                        noexcept;
    void process_market_order(Side side, Quantity qty)                         noexcept;

    // Called by Engine for partial fills from external feed
    void execute_order(OrderId id, Quantity fill_qty)                          noexcept;
    // Inside class OrderBook in OrderBook.hpp
    void reduce_order_qty(OrderId id, Quantity cancel_qty) noexcept;
    [[nodiscard]] const IdMap& get_id_map() const noexcept { return id_map_; }
    [[nodiscard]] const OrderPool& get_pool() const noexcept { return pool_; }

    // Accessors for benchmarks / tests
    const TopOfBook& tob()         const noexcept { return tob_; }
    const PriceLevel& bid_level(Price p) const noexcept { return bids_[p]; }
    const PriceLevel& ask_level(Price p) const noexcept { return asks_[p]; }

private:
    // ── data members in access-frequency order ───────────────────────────────
    TopOfBook              tob_;          // own cache line, always L1-hot
    OrderPool              pool_;         // flat order storage
    IdMap                  id_map_;       // O(1) id → pool index
    AsyncLogger&           logger_;

    // Price-level arrays: indexed directly by Price (fixed-point integer).
    // Pre-allocated to PRICE_BAND in constructor — no resize ever.
    std::vector<PriceLevel> bids_;        // bids_[p] = level at price p
    std::vector<PriceLevel> asks_;        // asks_[p] = level at price p

    // ── level linked-list helpers (index-based, no pointer chasing) ──────────
    void level_append(PriceLevel& level, uint32_t idx)        noexcept;
    void level_remove(PriceLevel& level, uint32_t idx)        noexcept;

    // ── top-of-book maintenance ──────────────────────────────────────────────
    void update_best_bid() noexcept;
    void update_best_ask() noexcept;

    // ── inner matching loop (extracted to avoid duplication) ─────────────────
    Quantity match_against_asks(OrderId taker_id, Price limit, Quantity qty) noexcept;
    Quantity match_against_bids(OrderId taker_id, Price limit, Quantity qty) noexcept;
};

} // namespace Nanomatch