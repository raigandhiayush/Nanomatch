#pragma once
#include "Types.hpp"
#include <string>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

namespace Nanomatch {

#pragma pack(push, 1)
struct OrderRecord {
    OrderId  order_id;  
    Price    price;     
    Quantity qty;       
    uint8_t  side;      
    char     type;      
};
#pragma pack(pop)
static_assert(sizeof(OrderRecord) == 18);

class MmapParser {
public:
    explicit MmapParser(const std::string& path);
    ~MmapParser();

    MmapParser(const MmapParser&)            = delete;
    MmapParser& operator=(const MmapParser&) = delete;

    [[nodiscard]] const OrderRecord* get_records() const noexcept { return data_; }
    [[nodiscard]] size_t             count()       const noexcept { return count_; }

private:
    int                 fd_    {-1};
    size_t              size_  {0};
    size_t              count_ {0};
    const OrderRecord* data_  {nullptr};
};

} // namespace Nanomatch