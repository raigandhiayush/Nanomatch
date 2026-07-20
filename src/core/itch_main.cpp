#include "OrderBook.hpp"
#include "AsyncLogger.hpp"
#include <iostream>
#include <sstream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <unordered_set>

namespace Nanomatch {
    void parse_itch_file(const std::string& filepath, AsyncLogger& logger,
                          const std::unordered_set<uint16_t>& allowed_locates,
                          size_t price_band, size_t max_orders);
}

// Global vector to collect latency samples without dynamic resizing penalties during the run
std::vector<uint64_t> latency_samples;

int main(int argc, char* argv[]) {
    // Bug fix: this used to default to "data/market_data.bin" -- the same
    // path the `nanomatch` binary uses. But that file is the flat
    // OrderRecord mock feed (data/generate_mock_data.py, 18-byte records),
    // NOT real ITCH 5.0 framing. Feeding it to parse_itch_file() made the
    // parser interpret random bytes as packet_length/message_type, walk off
    // into garbage offsets, and (pre-bounds-check) segfault. itch_replay
    // needs an actual ITCH 5.0 .bin feed passed explicitly -- no silent
    // fallback to the wrong file format.
    //
    // Bug fix (OOM crash on real ITCH feeds): parse_itch_file used to build
    // a full OrderBook -- ~918MB, ~32MB of which is mlock'd/pinned -- for
    // EVERY distinct stock_locate it saw. A real TotalView-ITCH file touches
    // thousands of tickers almost immediately, so on a 4GB machine you OOM
    // after only 4-5 tickers regardless of the file's total size. There is
    // no way to hold a full-exchange replay at this price-band resolution
    // in 4GB. You must restrict the run to specific stock_locate ids.
    // Bug fix (still OOMing at 1 ticker on tight-RAM machines): OrderBook
    // always sized bids_/asks_ to the full PRICE_BAND (16,000,000 ticks/side
    // = 854MB) and the order pool to MAX_ORDERS (1,048,576 = 32MB mlock'd),
    // regardless of how many tickers you're actually tracking. A single
    // stock's real trading range is a tiny fraction of $1,600, and it
    // doesn't have a million orders resting at once. These defaults are cut
    // ~8x (price_band 2,000,000 / max_orders 65,536, ~111MB/ticker) unless
    // overridden -- pass --price-band=N and/or --max-orders=N (4th/5th arg)
    // to widen them back out if you have the RAM and need a wider range.
    if (argc < 2) {
        std::cerr << "Usage: itch_replay <path-to-itch-5.0-feed.bin> <log_file> <locate_ids|--all-tickers> [--price-band=N] [--max-orders=N]\n"
                      "  Note: data/market_data.bin is NOT an ITCH file -- it's the\n"
                      "  flat OrderRecord mock feed used by the `nanomatch` binary.\n"
                      "  Point this at a real NASDAQ TotalView-ITCH 5.0 .bin feed.\n\n"
                      "  locate_ids: comma-separated stock_locate values to track, e.g. 5,12,88.\n"
                      "  --all-tickers: build a book for every ticker in the file. This WILL\n"
                      "              OOM on anything less than ~8TB of RAM for a full-exchange\n"
                      "              feed -- only use it against a file you've already filtered\n"
                      "              down to a handful of symbols.\n"
                      "  --price-band=N: price ticks per side per book (default 2,000,000, i.e.\n"
                      "              prices up to ~$200 at ITCH's $0.0001 fixed-point scale).\n"
                      "              Orders priced outside the band are safely rejected, not\n"
                      "              a crash -- widen this only if your ticker actually trades\n"
                      "              above that range.\n"
                      "  --max-orders=N: resting-order capacity per book (default 65,536).\n";
        return 1;
    }
    const std::string itch_file = argv[1];
    const std::string log_file  = (argc > 2) ? argv[2] : "build/trade_report.txt";

    size_t price_band = 2'000'000;   // ~111MB/ticker instead of ~918MB at PRICE_BAND defaults
    size_t max_orders  = 65'536;

    std::unordered_set<uint16_t> allowed_locates;
    if (argc > 3) {
        std::string arg = argv[3];
        if (arg != "--all-tickers") {
            std::stringstream ss(arg);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                if (!tok.empty()) allowed_locates.insert(static_cast<uint16_t>(std::stoul(tok)));
            }
        }
        // arg == "--all-tickers" leaves allowed_locates empty -> track everything
    } else {
        std::cerr << "[Error] No locate_ids given and --all-tickers not passed. "
                      "Refusing to run in unbounded multi-ticker mode (see usage above) "
                      "-- this is what was OOM-killing the process.\n";
        return 1;
    }
    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--price-band=", 0) == 0) price_band = std::stoul(arg.substr(13));
        else if (arg.rfind("--max-orders=", 0) == 0) max_orders = std::stoul(arg.substr(13));
    }

    size_t n_tickers = allowed_locates.empty() ? 1 : allowed_locates.size(); // lower bound if --all-tickers
    double mb_per_book = (2.0 * price_band * 28 + max_orders * 32.0 + max_orders * 2 * 16.0) / (1024.0 * 1024.0);
    std::cout << "[System] ~" << mb_per_book << " MB per tracked ticker"
              << (allowed_locates.empty() ? " (--all-tickers: unbounded ticker count!)\n"
                                           : " x " + std::to_string(n_tickers) + " ticker(s) = ~" +
                                             std::to_string(mb_per_book * n_tickers) + " MB\n");

    // Pre-allocate space for 50 million samples to avoid runtime vector resizing overhead
    latency_samples.reserve(50000000);

    std::cout << "[System] Initializing Multi-Ticker OrderBook Engine & AsyncLogger...\n";
    Nanomatch::AsyncLogger logger(log_file);
    logger.start();

    std::cout << "[System] Mapping and processing ITCH data stream with latency tracking...\n";
    
    auto start_time = std::chrono::high_resolution_clock::now();

    Nanomatch::parse_itch_file(itch_file, logger, allowed_locates, price_band, max_orders);

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;
    logger.stop();

    std::cout << "[Success] ITCH file parsing stream processed in " << elapsed.count() << " seconds.\n";

    std::sort(latency_samples.begin(), latency_samples.end());
    if (!latency_samples.empty()) {
        auto get_pct = [](double pct) {
            size_t idx = static_cast<size_t>(pct * latency_samples.size() / 100.0);
            return latency_samples[std::min(idx, latency_samples.size() - 1)];
        };
        std::cout << "=== Latency Percentiles (Cycles) ===\n";
        std::cout << "p50:   " << get_pct(50.0)  << "\n";
        std::cout << "p90:   " << get_pct(90.0)  << "\n";
        std::cout << "p99:   " << get_pct(99.0)  << "\n";
        std::cout << "p99.9: " << get_pct(99.9)  << "\n";
    }

    return 0;
}