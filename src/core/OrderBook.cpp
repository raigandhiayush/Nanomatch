#include "OrderBook.hpp"
#include <algorithm>

namespace Nanomatch {

OrderBook::OrderBook(size_t max_orders, AsyncLogger& logger) 
    : arena_(max_orders), logger_(logger) {
    size_t expected_price_range = 10000;
    bids_.resize(expected_price_range);
    asks_.resize(expected_price_range);

    for (size_t i = 0; i < expected_price_range; ++i) {
        bids_[i].price = i;
        asks_[i].price = i;
    }
}

void OrderBook::execute_order(OrderId id, Quantity fill_qty) { 
    auto it = order_lookup_.find(id); 
    if (it == order_lookup_.end()) return; 
    Order* o = it->second; 
    PriceLevel& level = get_price_level(o->side, o->price); 
    if (fill_qty >= o->qty) { 
        remove_from_level(level, o); 
        order_lookup_.erase(it); 
        arena_.deallocate(o); 
    } 
    else { 
        o->qty -= fill_qty; 
        level.total_volume -= fill_qty; 
    } 
}

void OrderBook::append_to_level(PriceLevel& level, Order* order) { // O(1)
    order->next = nullptr;
    order->prev = level.tail;

    if (level.head == nullptr) level.head = order;
    else level.tail->next = order;

    level.tail = order;
    level.total_volume += order->qty;
}

void OrderBook::remove_from_level(PriceLevel& level, Order* order) { // O(1)
    if (order->prev == nullptr) {
        level.head = order->next;
    } else {
        order->prev->next = order->next;
    }

    if (order->next) {
        order->next->prev = order->prev;
    } else {
        level.tail = order->prev;
    }

    level.total_volume -= order->qty;
    order->next = nullptr;
    order->prev = nullptr;
}

void OrderBook::insert_limit_order(OrderId id, Side side, Price price, Quantity qty) {
    Quantity remaining_qty = qty;

    if (side == Side::BUY) {
        for (auto& level : asks_) {
            if (level.price > price || remaining_qty == 0) {
                break;
            }
            if(level.empty()) continue;

            Order* current_ask = level.head;
            while (current_ask != nullptr && remaining_qty > 0) {
                Order* next_ask = current_ask->next;
                Quantity match_qty = std::min(remaining_qty, current_ask->qty);
                
                logger_.enqueue_trade(Trade{
                    current_ask->id,
                    id,
                    level.price,
                    match_qty,
                    rdtsc()
                });

                remaining_qty -= match_qty;
                current_ask->qty -= match_qty;
                level.total_volume -= match_qty;

                if (current_ask->qty == 0) {
                    remove_from_level(level, current_ask);
                    order_lookup_.erase(current_ask->id);
                    arena_.deallocate(current_ask);
                }

                current_ask = next_ask;
            }
        }

        if (remaining_qty > 0) {
            Order* new_order = arena_.allocate();
            if (new_order) {
                new_order->id = id;
                new_order->price = price;
                new_order->qty = remaining_qty;
                new_order->side = side;

                PriceLevel& level = get_price_level(Side::BUY, price);
                append_to_level(level, new_order);

                order_lookup_[id] = new_order;
            }
        }

    } else {
        for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
            auto& level = *it;
            if (level.empty() || level.price < price || remaining_qty == 0) {
                break;
            }

            Order* current_bid = level.head;
            while (current_bid != nullptr && remaining_qty > 0) {
                Order* next_bid = current_bid->next;
                Quantity match_qty = std::min(remaining_qty, current_bid->qty);

                logger_.enqueue_trade(Trade{
                    current_bid->id,
                    id,
                    level.price,
                    match_qty,
                    rdtsc()
                });

                remaining_qty -= match_qty;
                current_bid->qty -= match_qty;
                level.total_volume -= match_qty;

                if (current_bid->qty == 0) {
                    remove_from_level(level, current_bid);
                    order_lookup_.erase(current_bid->id);
                    arena_.deallocate(current_bid);
                }

                current_bid = next_bid;
            }
        }

        if (remaining_qty > 0) {
            Order* new_order = arena_.allocate();
            if (new_order) {
                new_order->id = id;
                new_order->price = price;
                new_order->qty = remaining_qty;
                new_order->side = side;

                PriceLevel& level = get_price_level(Side::SELL, price);
                append_to_level(level, new_order);

                order_lookup_[id] = new_order;
            }
        }
    }
}

void OrderBook::cancel_order(OrderId id) {
    auto it = order_lookup_.find(id);
    if (__builtin_expect((it == order_lookup_.end()), 0)) {
        return;
    }

    Order* order_to_cancel = it->second;
    PriceLevel& level = get_price_level(order_to_cancel->side, order_to_cancel->price);

    remove_from_level(level, order_to_cancel);

    order_lookup_.erase(it);
    arena_.deallocate(order_to_cancel);
}

void OrderBook::process_market_order(Side side, Quantity qty) {
    Quantity remaining_qty = qty;

    if (side == Side::BUY) {
        for (auto& level : asks_) {
            if (remaining_qty == 0) break;
            if (level.empty()) continue;

            Order* current_ask = level.head;
            while (current_ask != nullptr && remaining_qty > 0) {
                Order* next_ask = current_ask->next;
                Quantity match_qty = std::min(remaining_qty, current_ask->qty);

                logger_.enqueue_trade(Trade{
                    current_ask->id,
                    0, 
                    level.price,
                    match_qty,
                    rdtsc()
                });

                remaining_qty -= match_qty;
                current_ask->qty -= match_qty;
                level.total_volume -= match_qty;

                if (current_ask->qty == 0) {
                    remove_from_level(level, current_ask);
                    order_lookup_.erase(current_ask->id);
                    arena_.deallocate(current_ask);
                }

                current_ask = next_ask;
            }
        }

    } else {
        for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
            auto& level = *it;
            if (remaining_qty == 0) break;
            if (level.empty()) continue;

            Order* current_bid = level.head;
            while (current_bid != nullptr && remaining_qty > 0) {
                Order* next_bid = current_bid->next;
                Quantity match_qty = std::min(remaining_qty, current_bid->qty);

                logger_.enqueue_trade(Trade{
                    current_bid->id,
                    0,
                    level.price,
                    match_qty,
                    rdtsc()
                });

                remaining_qty -= match_qty;
                current_bid->qty -= match_qty;
                level.total_volume -= match_qty;

                if (current_bid->qty == 0) {
                    remove_from_level(level, current_bid);
                    order_lookup_.erase(current_bid->id);
                    arena_.deallocate(current_bid);
                }

                current_bid = next_bid;
            }
        }
    }
}

}