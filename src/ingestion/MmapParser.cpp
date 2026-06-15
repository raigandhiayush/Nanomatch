#include "ItchProtocols.hpp"
#include "MmapParser.hpp"
#include "../core/OrderBook.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <vector>

extern std::vector<uint64_t> latency_samples;

namespace Nanomatch {

    MmapParser::MmapParser(const std::string& path) { 
        fd_ = open(path.c_str(), O_RDONLY); 
        if (fd_ < 0) 
            throw std::runtime_error("MmapParser: failed to open '" + path + "'"); 

        struct stat st{};
        if (fstat(fd_, &st) != 0) {
            close(fd_);
            throw std::runtime_error("MmapParser: fstat failed for '" + path + "'");
        }
        size_ = static_cast<size_t>(st.st_size);

        if (size_ == 0 || size_ % sizeof(OrderRecord) != 0) {
            close(fd_);
            throw std::runtime_error(
                "MmapParser: '" + path + "' size (" + std::to_string(size_) +
                " bytes) is not a non-zero multiple of record size (" +
                std::to_string(sizeof(OrderRecord)) + ")");
        }

        void* mapped = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd_, 0); 
        if (mapped == MAP_FAILED) { 
            close(fd_); 
            throw std::runtime_error("MmapParser: mmap failed for '" + path + "'"); 
        } 
        
        data_  = static_cast<const OrderRecord*>(mapped); 
        count_ = size_ / sizeof(OrderRecord); 
    } 
    
    MmapParser::~MmapParser() { 
        if (data_) munmap(const_cast<OrderRecord*>(data_), size_); 
        if (fd_ >= 0) close(fd_); 
    }

    void parse_itch_file(const std::string& filepath, Nanomatch::OrderBook& book) {
        int fd = open(filepath.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("ITCH Parser Error: Could not open file at path: " + filepath);
        }

        off_t file_size = lseek(fd, 0, SEEK_END);
        if (file_size <= 0) {
            close(fd);
            throw std::runtime_error("ITCH Parser Error: File is empty or path invalid.");
        }
        
        void* mmap_ptr = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mmap_ptr == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("ITCH Parser Error: mmap failed.");
        }

        uint8_t* cursor = reinterpret_cast<uint8_t*>(mmap_ptr);
        uint8_t* end_ptr = cursor + file_size;

        while (cursor < end_ptr) {
            auto* header = reinterpret_cast<ItchCommonHeader*>(cursor);
            uint8_t* payload = cursor + sizeof(ItchCommonHeader);
            
            switch (header->message_type) {
                case 'A': { // Add Order Unattributed
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchAddOrderMessage*>(payload);
                    book.insert_limit_order(bswap64(msg->order_id), (msg->side == 'S') ? Side::SELL : Side::BUY, bswap32(msg->price), bswap32(msg->shares));
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'F': { // Add Order Attributed
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchAddOrderAttributedMessage*>(payload);
                    book.insert_limit_order(bswap64(msg->order_id), (msg->side == 'S') ? Side::SELL : Side::BUY, bswap32(msg->price), bswap32(msg->shares));
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'E': { // Order Executed
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchOrderExecutedMessage*>(payload);
                    book.execute_order(bswap64(msg->order_id), bswap32(msg->shares));
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'X': { // Order Cancel
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchOrderCancelMessage*>(payload);
                    book.reduce_order_qty(bswap64(msg->order_id), bswap32(msg->canceled_shares));
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'D': { // Order Delete
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchOrderDeleteMessage*>(payload);
                    book.cancel_order(bswap64(msg->order_id));
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'U': { // Order Replace (Atomic Modification)
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchOrderReplaceMessage*>(payload);
                    
                    // 1. Locate and extract original order context details
                    uint64_t old_id = bswap64(msg->original_order_id);
                    uint32_t old_idx = book.get_id_map().find(old_id); 
                    
                    if (old_idx != NULL_IDX) {
                        Side original_side = book.get_pool()[old_idx].side;
                        
                        // 2. Erase old reference
                        book.cancel_order(old_id);
                        
                        // 3. Inject new order with updated parameters
                        book.insert_limit_order(bswap64(msg->new_order_id), original_side, bswap32(msg->price), bswap32(msg->shares));
                    }
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                default: // Skip non-book altering message contexts (S, R, H, P, Q)
                    break;
            }
            
            cursor += sizeof(uint16_t) + __builtin_bswap16(header->packet_length);
        }

        munmap(mmap_ptr, file_size);
        close(fd);
    }
}