#pragma once
#include <cstdint>

namespace Nanomatch {

#pragma pack(push, 1)

struct ItchCommonHeader {
    uint16_t packet_length;
    char message_type;
};

struct ItchAddOrderMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_id;
    char side;
    uint32_t shares;
    char stock[8];
    uint32_t price;
};

struct ItchStockDirectoryMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    char     stock[8]; // The 8-character alphanumeric ticker pad (e.g., "AAPL    ")
    char     market_category;
    char     financial_status_indicator;
    uint32_t round_lot_size;
    char     round_lots_only;
    char     issue_classification;
    char     issue_subtype[2];
    char     authenticity;
    char     short_sale_threshold_indicator;
    char     ipo_flag;
    char     luld_reference_price_tier;
    char     etp_flag;
    uint32_t etp_leverage_factor;
    char     inverse_indicator;
};

struct ItchAddOrderAttributedMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_id;
    char     side;
    uint32_t shares;
    char     stock[8];
    uint32_t price;
    char     attribution[4];
};

struct ItchOrderExecutedMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_id;
    uint32_t shares;
    uint64_t match_number;
};

struct ItchOrderCancelMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_id;
    uint32_t canceled_shares;
};

struct ItchOrderDeleteMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_id;
};

struct ItchOrderReplaceMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t original_order_id;
    uint64_t new_order_id;
    uint32_t shares;
    uint32_t price;
};

#pragma pack(pop)

inline uint64_t bswap64(uint64_t val) { return __builtin_bswap64(val); }
inline uint32_t bswap32(uint32_t val) { return __builtin_bswap32(val); }
}