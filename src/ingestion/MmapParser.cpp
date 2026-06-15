#include "ItchProtocols.hpp"
#include "MmapParser.hpp"
#include "../core/OrderBook.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>

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
        if (fd < 0) return;

        off_t file_size = lseek(fd, 0, SEEK_END);
        void* mmap_ptr = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        
        if (mmap_ptr == MAP_FAILED) {
            close(fd);
            return;
        }

        uint8_t* cursor = reinterpret_cast<uint8_t*>(mmap_ptr);
        uint8_t* end_ptr = cursor + file_size;

        while (cursor < end_ptr) {
            auto* header = reinterpret_cast<ItchCommonHeader*>(cursor);
            uint8_t* payload = cursor + sizeof(ItchCommonHeader);

            switch (header->message_type) {
                case 'A': { 
                    auto* msg = reinterpret_cast<ItchAddOrderMessage*>(payload);
                    uint64_t order_id = bswap64(msg->order_id);
                    uint32_t price = bswap32(msg->price);
                    uint32_t qty = bswap32(msg->shares);
                    Side side = (msg->side == 'S') ? Side::SELL : Side::BUY; 
                    book.insert_limit_order(order_id, side, price, qty);
                    break;
                }
                case 'E': { 
                    auto* msg = reinterpret_cast<ItchOrderExecutedMessage*>(payload);
                    uint64_t order_id = bswap64(msg->order_id);
                    uint32_t executed_qty = bswap32(msg->shares);
                    
                    book.execute_order(order_id, executed_qty);
                    break;
                }
                default:
                    break;
            }
            cursor += sizeof(uint16_t) + __builtin_bswap16(header->packet_length);
        }

        munmap(mmap_ptr, file_size);
        close(fd);
    }
}