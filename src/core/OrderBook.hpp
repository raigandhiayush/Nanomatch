#pragma once
#include "Types.hpp"
#include "MemoryArena.hpp"
#include "AsyncLogger.hpp"
#include <unordered_map>
#include <vector>

namespace Nanomatch {

class OrderBook {
private:
    MemoryArena arena_;
    AsyncLogger& logger_;
    
    std::unordered_map<OrderId, Order*> order_lookup_;
    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
    
    void append_to_level(PriceLevel& level, Order* order);
    void remove_from_level(PriceLevel& level, Order* order);

public:
    inline PriceLevel& get_price_level(Side side, Price price) { 
        auto& book = (side == Side::BUY) ? bids_ : asks_; 
        if (price >= book.size()) book.resize(price + 1);
        return book[price]; 
    }
    explicit OrderBook(size_t max_orders, AsyncLogger& logger);
    
    void insert_limit_order(OrderId id, Side side, Price price, Quantity quantity);
    void cancel_order(OrderId id);
    void process_market_order(Side side, Quantity quantity);
    void execute_order(OrderId id, Quantity fill_qty);
};

}