#pragma once
// ─────────────────────────────────────────────────────────────────────────
// Naive STL baseline: std::map<Price, std::list<Order>> per side, with
// std::unordered_map<OrderId, iterator> for O(1)-ish cancel lookup.
// This is the "textbook" implementation the optimized OrderBook (flat
// index-based pool + intrusive bitmap price levels) is benchmarked against.
// Deliberately idiomatic / pointer-chasing so the throughput and cache-miss
// deltas in the report are real, not a strawman.
// ─────────────────────────────────────────────────────────────────────────
#include <map>
#include <list>
#include <unordered_map>
#include "Types.hpp"

namespace Nanomatch::Baseline {

struct RestingOrder {
    OrderId  id;
    Quantity qty;
};

class StlOrderBook {
public:
    // Returns true if the order rests (did not fully cross), matching the
    // same observable contract used by loops calling OrderBook::insert_limit_order.
    bool insert_limit_order(OrderId id, Side side, Price price, Quantity qty) {
        if (side == Side::BUY) {
            while (qty > 0 && !asks_.empty() && asks_.begin()->first <= price) {
                auto& level = asks_.begin()->second;
                auto& resting = level.front();
                Quantity fill = std::min(qty, resting.qty);
                qty -= fill;
                resting.qty -= fill;
                if (resting.qty == 0) {
                    id_index_.erase(resting.id);
                    level.pop_front();
                    if (level.empty()) asks_.erase(asks_.begin());
                }
            }
            if (qty > 0) {
                auto& level = bids_[price];
                level.push_back({id, qty});
                id_index_[id] = { side, price, std::prev(level.end()) };
                return true;
            }
        } else {
            while (qty > 0 && !bids_.empty() && bids_.rbegin()->first >= price) {
                auto& level = bids_.rbegin()->second;
                auto& resting = level.front();
                Quantity fill = std::min(qty, resting.qty);
                qty -= fill;
                resting.qty -= fill;
                if (resting.qty == 0) {
                    id_index_.erase(resting.id);
                    level.pop_front();
                    if (level.empty()) bids_.erase(std::prev(bids_.end()));
                }
            }
            if (qty > 0) {
                auto& level = asks_[price];
                level.push_back({id, qty});
                id_index_[id] = { side, price, std::prev(level.end()) };
                return true;
            }
        }
        return false;
    }

    bool cancel_order(OrderId id) {
        auto it = id_index_.find(id);
        if (it == id_index_.end()) return false;
        auto [side, price, list_it] = it->second;
        auto& book = (side == Side::BUY) ? bids_ : asks_;
        auto level_it = book.find(price);
        level_it->second.erase(list_it);
        if (level_it->second.empty()) book.erase(level_it);
        id_index_.erase(it);
        return true;
    }

    void process_market_order(Side side, Quantity qty) {
        if (side == Side::BUY) {
            while (qty > 0 && !asks_.empty()) {
                auto& level = asks_.begin()->second;
                auto& resting = level.front();
                Quantity fill = std::min(qty, resting.qty);
                qty -= fill;
                resting.qty -= fill;
                if (resting.qty == 0) {
                    id_index_.erase(resting.id);
                    level.pop_front();
                    if (level.empty()) asks_.erase(asks_.begin());
                }
            }
        } else {
            while (qty > 0 && !bids_.empty()) {
                auto& level = bids_.rbegin()->second;
                auto& resting = level.front();
                Quantity fill = std::min(qty, resting.qty);
                qty -= fill;
                resting.qty -= fill;
                if (resting.qty == 0) {
                    id_index_.erase(resting.id);
                    level.pop_front();
                    if (level.empty()) bids_.erase(std::prev(bids_.end()));
                }
            }
        }
    }

private:
    struct IndexEntry {
        Side side;
        Price price;
        std::list<RestingOrder>::iterator it;
    };
    std::map<Price, std::list<RestingOrder>> bids_;               // descending best-bid = rbegin
    std::map<Price, std::list<RestingOrder>> asks_;               // ascending best-ask  = begin
    std::unordered_map<OrderId, IndexEntry>  id_index_;
};

} // namespace Nanomatch::Baseline