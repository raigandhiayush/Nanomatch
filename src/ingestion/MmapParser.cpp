#include "ItchProtocols.hpp"
#include "MmapParser.hpp"
#include "../core/Types.hpp"
#include "../core/OrderBook.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <memory>

// Configuration for single-ticker benchmarking (defined in itch_main.cpp)
extern const std::string TARGET_TICKER;
extern uint16_t target_locate_id;

namespace Nanomatch {
    const std::string TARGET_TICKER = "AAPL    "; // Must be exactly 8 characters
    uint16_t target_locate_id = 0xFFFF;           //
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
}