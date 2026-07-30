# Nanomatch

![C++](https://img.shields.io/badge/C%2B%2B-20-blue)
![Build](https://img.shields.io/badge/build-CMake-success)
![License](https://img.shields.io/badge/license-MIT-green)

> A cache-aware, low-latency limit order book and matching engine written in
> modern C++20, built to explore the data-structure and systems techniques
> used in real-world high-frequency-trading infrastructure — flat memory
> pools, lock-free queues, mmap'd I/O, and a wire-accurate NASDAQ
> TotalView-ITCH 5.0 parser.

Nanomatch ships **two binaries** from a single codebase:

| Binary | Entry point | Purpose |
|---|---|---|
| `nanomatch` | `src/core/main.cpp` | Single-symbol engine driven by a synthetic, mmap'd `OrderRecord` feed (`data/market_data.bin`). Good for benchmarking the matching core in isolation. |
| `itch_replay` | `src/core/itch_main.cpp` | Multi-symbol replay engine that parses **real NASDAQ TotalView-ITCH 5.0** binary feeds, tracks a configurable set of tickers by `stock_locate`, and reports latency percentiles. |

---

## Table of Contents

- [Why Nanomatch](#why-nanomatch)
- [Architecture](#architecture)
- [Core Components](#core-components)
- [The ITCH 5.0 Ingestion Path](#the-itch-50-ingestion-path)
- [Data Layout & Memory Model](#data-layout--memory-model)
- [Algorithms](#algorithms)
- [Complexity](#complexity)
- [Building](#building)
- [Running](#running)
- [Sizing Memory for Real Feeds](#sizing-memory-for-real-feeds)
- [Benchmarking & Profiling](#benchmarking--profiling)
- [Testing](#testing)
- [Repository Layout](#repository-layout)
- [Possible Improvements](#possible-improvements)
- [References](#references)
- [License](#license)

---

## Why Nanomatch

Most toy order books reach for `std::map<Price, std::deque<Order>>` and
`std::unordered_map<OrderId, Order*>`. That's correct, but every insert,
cancel, and match walks pointers scattered across the heap — death by
cache miss at HFT-relevant order rates.

Nanomatch instead treats the order book as a **flat, index-addressed
data structure**: orders live in a single contiguous pool, price levels
are plain array slots inside a sliding window, and every "pointer" is
really a `uint32_t` index into that pool. The result is a book where the
hot path (insert / cancel / match) touches a small, predictable set of
cache lines instead of chasing heap pointers.

---

## Architecture

```text
                    Market Data (mmap'd file)
                              │
              ┌───────────────┴────────────────┐
              │                                 │
      MmapParser (OrderRecord)          ItchParser (ITCH 5.0)
      flat 18-byte synthetic feed       wire-format NASDAQ feed
              │                                 │
              └───────────────┬─────────────────┘
                              ▼
                        OrderBook
              ┌────────────────────────────┐
              │  OrderPool   (flat storage) │
              │  IdMap       (open-address) │
              │  Sliding price-band levels  │
              │  TopOfBook   (own cache line)│
              └──────────────┬──────────────┘
                              │ fills
                              ▼
                        AsyncLogger
                    (SPSC ring → writer thread)
                              │
                              ▼
                      trade_report.txt
```

`Engine` (used by `nanomatch`) wires an `OrderBook`, an `AsyncLogger`, and
an `MmapParser` together and drives a tight dispatch loop over `L`
(limit), `M` (market), and `C` (cancel) records. `itch_replay` follows
the same shape but drives the loop directly out of `parse_itch_file`,
fanning messages out to one `OrderBook` per tracked `stock_locate`.

---

## Core Components

### `OrderBook` (price-time priority matching)

Owns two sliding price-level windows (bids/asks), a flat order pool, an
ID→index map, and the top-of-book cache. Public hot-path API:

```cpp
bool insert_limit_order(OrderId id, Side side, Price price, Quantity qty) noexcept;
void cancel_order(OrderId id) noexcept;
void process_market_order(Side side, Quantity qty) noexcept;
bool execute_order(OrderId id, Quantity fill_qty) noexcept;      // partial/full fill from feed
bool reduce_order_qty(OrderId id, Quantity cancel_qty) noexcept; // partial cancel from feed
```

Every one of these is `noexcept` and allocation-free after construction.

### `OrderPool` — flat order storage

```cpp
static constexpr uint32_t MAX_ORDERS = 1u << 20; // default capacity
```

All `Order` objects live in one `mmap`'d, `MAP_POPULATE`d, `mlock`'d
array, pre-faulted and pinned so the matching thread never eats a page
fault mid-session. Free slots are tracked with an intrusive singly-linked
free list threaded through the unused `next_idx` field, so `allocate()`
and `deallocate()` are both O(1) with no calls into the general-purpose
allocator.

```cpp
struct Order {
    OrderId  id;        // 8
    uint32_t next_idx;   // 4  intrusive FIFO / free-list link
    uint32_t prev_idx;   // 4
    Price    price;      // 4  relative index into the price-band window
    Quantity qty;         // 4
    Side     side;        // 1
    uint8_t  _pad[7];
};                        // 32 bytes exactly, static_assert-enforced
```

### `IdMap` — open-addressed `OrderId → pool index`

A hand-rolled robin-hood-style open-addressing hash map replaces
`std::unordered_map<OrderId, Order*>`:

- Power-of-two capacity → `& mask` instead of `% capacity`.
- Stores 4-byte indices, not 8-byte pointers, halving the per-entry
  footprint and doubling what fits per cache line.
- 64-bit Murmur finalizer for hashing sequential/near-sequential order
  IDs into a well-distributed slot.
- Backward-shift deletion (no tombstones), so lookup chains never
  degrade as entries are erased.

### `TopOfBook` — its own cache line

```cpp
struct alignas(64) TopOfBook {
    Price best_bid, best_ask;
    Quantity bid_qty, ask_qty;
};
```

The "can this order cross?" check touches exactly one 64-byte line.

### Sliding, relative price-band windows

Rather than indexing `bids_`/`asks_` by *absolute* price (which forces
the array to span `[0, price_band)` starting at $0 — wasteful for a
cheap stock and outright infeasible for an expensive one), Nanomatch
indexes by `price - base_`, where `base_` is anchored to wherever the
ticker actually trades. `base_` is set lazily from the **first order** a
book ever sees (with a short bootstrap window that tolerates re-anchoring
while the book is still small, so an early outlier doesn't permanently
mis-anchor the window). This design was ported from
[`Amarjyoti-Chakravorty/NanoMatch`](https://github.com/Amarjyoti-Chakravorty/NanoMatch)'s
relative-band approach. A fixed-size window (e.g. `price_band=2,000,000`
→ a ~$200 span at ITCH's `$0.0001` tick size) then works for *any*
ticker, instead of needing a window sized to the most expensive
instrument you might ever see.

Each price-level slot also has a companion **bitmap** (`bid_bitmap_` /
`ask_bitmap_`, one bit per level) so scanning for the next non-empty
level near the top of book doesn't require walking every slot in the
window.

### `AsyncLogger` + `SPSCQueue` — off-thread trade reporting

Fills are pushed onto a lock-free, single-producer/single-consumer ring
buffer (`SPSCQueue<Trade, 1<<22>`) from the matching thread; a dedicated
writer thread drains it and appends CSV rows (`maker_id,taker_id,price,
qty,timestamp`) to `trade_report.txt`. The queue uses separate
`alignas(64)` head/tail atomics plus **producer-local and consumer-local
cached copies** of the opposite index, so the hot `emplace()`/`pop()`
path only touches the shared atomic when its local cache says the queue
might be full/empty — avoiding a cross-core cache-line bounce on every
single operation. If the ring is ever genuinely full, the logger drops
the trade and counts it (`dropped_count()`) rather than blocking the
matching thread on disk I/O.

### `Trade`

```cpp
struct alignas(32) Trade {
    OrderId  maker_id, taker_id;
    Price    price;
    Quantity qty;
    uint64_t timestamp;
}; // 32 bytes — two fit per 64-byte cache line
```

---

## The ITCH 5.0 Ingestion Path

`itch_replay` parses **real NASDAQ TotalView-ITCH 5.0** binary market
data — the actual wire protocol NASDAQ uses to disseminate full
order-book depth — via `mmap()` and a zero-copy, pointer-cast walk over
`#pragma pack(1)` structs mirroring the spec (`ItchProtocols.hpp`).

Message types handled:

| Type | Message | Effect on the book |
|---|---|---|
| `A` | Add Order | Insert resting limit order |
| `F` | Add Order (Attributed) | Insert resting limit order |
| `E` | Order Executed | Reduce/remove resting order on fill |
| `C` | Order Executed With Price | Reduce/remove resting order on fill (non-displayed / price-improved execution) |
| `X` | Order Cancel (partial) | Reduce resting quantity |
| `D` | Order Delete | Remove resting order |
| `U` | Order Replace | Cancel + re-insert at new price/qty/id |

The wire-format `Timestamp` field is 48-bit (6 bytes), modeled as
`uint8_t bytes[6]` rather than a native integer type, with a helper
(`itch_ts_to_u64`) to widen it — keeping every struct's field layout
exactly aligned to the real ITCH byte offsets. All multi-byte integer
fields are big-endian on the wire and are byte-swapped
(`bswap32`/`bswap64`) on read.

Parsing is bounds-checked against the mapped region on every message —
each message struct is only dereferenced after confirming
`packet_length` and the struct's size both fit before `end_ptr` — so a
truncated or malformed feed causes the parser to stop cleanly rather
than read past the mapping.

`itch_replay` only builds an `OrderBook` for `stock_locate` values you
explicitly opt into (`--all-tickers` opts into everything, which is not
recommended outside of a pre-filtered, single/few-symbol file — see
[Sizing Memory](#sizing-memory-for-real-feeds)). Order ownership across
message types is tracked in an `OrderId → OrderBook*` map (`ref_owner`)
so a later `E`/`X`/`D`/`U` for a given order is routed back to the same
per-ticker book without re-parsing which symbol it belongs to.

---

## Data Layout & Memory Model

| Structure | Storage | Notes |
|---|---|---|
| `Order` pool | `mmap` + `MAP_POPULATE` + `mlock` | Pre-faulted, pinned, never swapped |
| `bids_` / `asks_` | `std::vector<PriceLevel>`, sized to `price_band` | Indexed by `price - base_`, not absolute price |
| `IdMap` entries | `std::vector<Entry>` (open addressing) | Power-of-2 capacity, no heap allocation per insert |
| Trade queue | Fixed-size `std::array` inside `SPSCQueue` | Lock-free, cache-line-padded indices |
| ITCH feed | `mmap(PROT_READ)` | Zero-copy; parser walks the mapping directly |

`Order.price` inside the pool stores the **relative index** into the
active price-band window (not the raw tick price), so the hot
append/remove path (`level_append` / `level_remove`) never has to
re-derive `price - base_` — it's already sitting on the order.

---

## Algorithms

### Insert

1. Compute the relative slot `idx = price - base_` (bootstrap-anchoring
   `base_` from the first order if unset).
2. If the order crosses the opposite top of book, match against resting
   liquidity first (price-then-time priority).
3. Any unfilled remainder is appended to the FIFO intrusive list at its
   price level; `total_volume`/`order_count` and the level bitmap are
   updated; top-of-book is refreshed if this created a new best price.

### Cancel

1. O(1) lookup of the order's pool index via `IdMap`.
2. Unlink from its price level's intrusive doubly-linked list
   (`level_remove`).
3. Return the slot to the `OrderPool` free list; clear the level's
   bitmap bit if it's now empty; refresh top-of-book if it changed.

### Match

Incoming marketable quantity walks the opposite side's price levels from
best price outward, consuming resting orders in FIFO arrival order at
each level, emitting a `Trade` per fill via `AsyncLogger::enqueue_trade`,
until the incoming quantity is exhausted or no more crossing liquidity
remains.

### ITCH-driven partial fills / cancels

`execute_order` and `reduce_order_qty` adjust a *specific* resting order
in place (rather than running the general matching path), matching how
`E`/`C`/`X` messages report activity against a pre-existing order id;
they return whether the order was fully consumed so the caller can drop
it from `ref_owner`.

---

## Complexity

| Operation | Complexity |
|---|---|
| Insert (non-crossing) | O(1) amortized |
| Cancel | O(1) |
| ID lookup | O(1) average |
| Match | O(k), k = number of resting orders consumed |

---

## Building

Requires a C++20 compiler, CMake ≥ 3.16, and (optionally) Google
Benchmark for the microbenchmark suite.

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

This produces `nanomatch`, `itch_replay`, `engine_tests`,
`stlbaseline_benchmark`, and (if Google Benchmark is found on the
system) `engine_benchmark`.

The release build compiles with `-O3 -march=native -flto -Wall -Wextra
-pthread` — note `-march=native` means binaries are tuned for (and may
not be portable off) the machine they're built on.

---

## Running

### Synthetic single-symbol engine

Generate a synthetic 1M-record stress-test feed (limit orders, market
sweeps, and cancels with engineered edge cases around a tight initial
spread):

```bash
python3 data/generate_mock_data.py     # writes data/market_data.bin
./build/nanomatch data/market_data.bin build/trade_report.txt
```

### Real ITCH 5.0 replay

```bash
./build/itch_replay <path-to-itch-5.0-feed.bin> build/trade_report.txt 5,12,88 \
    --price-band=2000000 --max-orders=65536
```

- `data/market_data.bin` is **not** an ITCH file — it's the flat
  `OrderRecord` mock feed used by `nanomatch`. `itch_replay` needs a
  genuine NASDAQ TotalView-ITCH 5.0 binary feed.
- The 3rd argument is a comma-separated list of `stock_locate` values to
  track, or `--all-tickers` to track every symbol in the file (see
  below before doing this on a real feed).
- `itch_replay` prints a per-ticker memory estimate up front and, after
  the run, latency percentiles (p50/p90/p99/p99.9, in TSC cycles) over
  every message processed, plus diagnostic counters (`tracked_adds`,
  `successful_inserts`, `execute_calls`, `execute_hits`) to sanity-check
  that activity was actually seen for the locate IDs you passed.

---

## Sizing Memory for Real Feeds

A full-size book (`PRICE_BAND = 16,000,000` price ticks/side, `MAX_ORDERS
= 1,048,576`) is roughly **~918 MB** of pinned memory *per tracked
ticker* — a real TotalView-ITCH file touches thousands of distinct
`stock_locate` values almost immediately, so tracking "everything" is
only realistic with an enormous memory budget. `itch_replay` defaults to
a much smaller per-ticker footprint instead:

```
default: --price-band=2,000,000 --max-orders=65,536   (~111 MB/ticker)
```

Rules of thumb:

- Track only the `stock_locate` IDs you actually need (comma-separated
  list), rather than `--all-tickers`, on memory-constrained machines.
- `--price-band=N` sets ticks-per-side-per-book; widen it only if a
  tracked ticker actually trades across a wider range than the default
  covers (orders outside the band are safely rejected, not a crash).
- `--max-orders=N` sets resting-order capacity per book.
- `itch_replay` prints its computed `MB per tracked ticker` estimate at
  startup so you can sanity-check a run before it OOMs.

---

## Benchmarking & Profiling

Google Benchmark drives microbenchmarks comparing the optimized engine
against an STL baseline (`OrderBook` vs. `std::map`/`std::unordered_map`
equivalents):

```bash
./build/benchmarks/engine_benchmark
./build/benchmarks/stlbaseline_benchmark
```

`benchmarks/run_benchmarks.sh` additionally pins the process to an
isolated core with real-time scheduling priority and disables ASLR /
turbo boost for reproducible measurements:

```bash
./benchmarks/run_benchmarks.sh
```

For flame-graph profiling with `perf`:

```bash
perf stat ./build/benchmarks/engine_benchmark
perf record --call-graph fp ./build/benchmarks/engine_benchmark
perf script | ./FlameGraph/stackcollapse-perf.pl | ./FlameGraph/flamegraph.pl > flamegraph.svg
```

Sample profiling artifacts (flamegraph, `perf stat` output, cache-miss
stats, latency-percentile screenshots) live in `docs/`.

---

## Testing

```bash
./build/engine_tests
```

`tests/OrderBookTests.cpp` exercises the `OrderBook` API directly
(insert/cancel/match correctness, top-of-book maintenance) against the
same `OrderBook.cpp` translation unit used by both binaries.

---

## Repository Layout

```text
Nanomatch/
├── CMakeLists.txt
├── src/
│   ├── core/
│   │   ├── Types.hpp          # Order, Trade, PriceLevel, Side/OrderType, rdtsc()
│   │   ├── Engine.hpp         # wires OrderBook + AsyncLogger + MmapParser together
│   │   ├── OrderBook.hpp/.cpp # OrderPool, IdMap, TopOfBook, matching logic
│   │   ├── Telemetry.hpp      # global latency_samples vector
│   │   ├── main.cpp           # `nanomatch` entry point (synthetic feed)
│   │   └── itch_main.cpp      # `itch_replay` entry point (real ITCH feed)
│   ├── ingestion/
│   │   ├── ItchProtocols.hpp  # #pragma pack(1) ITCH 5.0 wire structs
│   │   ├── ItchParser.cpp     # mmap'd, bounds-checked ITCH message dispatch
│   │   ├── MmapParser.hpp/.cpp# mmap'd OrderRecord reader for the synthetic feed
│   └── logging/
│       ├── SPSCQueue.hpp      # lock-free single-producer/single-consumer ring
│       └── AsyncLogger.hpp    # trade-report writer thread
├── benchmarks/
│   ├── EngineBenchmark.cpp
│   ├── Baselinecomparsion.cpp # STL baseline comparison
│   └── run_benchmarks.sh      # isolated-core, RT-priority benchmark harness
├── tests/
│   └── OrderBookTests.cpp
├── data/
│   └── generate_mock_data.py  # synthetic 1M-record OrderRecord feed generator
└── docs/                      # flamegraph, perf stat, cache stats, screenshots
```

---

## Possible Improvements

Nanomatch is a single-threaded-per-book engine focused on data-structure
and memory-layout techniques rather than a production trading system.
Natural directions to extend it:

- **Multi-symbol parallelism** — shard tracked tickers across worker
  threads (each still single-writer per book) instead of processing the
  whole ITCH stream on one thread.
- **NUMA-aware placement** — pin each `OrderPool`/price-band pair to the
  NUMA node its matching thread runs on.
- **SIMD-accelerated scanning** — vectorize the price-level bitmap scan
  used to find the next non-empty level.
- **Order Replace hardening** — the current `U` handling cancels and
  re-inserts; a dedicated in-place modify-price/qty path would avoid the
  pool free/allocate round trip for the common "just resting quantity
  changed" case.
- **Snapshot/restore** — persist a book's state (price levels + pool) so
  a replay can resume mid-stream instead of always starting from an
  empty book.
- **Wider ITCH coverage** — the parser currently handles the order-flow
  message types (`A`/`F`/`E`/`C`/`X`/`D`/`U`); reference/trade-only
  message types (e.g. Stock Directory, Trade, NOII) are parsed enough to
  advance the cursor correctly but aren't otherwise acted on.
- **Configurable rebasing policy** — the sliding price-band's bootstrap
  re-anchoring window (`BOOTSTRAP_REBASE_LIMIT`) is a fixed constant;
  making it adaptive to a ticker's realized volatility could reduce the
  chance of a still-mis-anchored window on unusually volatile opens.

---

## References

- [NASDAQ TotalView-ITCH 5.0 Specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf)
- [Google Benchmark](https://github.com/google/benchmark)
- [Brendan Gregg's FlameGraph](https://github.com/brendangregg/FlameGraph)
- Linux `perf`

---

## License

MIT License
