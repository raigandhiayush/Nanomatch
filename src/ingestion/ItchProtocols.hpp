#pragma once
#include <cstdint>

namespace Nanomatch {

// Force the compiler to pack structs tightly with zero padding alignment bytes
#pragma pack(push, 1)

struct ItchCommonHeader {
    uint16_t packet_length; // Length of the payload message
    char message_type;      // 'A' = Add, 'E' = Execute, 'X' = Cancel, 'D' = Delete
};

struct ItchAddOrderMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp; // Nanoseconds from midnight
    uint64_t order_id;
    char side;          // 'B' = Buy, 'S' = Sell
    uint32_t shares;
    char stock[8];      // Stock ticker padding spaces
    uint32_t price;     // Price integer (scaled by 10,000)
};

struct ItchOrderExecutedMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_id;
    uint32_t shares;
    uint64_t match_number;
};

#pragma pack(pop)

// Fast utility function to fix Big-Endian integers coming over the network stream
inline uint64_t bswap64(uint64_t val) { return __builtin_bswap64(val); }
inline uint32_t bswap32(uint32_t val) { return __builtin_bswap32(val); }
}