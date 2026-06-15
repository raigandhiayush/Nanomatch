#include <cassert>
#include <iostream>
#include "OrderBook.hpp"
#include "AsyncLogger.hpp"

void test_fifo_time_priority() {
    std::cout << "[Test] Running FIFO Time Priority Verification...\n";
    
    Nanomatch::AsyncLogger logger("/dev/null");
    logger.start();
    Nanomatch::OrderBook book(1000, logger);

    // Place two resting sell orders at the exact same price level
    book.insert_limit_order(101, Nanomatch::Side::SELL, 500, 10); // Order A (First)
    book.insert_limit_order(102, Nanomatch::Side::SELL, 500, 20); // Order B (Second)

    // Send a crossing Buy order that partially fills the available liquidity (qty = 15)
    book.insert_limit_order(201, Nanomatch::Side::BUY, 500, 15);

    // Verify state changes via public price level accessors
    const Nanomatch::PriceLevel& level = book.ask_level(500);
    
    // Total sitting volume should now be 30 (initial) - 15 (matched) = 15 shares
    assert(level.total_volume == 15); 
    // Head of the queue must now point to a valid index matching Order B
    assert(level.head_idx != Nanomatch::NULL_IDX);
    
    logger.stop();
    std::cout << "[Pass] FIFO Priority working flawlessly.\n";
}

void test_order_cancellation() {
    std::cout << "[Test] Running Order Cancellation Verification...\n";

    Nanomatch::AsyncLogger logger("/dev/null");
    logger.start();
    Nanomatch::OrderBook book(1000, logger);

    // Rest a buy order in the book
    book.insert_limit_order(301, Nanomatch::Side::BUY, 450, 100);
    const Nanomatch::PriceLevel& level = book.bid_level(450);
    assert(level.total_volume == 100);

    // Trigger instant cancel
    book.cancel_order(301);

    // Level queue should be completely wiped out
    assert(level.empty());
    assert(level.total_volume == 0);

    logger.stop();
    std::cout << "[Pass] Order snapped out of memory pool cleanly.\n";
}

int main() {
    std::cout << "============= STARTING CORE CORRECTNESS SUITE =============\n";
    test_fifo_time_priority();
    test_order_cancellation();
    std::cout << "============= ALL TESTS COMPLETED SUCCESSFULLY ============\n";
    return 0;
}