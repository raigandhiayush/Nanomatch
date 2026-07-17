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
#include <unordered_set>

extern std::vector<uint64_t> latency_samples;

namespace Nanomatch {
    static constexpr size_t kMaxStockLocate = 8192;

    // Bug fix (OOM crash on real ITCH feeds): every distinct stock_locate
    // seen in the file used to get its own full-size OrderBook -- dense
    // bids_/asks_ arrays sized to PRICE_BAND (16,000,000 slots each, ~854MB)
    // plus a 1,048,576-order pool that's mlock'd/prefaulted immediately
    // (~32MB pinned, non-swappable). That's ~918MB PHYSICALLY PINNED per
    // ticker. A real NASDAQ TotalView-ITCH file touches thousands of
    // distinct stock_locate values in the first few thousand messages, so
    // on a 4GB machine you OOM (and get killed by the kernel) after
    // allocating books for maybe 4-5 tickers -- regardless of file size.
    // There's no way to hold a full-exchange, full-price-band replay in
    // 4GB with this data layout; the fix is to only build books for the
    // tickers you actually care about. `allowed_locates` does that: if
    // non-empty, any stock_locate not in the set is skipped entirely (its
    // messages are still parsed/validated for correct cursor advancement,
    // just never allocate a book or touch ref_owner for it).
    void parse_itch_file(const std::string& filepath, AsyncLogger& logger,
                          const std::unordered_set<uint16_t>& allowed_locates = {},
                          size_t price_band = PRICE_BAND, size_t max_orders = MAX_ORDERS) {
        int fd = open(filepath.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("Could not open file: " + filepath);

        off_t file_size = lseek(fd, 0, SEEK_END);
        if (file_size < 0) { close(fd); throw std::runtime_error("lseek failed: " + filepath); }

        void* mmap_ptr = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mmap_ptr == MAP_FAILED) { close(fd); throw std::runtime_error("mmap failed: " + filepath); }

        std::vector<std::unique_ptr<OrderBook>> market_books(kMaxStockLocate);
        std::unordered_map<OrderId, OrderBook*> ref_owner;
        ref_owner.reserve(1u << 20);

        // empty allowed_locates == "allow every ticker" (the old, OOM-prone
        // behavior). Callers on memory-constrained machines should always
        // pass a non-empty set.
        auto is_tracked = [&](uint16_t locate) noexcept {
            return allowed_locates.empty() || allowed_locates.count(locate) != 0;
        };

        uint8_t* cursor  = reinterpret_cast<uint8_t*>(mmap_ptr);
        uint8_t* end_ptr = cursor + file_size;
        uint64_t message_count = 0;
        uint64_t rejected_price_band = 0;   // diagnostic: orders priced outside [0, price_band)
        uint64_t tracked_adds = 0;          // diagnostic: adds seen for a tracked locate_id

        while (cursor < end_ptr) {
            // Bug fix: previously the header (and every message payload cast
            // that follows) was dereferenced with no check that it actually
            // fits inside the mapped region. Malformed/truncated ITCH data
            // (or a file that isn't ITCH data at all, e.g. the OrderRecord
            // mock feed used by the `nanomatch` binary) produces garbage
            // packet_length values, which used to walk `cursor` to within a
            // few bytes of end_ptr and then dereference a 30-40 byte struct
            // straight off the end of the mapping -> SIGSEGV. That's the
            // crash you were hitting.
            if (cursor + sizeof(ItchCommonHeader) > end_ptr) {
                break;
            }

            auto* header = reinterpret_cast<ItchCommonHeader*>(cursor);
            uint8_t* payload = cursor + sizeof(ItchCommonHeader);
            uint16_t packet_length = __builtin_bswap16(header->packet_length);
            uint16_t locate_id = 0;

            // packet_length must at least cover the message_type byte, and
            // the whole record (2-byte length prefix + packet_length bytes)
            // must fit before end_ptr. If not, this isn't valid ITCH framing
            // -- bail instead of reading off the end of the mmap.
            if (packet_length < 1 || cursor + sizeof(uint16_t) + packet_length > end_ptr) {
                break;
            }

            // Body available after the header, used to bounds-check each
            // specific message struct below before it's dereferenced.
            size_t body_avail = static_cast<size_t>(end_ptr - payload);

            switch (header->message_type) {
                case 'A': {
                    if (body_avail < sizeof(ItchAddOrderMessage)) { break; }
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchAddOrderMessage*>(payload);
                    locate_id = __builtin_bswap16(msg->stock_locate);
                    if (locate_id < kMaxStockLocate && is_tracked(locate_id)) {
                        ++tracked_adds;
                        auto& book = market_books[locate_id];
                        if (!book) book = std::make_unique<OrderBook>(price_band, logger, max_orders);
                        Price px = bswap32(msg->price);
                        if (px >= price_band) ++rejected_price_band;
                        if (book->insert_limit_order(bswap64(msg->order_id), (msg->side == 'S') ? Side::SELL : Side::BUY, px, bswap32(msg->shares))) {
                            ref_owner[bswap64(msg->order_id)] = book.get();
                        }
                    }
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'F': {
                    if (body_avail < sizeof(ItchAddOrderAttributedMessage)) { break; }
                    uint64_t t0 = Nanomatch::rdtsc();
                    auto* msg = reinterpret_cast<ItchAddOrderAttributedMessage*>(payload);
                    locate_id = __builtin_bswap16(msg->stock_locate);
                    if (locate_id < kMaxStockLocate && is_tracked(locate_id)) {
                        ++tracked_adds;
                        auto& book = market_books[locate_id];
                        if (!book) book = std::make_unique<OrderBook>(price_band, logger, max_orders);
                        Price px = bswap32(msg->price);
                        if (px >= price_band) ++rejected_price_band;
                        if (book->insert_limit_order(bswap64(msg->order_id), (msg->side == 'S') ? Side::SELL : Side::BUY, px, bswap32(msg->shares))) {
                            ref_owner[bswap64(msg->order_id)] = book.get();
                        }
                    }
                    latency_samples.push_back(Nanomatch::rdtsc() - t0);
                    break;
                }
                case 'E': {
                    if (body_avail < sizeof(ItchOrderExecutedMessage)) { break; }
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
                    if (body_avail < sizeof(ItchOrderCancelMessage)) { break; }
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
                    if (body_avail < sizeof(ItchOrderDeleteMessage)) { break; }
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
                    if (body_avail < sizeof(ItchOrderReplaceMessage)) { break; }
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

        // Diagnostic: this is what was silently causing empty trade reports.
        // insert_limit_order() rejects any order priced >= price_band with
        // no logging at all (it's a noexcept hot-path fast return). If your
        // ticker's actual price is above what --price-band covers, every
        // single order for it gets dropped, ref_owner never gets populated,
        // so the later 'E' (Order Executed) messages that reference those
        // order ids find nothing in ref_owner and log zero trades. The run
        // "succeeds" and produces an empty report instead of an error.
        if (tracked_adds > 0 && rejected_price_band > 0) {
            std::cerr << "[Warning] " << rejected_price_band << " / " << tracked_adds
                      << " add-order messages for tracked ticker(s) were priced outside "
                      << "the configured --price-band (" << price_band
                      << " ticks, i.e. up to $" << (price_band / 10000.0)
                      << " at ITCH's $0.0001 scale) and were dropped. "
                      << "If this ticker trades above that, rerun with a larger --price-band.\n";
        } else if (tracked_adds == 0) {
            std::cerr << "[Warning] No add-order messages were seen for the tracked locate_id(s). "
                      << "Either that stock_locate had no activity in this file, or it's the wrong "
                      << "locate_id for the ticker you meant -- dump the 'R' Stock Directory messages "
                      << "to confirm the locate_id -> ticker mapping.\n";
        }
    }
}