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

extern std::vector<uint64_t> latency_samples;

namespace Nanomatch {
    void parse_itch_file(const std::string& filepath, OrderBook& book) {
        int fd = open(filepath.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("Could not open file: " + filepath);

        off_t file_size = lseek(fd, 0, SEEK_END);
        if (file_size < 0) { close(fd); throw std::runtime_error("lseek failed: " + filepath); }

        void* mmap_ptr = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mmap_ptr == MAP_FAILED) { close(fd); throw std::runtime_error("mmap failed: " + filepath); }

        uint8_t* cursor  = reinterpret_cast<uint8_t*>(mmap_ptr);
        uint8_t* end_ptr = cursor + file_size;
        uint64_t message_count = 0;   // moved out of the loop — was reset every iteration

        while (cursor < end_ptr) {
            auto* header = reinterpret_cast<ItchCommonHeader*>(cursor);
            uint8_t* payload = cursor + sizeof(ItchCommonHeader);

            switch (header->message_type) {
                case 'A': {
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchAddOrderMessage*>(payload);
                    book.insert_limit_order(bswap64(msg->order_id), (msg->side == 'S') ? Side::SELL : Side::BUY, bswap32(msg->price), bswap32(msg->shares));
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'F': {
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchAddOrderAttributedMessage*>(payload);
                    book.insert_limit_order(bswap64(msg->order_id), (msg->side == 'S') ? Side::SELL : Side::BUY, bswap32(msg->price), bswap32(msg->shares));
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'E': {
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchOrderExecutedMessage*>(payload);
                    book.execute_order(bswap64(msg->order_id), bswap32(msg->shares));
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'X': {
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchOrderCancelMessage*>(payload);
                    book.reduce_order_qty(bswap64(msg->order_id), bswap32(msg->canceled_shares));
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'D': {
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchOrderDeleteMessage*>(payload);
                    book.cancel_order(bswap64(msg->order_id));
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'U': {
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchOrderReplaceMessage*>(payload);
                    uint64_t old_id = bswap64(msg->original_order_id);
                    uint32_t old_idx = book.get_id_map().find(old_id);
                    if (old_idx != NULL_IDX) {
                        Side original_side = book.get_pool()[old_idx].side;
                        book.cancel_order(old_id);
                        book.insert_limit_order(bswap64(msg->new_order_id), original_side, bswap32(msg->price), bswap32(msg->shares));
                    }
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                default:
                    break;
            }

            cursor += sizeof(uint16_t) + __builtin_bswap16(header->packet_length);
            if (__builtin_expect(++message_count % 500000 == 0, 0)) {
                std::this_thread::yield();
            }
        }

        munmap(mmap_ptr, file_size);
        close(fd);
    }
}