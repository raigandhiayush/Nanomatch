// src/ingestion/ItchParser.cpp
#include "ItchProtocols.hpp"
#include "../core/Types.hpp"
#include "../core/OrderBook.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <vector>
#include <thread>
#include <memory>
#include <unordered_map>

extern std::vector<uint64_t> latency_samples;

namespace Nanomatch {
    static constexpr size_t kMaxStockLocate = 8192;

    void parse_itch_file(const std::string& filepath, AsyncLogger& logger) {
        int fd = open(filepath.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("Could not open file: " + filepath);

        off_t file_size = lseek(fd, 0, SEEK_END);
        if (file_size < 0) { close(fd); throw std::runtime_error("lseek failed: " + filepath); }

        void* mmap_ptr = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mmap_ptr == MAP_FAILED) { close(fd); throw std::runtime_error("mmap failed: " + filepath); }

        std::vector<std::unique_ptr<OrderBook>> market_books(kMaxStockLocate);
        std::unordered_map<OrderId, OrderBook*> ref_owner;
        ref_owner.reserve(1u << 20);

        uint8_t* cursor  = reinterpret_cast<uint8_t*>(mmap_ptr);
        uint8_t* end_ptr = cursor + file_size;
        uint64_t message_count = 0;

        while (cursor < end_ptr) {
            auto* header = reinterpret_cast<ItchCommonHeader*>(cursor);
            uint8_t* payload = cursor + sizeof(ItchCommonHeader);
            uint16_t packet_length = __builtin_bswap16(header->packet_length);
            uint16_t locate_id = 0;

            switch (header->message_type) {
                case 'A': {
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchAddOrderMessage*>(payload);
                    locate_id = __builtin_bswap16(msg->stock_locate);
                    if (locate_id < kMaxStockLocate) {
                        auto& book = market_books[locate_id];
                        if (!book) book = std::make_unique<OrderBook>(PRICE_BAND, logger);
                        if (book->insert_limit_order(bswap64(msg->order_id), (msg->side == 'S') ? Side::SELL : Side::BUY, bswap32(msg->price), bswap32(msg->shares))) {
                            ref_owner[bswap64(msg->order_id)] = book.get();
                        }
                    }
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'F': {
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchAddOrderAttributedMessage*>(payload);
                    locate_id = __builtin_bswap16(msg->stock_locate);
                    if (locate_id < kMaxStockLocate) {
                        auto& book = market_books[locate_id];
                        if (!book) book = std::make_unique<OrderBook>(PRICE_BAND, logger);
                        if (book->insert_limit_order(bswap64(msg->order_id), (msg->side == 'S') ? Side::SELL : Side::BUY, bswap32(msg->price), bswap32(msg->shares))) {
                            ref_owner[bswap64(msg->order_id)] = book.get();
                        }
                    }
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'E': {
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchOrderExecutedMessage*>(payload);
                    OrderId id = bswap64(msg->order_id);
                    auto it = ref_owner.find(id);
                    if (it != ref_owner.end()) {
                        if (it->second->execute_order(id, bswap32(msg->shares))) {
                            ref_owner.erase(it);
                        }
                    }
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'X': {
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchOrderCancelMessage*>(payload);
                    OrderId id = bswap64(msg->order_id);
                    auto it = ref_owner.find(id);
                    if (it != ref_owner.end()) {
                        if (it->second->reduce_order_qty(id, bswap32(msg->canceled_shares))) {
                            ref_owner.erase(it);
                        }
                    }
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'D': {
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchOrderDeleteMessage*>(payload);
                    auto it = ref_owner.find(bswap64(msg->order_id));
                    if (it != ref_owner.end()) {
                        it->second->cancel_order(bswap64(msg->order_id));
                        ref_owner.erase(it);
                    }
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'U': {
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchOrderReplaceMessage*>(payload);
                    OrderId old_id = bswap64(msg->original_order_id);
                    auto it = ref_owner.find(old_id);
                    if (it != ref_owner.end()) {
                        OrderBook* book = it->second;
                        uint32_t old_idx = book->get_id_map().find(old_id);
                        if (old_idx != NULL_IDX) {
                            Side original_side = book->get_pool()[old_idx].side;
                            book->cancel_order(old_id);
                            if (book->insert_limit_order(bswap64(msg->new_order_id), original_side, bswap32(msg->price), bswap32(msg->shares))) {
                                ref_owner.erase(it);
                                ref_owner[bswap64(msg->new_order_id)] = book;
                            } else {
                                ref_owner.erase(it);
                            }
                        }
                    }
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                default:
                    break;
            }

            cursor += sizeof(uint16_t) + packet_length;
            if (__builtin_expect(++message_count % 500000 == 0, 0)) {
                std::this_thread::yield();
            }
        }

        munmap(mmap_ptr, file_size);
        close(fd);
    }
}
