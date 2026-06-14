#pragma once
#include "OrderBook.hpp"
#include "MmapParser.hpp"
#include "AsyncLogger.hpp"

namespace Nanomatch {
    class Engine {
    public:
        Engine(const std::string& input_file, const std::string& log_file, size_t max_orders)
            : logger_(log_file), book_(max_orders, logger_), parser_(input_file) {}

        void run() {
            logger_.start();

            const OrderRecord* records = parser_.get_records();
            size_t total_records = parser_.count();

            for (size_t i = 0; i < total_records; ++i) {
                const auto& rec = records[i];
                Side side = (rec.side == 0) ? Side::BUY : Side::SELL;

                switch (rec.type) {
                    case 'L':
                        book_.insert_limit_order(rec.order_id, side, rec.price, rec.qty);
                        break;
                    case 'M':
                        book_.process_market_order(side, rec.qty);
                        break;
                    case 'C':
                        book_.cancel_order(rec.order_id);
                        break;
                }
            }

            logger_.stop();
        }

    private:
        AsyncLogger logger_;
        OrderBook book_;
        MmapParser parser_;
    };
}