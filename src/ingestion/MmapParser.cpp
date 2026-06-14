#include "ItchProtocols.hpp"
#include "MmapParser.hpp"
#include "../core/OrderBook.hpp"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace Nanomatch{
    MmapParser::MmapParser(const std::string& fp) { 
        fd_ = open(fp.c_str(), O_RDONLY); 
        if (fd_ < 0) throw std::runtime_error("open failed"); 
        file_size_ = lseek(fd_, 0, SEEK_END); 
        mmap_ptr_ = mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0); 
        if (mmap_ptr_ == MAP_FAILED) { 
            close(fd_); 
            throw std::runtime_error("mmap failed"); 
        } 
        records_ = reinterpret_cast<const OrderRecord*>(mmap_ptr_); 
        record_count_ = file_size_ / sizeof(OrderRecord); 
    } 
    
    MmapParser::~MmapParser() { 
        if (mmap_ptr_ && mmap_ptr_ != MAP_FAILED) munmap(mmap_ptr_, file_size_); 
        if (fd_ >= 0) close(fd_); 
    }

    void parse_itch_file(const std::string& filepath, Nanomatch::OrderBook& book) {
        int fd = open(filepath.c_str(), O_RDONLY);
        if (fd < 0) return;

        // Get true total length of file
        off_t file_size = lseek(fd, 0, SEEK_END);
        void* mmap_ptr = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        
        if (mmap_ptr == MAP_FAILED) {
            close(fd);
            return;
        }

        uint8_t* cursor = reinterpret_cast<uint8_t*>(mmap_ptr);
        uint8_t* end_ptr = cursor + file_size;

        // Walk across the binary chunk without allocating heap copies
        while (cursor < end_ptr) {
            auto* header = reinterpret_cast<ItchCommonHeader*>(cursor);
            
            // Advance past header fields to get to raw struct payload
            uint8_t* payload = cursor + sizeof(ItchCommonHeader);

            switch (header->message_type) {
                case 'A': { // Add Order message
                    auto* msg = reinterpret_cast<ItchAddOrderMessage*>(payload);
                    uint64_t order_id = bswap64(msg->order_id);
                    uint32_t price = bswap32(msg->price);
                    uint32_t qty = bswap32(msg->shares);
                    Side side = (msg->side == 'S') ? Side::SELL : Side::BUY; 
                    book.insert_limit_order(order_id, side, price, qty);
                    break;
                }
                case 'E': { // Execute Order filling shares
                    auto* msg = reinterpret_cast<ItchOrderExecutedMessage*>(payload);
                    uint64_t order_id = bswap64(msg->order_id);
                    uint32_t executed_qty = bswap32(msg->shares);
                    
                    book.execute_order(order_id, executed_qty);
                    break;
                }
                default:
                    break;
            }
            
            // Advance cursor precisely by message footprint size
            cursor += sizeof(uint16_t) + __builtin_bswap16(header->packet_length);
        }

        munmap(mmap_ptr, file_size);
        close(fd);
    }
}