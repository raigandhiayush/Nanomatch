#include "OrderBook.hpp"
#include <algorithm>

namespace Nanomatch {

static uint32_t round_up_pow2(uint32_t v) noexcept {
    if (v == 0) return 1;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

OrderBook::OrderBook(size_t price_band, AsyncLogger& logger, size_t max_orders)
    : pool_(static_cast<uint32_t>(max_orders)),
      id_map_(round_up_pow2(static_cast<uint32_t>(max_orders) * 2u)),
      logger_(logger),
      bids_(price_band),
      asks_(price_band),
      bid_bitmap_((price_band + 63) / 64),
      ask_bitmap_((price_band + 63) / 64)
{
    // pool_ and id_map_ are sized to the expected maximum live order count.
    // bids_/asks_ are sized to price_band slots in the *relative* sliding
    // window (see base_ in the header) -- base_ isn't known yet (it's
    // anchored to whichever price the first order carries), so there's
    // nothing to precompute per-slot here anymore. Levels default-construct
    // to price 0 / empty, which is fine since price_idx is always derived
    // from base_ + relative index on demand, never read from the level.
}

// ─── level linked-list helpers ───────────────────────────────────────────

void OrderBook::level_append(PriceLevel& level, uint32_t idx) noexcept {
    Order& o = pool_[idx];

    o.prev_idx = level.tail_idx;
    o.next_idx = NULL_IDX;

    // o.price stores the relative index into bids_/asks_. Use it directly.
    uint32_t price_idx = o.price; // relative index in the sliding window

    if (level.head_idx == NULL_IDX) {
        level.head_idx = idx;
        // if we appended into an empty level, set the corresponding bitmap bit
        if (&level == &bids_[price_idx]) {
            bid_bitmap_[price_idx >> 6] |= (1ull << (price_idx & 63));
        } else {
            ask_bitmap_[price_idx >> 6] |= (1ull << (price_idx & 63));
        }
    } else {
        pool_[level.tail_idx].next_idx = idx;
    }
    level.tail_idx = idx;

    level.total_volume += o.qty;
    ++level.order_count;
}

void OrderBook::level_remove(PriceLevel& level, uint32_t idx) noexcept {
    Order& o = pool_[idx];

    if (o.prev_idx == NULL_IDX) {
        level.head_idx = o.next_idx;
    } else {
        pool_[o.prev_idx].next_idx = o.next_idx;
    }

    if (o.next_idx == NULL_IDX) {
        level.tail_idx = o.prev_idx;
    } else {
        pool_[o.next_idx].prev_idx = o.prev_idx;
    }

    level.total_volume -= o.qty;
    --level.order_count;

    if (level.empty()) {
        if (&level == &bids_[o.price]) {
            bid_bitmap_[o.price >> 6] &= ~(1ull << (o.price & 63));
        } else {
            ask_bitmap_[o.price >> 6] &= ~(1ull << (o.price & 63));
        }
    }

    o.next_idx = NULL_IDX;
    o.prev_idx = NULL_IDX;
}

static inline uint32_t find_prev_set_bit(const std::vector<uint64_t>& bitmap, uint32_t start) noexcept {
    uint32_t word = start >> 6;
    uint32_t bit  = start & 63;
    uint64_t mask = (~uint64_t{0}) >> (63 - bit);
    uint64_t word_bits = bitmap[word] & mask;
    while (true) {
        if (word_bits != 0) {
            return (word << 6) | (63u - __builtin_clzll(word_bits));
        }
        if (word == 0) break;
        --word;
        word_bits = bitmap[word];
    }
    return UINT32_MAX;
}

static inline uint32_t find_next_set_bit(const std::vector<uint64_t>& bitmap, uint32_t start) noexcept {
    uint32_t word = start >> 6;
    uint32_t bit  = start & 63;
    uint64_t mask = ~uint64_t{0} << bit;
    uint64_t word_bits = bitmap[word] & mask;
    const uint32_t last_word = bitmap.size() - 1;
    while (true) {
        if (word_bits != 0) {
            return (word << 6) | __builtin_ctzll(word_bits);
        }
        if (word == last_word) break;
        ++word;
        word_bits = bitmap[word];
    }
    return UINT32_MAX;
}

// ─── top-of-book maintenance ─────────────────────────────────────────────
//
// Called only when the *current* best level may have just become empty.
// Scans outward from the current best toward the back of the book — in
// the common case the new best is immediately adjacent, so this is O(1)
// amortized rather than a full PRICE_BAND sweep.

void OrderBook::update_best_bid() noexcept {
    Price start = (tob_.best_bid == UINT32_MAX) ? static_cast<Price>(bids_.size() - 1)
                                                 : tob_.best_bid;
    uint32_t next = find_prev_set_bit(bid_bitmap_, start);
    if (next != UINT32_MAX) {
        tob_.best_bid = next;
        tob_.bid_qty  = bids_[next].total_volume;
    } else {
        tob_.best_bid = UINT32_MAX;
        tob_.bid_qty  = 0;
    }
}

void OrderBook::update_best_ask() noexcept {
    Price start = tob_.best_ask;
    if (start >= asks_.size()) start = 0;
    uint32_t next = find_next_set_bit(ask_bitmap_, start);
    if (next != UINT32_MAX) {
        tob_.best_ask = next;
        tob_.ask_qty  = asks_[next].total_volume;
    } else {
        tob_.best_ask = UINT32_MAX;
        tob_.ask_qty  = 0;
    }
}

// ─── matching ─────────────────────────────────────────────────────────────
//
// Walks price levels from the current best outward, FIFO within each
// level. Returns the quantity left unfilled (0 if the taker was fully
// matched).

Quantity OrderBook::match_against_asks(OrderId taker_id, Price limit, Quantity qty) noexcept {
    Quantity remaining = qty;

    while (remaining > 0) {
        Price best = tob_.best_ask;
        if (best > limit) break;               // also catches "no asks" (best == UINT32_MAX)

        PriceLevel& level = asks_[best];

        uint32_t idx = level.head_idx;
        while (idx != NULL_IDX && remaining > 0) {
            Order& o = pool_[idx];
            uint32_t next = o.next_idx;
            if (next != NULL_IDX) __builtin_prefetch(&pool_[next], 0, 1);

            Quantity match_qty = std::min(remaining, o.qty);
            logger_.enqueue_trade(Trade{o.id, taker_id, level.price, match_qty, rdtsc()});

            remaining        -= match_qty;
            o.qty             -= match_qty;
            level.total_volume -= match_qty;

            if (o.qty == 0) {
                level_remove(level, idx);
                id_map_.erase(o.id);
                pool_.deallocate(idx);
                if (live_order_count_ > 0) --live_order_count_;
            }
            idx = next;
        }

        if (level.empty()) {
            update_best_ask();
        } else {
            tob_.ask_qty = level.total_volume;
        }
    }

    return remaining;
}

Quantity OrderBook::match_against_bids(OrderId taker_id, Price limit, Quantity qty) noexcept {
    Quantity remaining = qty;

    while (remaining > 0) {
        Price best = tob_.best_bid;
        if (best == UINT32_MAX || best < limit) break;

        PriceLevel& level = bids_[best];
        if (level.empty()) break;               // no bids at all (best == 0, bids_[0] empty)

        uint32_t idx = level.head_idx;
        while (idx != NULL_IDX && remaining > 0) {
            Order& o = pool_[idx];
            uint32_t next = o.next_idx;
            if (next != NULL_IDX) __builtin_prefetch(&pool_[next], 0, 1);

            Quantity match_qty = std::min(remaining, o.qty);
            logger_.enqueue_trade(Trade{o.id, taker_id, level.price, match_qty, rdtsc()});

            remaining          -= match_qty;
            o.qty              -= match_qty;
            level.total_volume -= match_qty;

            if (o.qty == 0) {
                level_remove(level, idx);
                id_map_.erase(o.id);
                pool_.deallocate(idx);
                if (live_order_count_ > 0) --live_order_count_;
            }
            idx = next;
        }

        if (level.empty()) {
            update_best_bid();
        } else {
            tob_.bid_qty = level.total_volume;
        }
    }

    return remaining;
}

// ─── hot path ops ─────────────────────────────────────────────────────────

bool OrderBook::insert_limit_order(OrderId id, Side side, Price price, Quantity qty) noexcept {
    // Anchor the sliding window from the very first order this book ever
    // sees, centered so that price sits mid-array. If that anchor turns out
    // to be a bad fit for a subsequent early order, in_band()/rebase_base()
    // below (gated by BOOTSTRAP_REBASE_LIMIT) already re-centers the window
    // while the book is still small -- so there's no need to hold orders
    // back first. (This used to buffer the first 8 orders before anchoring
    // at their median, but that meant any book that saw fewer than 8 orders
    // in its entire lifetime never anchored at all and silently never
    // traded -- a real problem for thin/illiquid symbols, and the reason a
    // 3-order unit test never saw any fills.)
    if (!base_set_) {
        Price half = static_cast<Price>(bids_.size() / 2);
        base_ = (price < half) ? 0 : static_cast<Price>(price - half);
        base_set_ = true;
    }

    // Convert absolute 'price' into a relative index into the sliding window
    long rel = to_idx(price); // relative index into bids_/asks_
    if (!in_band(rel)) {
        // If the book is still small we can attempt to rebase so this order
        // fits instead of rejecting; this avoids anchoring to an early
        // outlier. Only do this when live_order_count_ is small.
        if (live_order_count_ <= BOOTSTRAP_REBASE_LIMIT) {
            // compute a new base that centers the requested price
            Price half = static_cast<Price>(bids_.size() / 2);
            Price desired_base = (price < half) ? 0 : static_cast<Price>(price - half);
            rebase_base(desired_base);
            rel = to_idx(price);
        }
        if (!in_band(rel)) {
            return false; // still out of band
        }
    }
    uint32_t pidx = static_cast<uint32_t>(rel);

    Quantity remaining;

    if (side == Side::BUY) {
        // Match against asks using relative index
        remaining = match_against_asks(id, pidx, qty);
        if (remaining == 0) {
            return false; // fully matched; nothing rests on the book
        }

        uint32_t slot = pool_.allocate();
        if (slot == NULL_IDX) {
            return false;     // pool exhausted — drop the order
        }

        Order& o = pool_[slot];
        o.id    = id;
        o.price = pidx;                         // store relative index in order
        o.qty   = remaining;
        o.side  = side;

        PriceLevel& level = bids_[pidx];
        // store the absolute price on the level for logging
        if (level.empty()) level.price = price;
        level_append(level, slot);
        id_map_.insert(id, slot);
        ++live_order_count_;

        if (tob_.best_bid == UINT32_MAX || pidx > tob_.best_bid) {
            tob_.best_bid = pidx;
            tob_.bid_qty  = level.total_volume;
        }
        return true;
    } else {
        // SELL side
        remaining = match_against_bids(id, pidx, qty);
        if (remaining == 0) {
            return false; // fully matched; nothing rests on the book
        }

        uint32_t slot = pool_.allocate();
        if (slot == NULL_IDX) {
            return false;
        }

        Order& o = pool_[slot];
        o.id    = id;
        o.price = pidx;
        o.qty   = remaining;
        o.side  = side;

        PriceLevel& level = asks_[pidx];
        if (level.empty()) level.price = price;
        level_append(level, slot);
        id_map_.insert(id, slot);
        ++live_order_count_;

        if (tob_.best_ask == UINT32_MAX || pidx < tob_.best_ask) {
            tob_.best_ask = pidx;
            tob_.ask_qty  = level.total_volume;
        }
        return true;
    }
}

void OrderBook::rebase_base(Price new_base) noexcept {
    if (new_base == base_) return;
    const size_t N = bids_.size();

    std::vector<PriceLevel> new_bids(N);
    std::vector<PriceLevel> new_asks(N);
    std::vector<uint64_t> new_bid_bitmap((N + 63) / 64);
    std::vector<uint64_t> new_ask_bitmap((N + 63) / 64);

    // Move bids
    for (size_t idx = 0; idx < N; ++idx) {
        PriceLevel& lvl = bids_[idx];
        if (lvl.empty()) continue;
        Price abs_price = static_cast<Price>(base_ + idx);
        long new_rel = static_cast<long>(abs_price) - static_cast<long>(new_base);
        if (new_rel < 0 || new_rel >= static_cast<long>(N)) {
            // out of new band: drop all orders (shouldn't happen for small rebases)
            uint32_t j = lvl.head_idx;
            while (j != NULL_IDX) {
                uint32_t next = pool_[j].next_idx; // capture before deallocate
                id_map_.erase(pool_[j].id);
                pool_.deallocate(j);
                j = next;
                if (live_order_count_ > 0) --live_order_count_;
            }
            continue;
        }
        uint32_t nr = static_cast<uint32_t>(new_rel);
        // move linked list nodes into new_bids[nr]
        PriceLevel& dst = new_bids[nr];
        dst.price = abs_price;
        uint32_t j = lvl.head_idx;
        while (j != NULL_IDX) {
            pool_[j].price = nr; // update stored relative index
            uint32_t next = pool_[j].next_idx;
            // append to dst
            if (dst.head_idx == NULL_IDX) {
                dst.head_idx = j;
                pool_[j].prev_idx = NULL_IDX;
            } else {
                pool_[dst.tail_idx].next_idx = j;
                pool_[j].prev_idx = dst.tail_idx;
            }
            dst.tail_idx = j;
            pool_[j].next_idx = NULL_IDX;
            dst.total_volume += pool_[j].qty;
            ++dst.order_count;
            j = next;
        }
        new_bid_bitmap[nr >> 6] |= (1ull << (nr & 63));
    }

    // Move asks (same as bids)
    for (size_t idx = 0; idx < N; ++idx) {
        PriceLevel& lvl = asks_[idx];
        if (lvl.empty()) continue;
        Price abs_price = static_cast<Price>(base_ + idx);
        long new_rel = static_cast<long>(abs_price) - static_cast<long>(new_base);
        if (new_rel < 0 || new_rel >= static_cast<long>(N)) {
            uint32_t j = lvl.head_idx;
            while (j != NULL_IDX) {
                uint32_t next = pool_[j].next_idx;
                id_map_.erase(pool_[j].id);
                pool_.deallocate(j);
                j = next;
                if (live_order_count_ > 0) --live_order_count_;
            }
            continue;
        }
        uint32_t nr = static_cast<uint32_t>(new_rel);
        PriceLevel& dst = new_asks[nr];
        dst.price = abs_price;
        uint32_t j = lvl.head_idx;
        while (j != NULL_IDX) {
            pool_[j].price = nr;
            uint32_t next = pool_[j].next_idx;
            if (dst.head_idx == NULL_IDX) {
                dst.head_idx = j;
                pool_[j].prev_idx = NULL_IDX;
            } else {
                pool_[dst.tail_idx].next_idx = j;
                pool_[j].prev_idx = dst.tail_idx;
            }
            dst.tail_idx = j;
            pool_[j].next_idx = NULL_IDX;
            dst.total_volume += pool_[j].qty;
            ++dst.order_count;
            j = next;
        }
        new_ask_bitmap[nr >> 6] |= (1ull << (nr & 63));
    }

    bids_.swap(new_bids);
    asks_.swap(new_asks);
    bid_bitmap_.swap(new_bid_bitmap);
    ask_bitmap_.swap(new_ask_bitmap);

    base_ = new_base;

    // Recompute top-of-book
    update_best_bid();
    update_best_ask();
}

void OrderBook::cancel_order(OrderId id) noexcept {
    uint32_t idx = id_map_.find(id);
    if (idx == NULL_IDX) return;

    Order& o      = pool_[idx];
    Side   side   = o.side;
    Price  price  = o.price;
    PriceLevel& level = (side == Side::BUY) ? bids_[price] : asks_[price];

    level_remove(level, idx);
    id_map_.erase(id);
    pool_.deallocate(idx);

    if (side == Side::BUY) {
        if (price == tob_.best_bid) {
            if (level.empty()) update_best_bid();
            else                tob_.bid_qty = level.total_volume;
        }
    } else {
        if (price == tob_.best_ask) {
            if (level.empty()) update_best_ask();
            else                tob_.ask_qty = level.total_volume;
        }
    }
}

void OrderBook::process_market_order(Side side, Quantity qty) noexcept {
    if (side == Side::BUY) {
        // No price limit — sweep every ask level up to the top of the band.
        match_against_asks(0, static_cast<Price>(asks_.size() - 1), qty);
    } else {
        // No price limit — sweep every bid level down to price 0.
        match_against_bids(0, 0, qty);
    }
}

// ─── external partial-fill path ──────────────────────────────────────────

bool OrderBook::execute_order(OrderId id, Quantity fill_qty) noexcept {
    uint32_t idx = id_map_.find(id);
    if (idx == NULL_IDX) return false;

    Order& o     = pool_[idx];
    Side   side  = o.side;
    uint32_t rel_price = o.price; // relative index into bids_/asks_
    PriceLevel& level = (side == Side::BUY) ? bids_[rel_price] : asks_[rel_price];
    Price abs_price = level.price; // absolute price for logging

    if (fill_qty >= o.qty) {
        logger_.enqueue_trade(Trade{o.id, 0, abs_price, o.qty, rdtsc()});
        level_remove(level, idx);
        id_map_.erase(id);
        pool_.deallocate(idx);
        if (live_order_count_ > 0) --live_order_count_;

        if (side == Side::BUY) {
            if (rel_price == tob_.best_bid) {
                if (level.empty()) update_best_bid();
                else                tob_.bid_qty = level.total_volume;
            }
        } else {
            if (rel_price == tob_.best_ask) {
                if (level.empty()) update_best_ask();
                else                tob_.ask_qty = level.total_volume;
            }
        }
        return true;
    }

    logger_.enqueue_trade(Trade{o.id, 0, abs_price, fill_qty, rdtsc()});
    o.qty              -= fill_qty;
    level.total_volume -= fill_qty;

    if (side == Side::BUY && rel_price == tob_.best_bid) {
        tob_.bid_qty = level.total_volume;
    } else if (side == Side::SELL && rel_price == tob_.best_ask) {
        tob_.ask_qty = level.total_volume;
    }
    return false;
}

bool OrderBook::reduce_order_qty(OrderId id, Quantity cancel_qty) noexcept {
    uint32_t idx = id_map_.find(id);
    if (idx == NULL_IDX) return false;

    Order& o = pool_[idx];
    PriceLevel& level = (o.side == Side::BUY) ? bids_[o.price] : asks_[o.price];

    if (cancel_qty >= o.qty) {
        // If canceling the whole amount or more, treat as full deletion
        cancel_order(id);
        return true;
    }

    o.qty -= cancel_qty;
    level.total_volume -= cancel_qty;
    
    // Maintain TopOfBook metrics
    if (o.side == Side::BUY && o.price == tob_.best_bid) {
        tob_.bid_qty = level.total_volume;
    } else if (o.side == Side::SELL && o.price == tob_.best_ask) {
        tob_.ask_qty = level.total_volume;
    }
    return false;
}

} // namespace Nanomatch