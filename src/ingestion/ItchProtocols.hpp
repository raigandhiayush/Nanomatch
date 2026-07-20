#pragma once
#include <cstdint>

namespace Nanomatch {

#pragma pack(push, 1)

// ITCH 5.0's wire "Timestamp" field is 6 bytes (48-bit nanoseconds since
// midnight), NOT 8. Using uint64_t here (as this struct set used to) makes
// every packed message struct 2 bytes wider than the real wire format, so
// every field declared after `timestamp` -- order_id, side, shares, price,
// everything -- gets read from 2 bytes off from where it actually is in a
// real feed. Reads still "succeed" (no crash, no bounds violation), they
// just silently return garbage: wrong order ids, wrong sides, wrong
// prices/quantities. That's what produced the zero-execution symptom --
// resting orders and incoming orders were being matched against corrupted
// price/qty fields, so crossing orders that should have executed just
// never lined up.
struct ItchTimestamp {
    uint8_t bytes[6];
};

inline uint64_t itch_ts_to_u64(const ItchTimestamp& t) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | t.bytes[i];
    return v;
}

struct ItchCommonHeader {
    uint16_t packet_length;
    char message_type;
};

struct ItchAddOrderMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    ItchTimestamp timestamp;
    uint64_t order_id;
    char side;
    uint32_t shares;
    char stock[8];
    uint32_t price;
};

struct ItchStockDirectoryMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    ItchTimestamp timestamp;
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
    ItchTimestamp timestamp;
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
    ItchTimestamp timestamp;
    uint64_t order_id;
    uint32_t shares;
    uint64_t match_number;
};

// Order Executed With Price ('C'): same prefix as plain Executed ('E'),
// plus a printable flag and an explicit execution price. Previously
// unhandled by the parser (fell into `default: break`), so any fill that
// arrived as a 'C' message instead of 'E' was silently dropped -- the
// order stayed marked live/resting even though the exchange had already
// executed it, which is another route to the zero-execution / stale-book
// symptom.
struct ItchOrderExecutedWithPriceMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    ItchTimestamp timestamp;
    uint64_t order_id;
    uint32_t shares;
    uint64_t match_number;
    char     printable;
    uint32_t execution_price;
};

struct ItchOrderCancelMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    ItchTimestamp timestamp;
    uint64_t order_id;
    uint32_t canceled_shares;
};

struct ItchOrderDeleteMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    ItchTimestamp timestamp;
    uint64_t order_id;
};

struct ItchOrderReplaceMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    ItchTimestamp timestamp;
    uint64_t original_order_id;
    uint64_t new_order_id;
    uint32_t shares;
    uint32_t price;
};

#pragma pack(pop)

inline uint64_t bswap64(uint64_t val) { return __builtin_bswap64(val); }
inline uint32_t bswap32(uint32_t val) { return __builtin_bswap32(val); }
}