#pragma once
#include "Types.hpp"
#include <vector>
#include <stdexcept>

namespace Nanomatch{
    class MemoryArena{
        private:
            size_t capacity_;
            Order* head_;
            std::vector<Order> pool_;

        public:
            explicit MemoryArena(size_t max_orders) : capacity_(max_orders), head_(nullptr){
                pool_.resize(capacity_);
                for(size_t i=0; i<capacity_-1; i++){
                    pool_[i].next = &pool_[i];
                }
                pool_[capacity_-1].next=nullptr;
                head_=&pool_[0];
            }
            
            MemoryArena(const MemoryArena&) = delete;
            MemoryArena& operator=(const MemoryArena&) = delete;

            inline Order* allocate() noexcept{
                if(__builtin_expect((head_==nullptr),0)){
                    return nullptr;
                }
                Order* node=head_;
                head_ = head_->next;
                node->next = nullptr;
                node->prev = nullptr;
                return node;
            }

            inline void deallocate(Order* order) noexcept{
                if(__builtin_expect((head_==nullptr),0)) return;

                order->next = head_;
                head_ = order;
            }

            size_t capacity() const noexcept{
                return capacity_;
            }
    };
}