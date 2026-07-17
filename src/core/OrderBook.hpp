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
    Price    best_bid {UINT32_MAX};
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
        : cap_(round_up_pow2(cap)), mask_(cap_ - 1)
    {
        entries_.assign(cap_, Entry{0, EMPTY});
    }

    void insert(OrderId key, uint32_t val) noexcept {
        uint32_t slot = hash(key) & mask_;
        while (entries_[slot].val != EMPTY && entries_[slot].key != key)
            slot = (slot + 1) & mask_;
        entries_[slot].key = key;
        entries_[slot].val = val;
    }

    // Returns NULL_IDX if not found
    [[nodiscard]] uint32_t find(OrderId key) const noexcept {
        uint32_t slot = hash(key) & mask_;
        while (entries_[slot].val != EMPTY) {
            if (entries_[slot].key == key) return entries_[slot].val;
            slot = (slot + 1) & mask_;
        }
        return EMPTY;
    }

    void erase(OrderId key) noexcept {
        uint32_t slot = hash(key) & mask_;
        while (entries_[slot].val != EMPTY && entries_[slot].key != key)
            slot = (slot + 1) & mask_;
        if (entries_[slot].val == EMPTY) return;

        entries_[slot].val = EMPTY;
        uint32_t cur = slot;
        while (true) {
            uint32_t nxt = (cur + 1) & mask_;
            if (entries_[nxt].val == EMPTY) break;
            uint32_t nat = hash(entries_[nxt].key) & mask_;
            // nxt belongs at nat; if nat is NOT between cur+1 and nxt (wrapping),
            // then nxt was displaced past cur and should move back
            if (((nxt - nat) & mask_) >= ((nxt - cur) & mask_)) {
                entries_[cur] = entries_[nxt];
                entries_[nxt].val = EMPTY;
                cur = nxt;
            } else {
                break;
            }
        }
    }

private:
    struct Entry {
        OrderId   key;
        uint32_t  val;
    };

    static uint32_t round_up_pow2(uint32_t v) noexcept {
        if (v <= 1) return 1;
        --v;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        return v + 1;
    }

    static uint32_t hash(OrderId key) noexcept {
        // Murmur finalizer — good distribution for sequential IDs
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        return static_cast<uint32_t>(key);
    }

    uint32_t             cap_   {0};
    uint32_t             mask_  {0};
    std::vector<Entry>   entries_;
};

// ─── OrderBook ───────────────────────────────────────────────────────────────

class OrderBook {
public:
    explicit OrderBook(size_t price_band, AsyncLogger& logger, size_t max_orders = MAX_ORDERS);

    // Hot path — called on every message
    bool insert_limit_order(OrderId id, Side side, Price price, Quantity qty) noexcept;
    void cancel_order      (OrderId id)                                        noexcept;
    void process_market_order(Side side, Quantity qty)                         noexcept;

    // Called by Engine for partial fills from external feed
    bool execute_order(OrderId id, Quantity fill_qty)                          noexcept;
    // Returns true if the order was fully removed from the book.
    bool reduce_order_qty(OrderId id, Quantity cancel_qty) noexcept;
    [[nodiscard]] const IdMap& get_id_map() const noexcept { return id_map_; }
    [[nodiscard]] const OrderPool& get_pool() const noexcept { return pool_; }

    // Accessors for benchmarks / tests
    const TopOfBook& tob()         const noexcept { return tob_; }
    const PriceLevel& bid_level(Price p) const noexcept { return bids_[to_idx(p)]; }
    const PriceLevel& ask_level(Price p) const noexcept { return asks_[to_idx(p)]; }

private:
    // ── data members in access-frequency order ───────────────────────────────
    TopOfBook              tob_;          // own cache line, always L1-hot
    OrderPool              pool_;         // flat order storage
    IdMap                  id_map_;       // O(1) id → pool index
    AsyncLogger&           logger_;

    // ── relative sliding price band ───────────────────────────────────────────
    //
    // Bug fix / memory fix: bids_/asks_ used to be indexed directly by the
    // ABSOLUTE price (bids_[price]), so the array had to span [0, price_band)
    // starting at $0 -- to hold a $250 stock you needed price_band > 2,500,000
    // ticks, and a cheap $2 stock wasted almost the entire array. Ported from
    // github.com/Amarjyoti-Chakravorty/NanoMatch: bids_/asks_ are now indexed
    // by (price - base_), where base_ is anchored to wherever the ticker
    // actually trades (set from the first order this book ever sees, centered
    // so that price sits mid-array). This means a small, fixed-size window
    // (e.g. price_band=200,000 -> a $20 span at ITCH's $0.0001 tick size)
    // works for ANY ticker regardless of its absolute price level, instead of
    // needing a window sized to the most expensive stock you might see.
    Price   base_       {0};
    bool    base_set_   {false};

    [[nodiscard]] long to_idx(Price p) const noexcept {
        return static_cast<long>(p) - static_cast<long>(base_);
    }
    [[nodiscard]] bool in_band(long idx) const noexcept {
        return idx >= 0 && idx < static_cast<long>(bids_.size());
    }

    // Price-level arrays: indexed by (price - base_), a relative slot in the
    // sliding window, not by absolute price. Sized to the configured band
    // width in the constructor.
    std::vector<PriceLevel> bids_;        // bids_[idx] = level at price (base_+idx)
    std::vector<PriceLevel> asks_;        // asks_[idx] = level at price (base_+idx)

    std::vector<uint64_t>   bid_bitmap_;  // one bit per active bid price level
    std::vector<uint64_t>   ask_bitmap_;  // one bit per active ask price level
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