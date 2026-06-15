#include "OrderBook.hpp"
#include <algorithm>

namespace Nanomatch {

OrderBook::OrderBook(size_t /*max_orders_unused*/, AsyncLogger& logger)
    : logger_(logger), bids_(PRICE_BAND), asks_(PRICE_BAND)
{
    // pool_ and id_map_ use their default capacities (MAX_ORDERS /
    // MAX_ORDERS*2). bids_/asks_ are pre-sized to the full price band and
    // never resized again — the array index *is* the price.
    for (uint32_t p = 0; p < bids_.size(); ++p) {
        bids_[p].price = p;
        asks_[p].price = p;
    }
}

// ─── level linked-list helpers ───────────────────────────────────────────

void OrderBook::level_append(PriceLevel& level, uint32_t idx) noexcept {
    Order& o = pool_[idx];

    o.prev_idx = level.tail_idx;
    o.next_idx = NULL_IDX;

    if (level.head_idx == NULL_IDX) {
        level.head_idx = idx;
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

    o.next_idx = NULL_IDX;
    o.prev_idx = NULL_IDX;
}

// ─── top-of-book maintenance ─────────────────────────────────────────────
//
// Called only when the *current* best level may have just become empty.
// Scans outward from the current best toward the back of the book — in
// the common case the new best is immediately adjacent, so this is O(1)
// amortized rather than a full PRICE_BAND sweep.

void OrderBook::update_best_bid() noexcept {
    Price p = tob_.best_bid;
    while (true) {
        if (!bids_[p].empty()) {
            tob_.best_bid = p;
            tob_.bid_qty  = bids_[p].total_volume;
            return;
        }
        if (p == 0) break;
        --p;
    }
    tob_.best_bid = 0;
    tob_.bid_qty  = 0;
}

void OrderBook::update_best_ask() noexcept {
    Price p = tob_.best_ask;
    if (p >= asks_.size()) p = 0;
    while (p < asks_.size()) {
        if (!asks_[p].empty()) {
            tob_.best_ask = p;
            tob_.ask_qty  = asks_[p].total_volume;
            return;
        }
        ++p;
    }
    tob_.best_ask = UINT32_MAX;
    tob_.ask_qty  = 0;
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

            Quantity match_qty = std::min(remaining, o.qty);
            logger_.enqueue_trade(Trade{o.id, taker_id, level.price, match_qty, rdtsc()});

            remaining        -= match_qty;
            o.qty             -= match_qty;
            level.total_volume -= match_qty;

            if (o.qty == 0) {
                level_remove(level, idx);
                id_map_.erase(o.id);
                pool_.deallocate(idx);
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
        if (best < limit) break;

        PriceLevel& level = bids_[best];
        if (level.empty()) break;               // no bids at all (best == 0, bids_[0] empty)

        uint32_t idx = level.head_idx;
        while (idx != NULL_IDX && remaining > 0) {
            Order& o = pool_[idx];
            uint32_t next = o.next_idx;

            Quantity match_qty = std::min(remaining, o.qty);
            logger_.enqueue_trade(Trade{o.id, taker_id, level.price, match_qty, rdtsc()});

            remaining          -= match_qty;
            o.qty              -= match_qty;
            level.total_volume -= match_qty;

            if (o.qty == 0) {
                level_remove(level, idx);
                id_map_.erase(o.id);
                pool_.deallocate(idx);
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

void OrderBook::insert_limit_order(OrderId id, Side side, Price price, Quantity qty) noexcept {
    if (price >= bids_.size()) return;   // outside the pre-allocated price band

    Quantity remaining;

    if (side == Side::BUY) {
        remaining = match_against_asks(id, price, qty);
        if (remaining == 0) return;

        uint32_t idx = pool_.allocate();
        if (idx == NULL_IDX) return;     // pool exhausted — drop the order

        Order& o = pool_[idx];
        o.id    = id;
        o.price = price;
        o.qty   = remaining;
        o.side  = side;

        PriceLevel& level = bids_[price];
        level_append(level, idx);
        id_map_.insert(id, idx);

        if (price >= tob_.best_bid) {
            tob_.best_bid = price;
            tob_.bid_qty  = level.total_volume;
        }
    } else {
        remaining = match_against_bids(id, price, qty);
        if (remaining == 0) return;

        uint32_t idx = pool_.allocate();
        if (idx == NULL_IDX) return;

        Order& o = pool_[idx];
        o.id    = id;
        o.price = price;
        o.qty   = remaining;
        o.side  = side;

        PriceLevel& level = asks_[price];
        level_append(level, idx);
        id_map_.insert(id, idx);

        if (price <= tob_.best_ask) {
            tob_.best_ask = price;
            tob_.ask_qty  = level.total_volume;
        }
    }
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

void OrderBook::execute_order(OrderId id, Quantity fill_qty) noexcept {
    uint32_t idx = id_map_.find(id);
    if (idx == NULL_IDX) return;

    Order& o     = pool_[idx];
    Side   side  = o.side;
    Price  price = o.price;
    PriceLevel& level = (side == Side::BUY) ? bids_[price] : asks_[price];

    if (fill_qty >= o.qty) {
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
    } else {
        o.qty              -= fill_qty;
        level.total_volume -= fill_qty;

        if (side == Side::BUY && price == tob_.best_bid) {
            tob_.bid_qty = level.total_volume;
        } else if (side == Side::SELL && price == tob_.best_ask) {
            tob_.ask_qty = level.total_volume;
        }
    }
}

} // namespace Nanomatch