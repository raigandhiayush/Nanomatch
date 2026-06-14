#pragma once
#include <string>
#include <cstddef>
#include "Types.hpp"

namespace Nanomatch {
    struct __attribute__((packed)) OrderRecord {
        char type;
        uint64_t order_id;
        uint8_t side;
        uint32_t price;
        uint32_t qty;
    };

    class MmapParser {
    public:
        explicit MmapParser(const std::string& filepath);
        ~MmapParser();

        MmapParser(const MmapParser&) = delete;
        MmapParser& operator=(const MmapParser&) = delete;

        const OrderRecord* get_records() const noexcept { return records_; }
        size_t count() const noexcept { return record_count_; }

    private:
        int fd_{-1};
        void* mmap_ptr_{nullptr};
        size_t file_size_{0};
        
        const OrderRecord* records_{nullptr};
        size_t record_count_{0};
    };
}