# Nanomatch

**Ultra-low latency NASDAQ ITCH 5.0 order matching engine written in C++20.**

Nanomatch is a from-scratch implementation of a high-frequency trading (HFT) order matching engine designed to process real NASDAQ TotalView-ITCH 5.0 binary feeds. It prioritizes deterministic sub-microsecond latency over throughput, using lock-free data structures, memory-mapped I/O, a custom slab allocator, and a wait-free SPSC logger — with zero heap allocations on the hot path.

---

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [Project Structure](#project-structure)
- [Core Components](#core-components)
  - [Order Book](#order-book)
  - [Matching Engine](#matching-engine)
  - [Memory Pool](#memory-pool)
  - [ID Map](#id-map)
  - [ITCH 5.0 Parser](#itch-50-parser)
  - [Async Logger](#async-logger)
- [NASDAQ ITCH 5.0 Protocol](#nasdaq-itch-50-protocol)
- [Data Flow](#data-flow)
- [Build Instructions](#build-instructions)
- [Running](#running)
  - [Synthetic Benchmark Mode](#synthetic-benchmark-mode)
  - [ITCH Replay Mode](#itch-replay-mode)
- [Benchmarks](#benchmarks)
- [Tests](#tests)
- [Performance Design Decisions](#performance-design-decisions)
- [Known Limitations](#known-limitations)
- [Dependencies](#dependencies)

---

## Architecture Overview

```
 ITCH 5.0 Binary Feed (.bin)
          │
          ▼
  ┌───────────────────┐
  │   MmapParser      │  mmap() entire file into virtual address space
  │  (parse_itch_file)│  SoupBinTCP frame decode → ITCH message dispatch
  └────────┬──────────┘
           │  per-message callbacks (A/F/E/X/D/U)
           ▼
  ┌───────────────────────────────────────────┐
  │              OrderBook (per ticker)        │
  │                                           │
  │  bids_[PRICE_BAND]   asks_[PRICE_BAND]    │  price-indexed Level arrays
  │       ▲                    ▲              │
  │  ┌────┴────┐          ┌────┴────┐         │
  │  │ PriceLevel│        │PriceLevel│        │  doubly-linked list of Orders
  │  │ (head/tail│        │(head/tail│        │  + total_volume counter
  │  └──────────┘        └──────────┘        │
  │                                           │
  │  IdMap (Robin Hood hashmap)               │  OrderId → pool index
  │  OrderPool (slab allocator)               │  fixed-size arena, O(1) alloc
  │  TopOfBook (64-byte aligned cache line)   │  best bid/ask + quantities
  └───────────────────────────────────────────┘
           │  trade events
           ▼
  ┌───────────────────┐
  │   AsyncLogger     │  SPSC lock-free ring buffer
  │                   │  dedicated drain thread → stdout/file
  └───────────────────┘
```

Each ticker (identified by ITCH `stock_locate`) gets its own `OrderBook` instance. There is no shared state between books — this means multi-ticker replay is embarrassingly parallelisable.

---

## Project Structure

```
Nanomatch/
├── CMakeLists.txt
├── src/
│   ├── core/
│   │   ├── main.cpp           # Synthetic benchmark entry point
│   │   ├── itch_main.cpp      # ITCH replay entry point
│   │   ├── OrderBook.hpp      # OrderBook, PriceLevel, TopOfBook, IdMap, OrderPool
│   │   ├── OrderBook.cpp      # Matching logic implementation
│   │   ├── Engine.hpp         # Engine wrapper (feed loop + thread management)
│   │   ├── Types.hpp          # OrderId, Price, Quantity, Side, rdtsc(), constants
│   │   └── MemoryPool.hpp     # Slab allocator
│   ├── ingestion/
│   │   ├── MmapParser.hpp     # MmapParser class + parse_itch_file() declaration
│   │   ├── MmapParser.cpp     # MmapParser implementation + ITCH parse loop
│   │   └── ItchProtocols.hpp  # POD structs for all handled ITCH message types
│   └── logging/
│       ├── AsyncLogger.hpp    # SPSC ring buffer + TradeEvent struct
│       └── AsyncLogger.cpp    # Drain thread implementation
├── benchmarks/
│   ├── CMakeLists.txt
│   └── EngineBenchmark.cpp    # Latency percentile benchmarks
└── tests/
    ├── CMakeLists.txt
    └── OrderBookTests.cpp     # Unit tests (insert, cancel, execute, match)
```

---

## Core Components

### Order Book

**File:** `src/core/OrderBook.hpp` / `src/core/OrderBook.cpp`

The `OrderBook` maintains one side-separated price-level structure for bids and asks. Prices are stored as **fixed-point integers** (ITCH unit: 1/10000 of a dollar, so $1.00 = `10000`). The book uses two flat arrays indexed directly by price:

```cpp
std::array<PriceLevel, PRICE_BAND> bids_;
std::array<PriceLevel, PRICE_BAND> asks_;
```

`PRICE_BAND` (defined in `Types.hpp`) must be large enough to cover the maximum expected price in fixed-point units. For real ITCH data this should be at least `16'000'000` (covering up to ~$1600/share).

Each `PriceLevel` is a doubly-linked list of `Order` nodes allocated from the pool:

```
PriceLevel { head_idx, tail_idx, total_volume }
    │
    └─► Order { id, price, qty, side, prev_idx, next_idx }
             └─► Order ...
```

**Key operations and their complexity:**

| Operation | Complexity | Notes |
|---|---|---|
| `insert_limit_order` | O(1) | Pool alloc + IdMap insert + level append |
| `cancel_order` | O(1) | IdMap lookup + level remove + pool free |
| `reduce_order_qty` | O(1) | IdMap lookup + qty decrement |
| `execute_order` | O(1) amortized | Partial fill = qty decrement; full fill = cancel |
| `match_against_bids` | O(k) | k = number of price levels crossed |
| `match_against_asks` | O(k) | k = number of price levels crossed |
| `update_best_bid/ask` | O(Δprice) | Linear scan from last best toward empty |

**Top of Book** is cached in a separate 64-byte cache-line-aligned struct:

```cpp
struct alignas(64) TopOfBook {
    Price    best_bid;
    Price    best_ask;
    Quantity bid_qty;
    Quantity ask_qty;
};
```

This means reading the spread never touches the price level arrays.

---

### Matching Engine

**File:** `src/core/Engine.hpp`

The `Engine` wraps an `OrderBook` with a feed loop that reads synthetic `OrderRecord`s from a memory-mapped binary file (used in benchmark/synthetic mode). It owns:

- One `OrderBook`
- One `AsyncLogger`
- One `MmapParser` (for the synthetic binary format)

In ITCH replay mode (`itch_main.cpp`), the engine is bypassed and `parse_itch_file()` dispatches directly into a `vector<unique_ptr<OrderBook>>` — one per `stock_locate`.

**Matching logic (price-time priority, FIFO within level):**

For an incoming aggressive order:
1. Check if the best opposite side crosses the limit price.
2. Walk the resting level head-to-tail (FIFO), filling as much qty as possible.
3. If a resting order is fully filled, remove it from the level and the IdMap and free it to the pool.
4. If the resting level empties, call `update_best_ask/bid()` to scan to the next non-empty level.
5. Repeat until the aggressive order is fully filled or no more crossing levels exist.
6. Any unfilled remainder of the aggressive order is inserted as a new resting limit order.

---

### Memory Pool

**File:** `src/core/MemoryPool.hpp` (also embedded in `OrderBook.hpp` as `OrderPool`)

A fixed-size slab allocator over a `std::array<Order, POOL_SIZE>`. Free slots are tracked as an intrusive free list through `Order::next_idx`. Allocation and deallocation are both O(1) with no system calls.

```cpp
uint32_t allocate();        // pops free_head_, returns slot index
void     deallocate(idx);   // pushes idx back onto free_head_
```

`POOL_SIZE` (set in `Types.hpp`) must be ≥ the maximum number of simultaneously live orders. For full ITCH replay this should be at least `1'000'000`.

---

### ID Map

**File:** `src/core/OrderBook.hpp` (`IdMap` class)

A Robin Hood open-addressing hashmap from `OrderId (uint64_t)` → `pool index (uint32_t)`. Uses linear probing with Robin Hood displacement to bound worst-case probe length.

```
Operations: find O(1) avg,  insert O(1) avg,  erase O(1) avg
Load factor: kept ≤ 0.75 via fixed capacity (must be power of 2)
```

The capacity is set at construction time; for ITCH replay it should be sized to the expected peak live order count per book (typically 100k–500k for active names).

**Important:** The backshift-on-erase invariant must hold for correctness. See the [Known Limitations](#known-limitations) section for the bug fix required in the current codebase.

---

### ITCH 5.0 Parser

**Files:** `src/ingestion/MmapParser.cpp`, `src/ingestion/ItchProtocols.hpp`

`parse_itch_file()` maps the entire ITCH binary feed into virtual memory with `mmap(MAP_PRIVATE | MAP_POPULATE)` and walks it with a raw pointer. No buffered I/O, no copies.

**Wire format (SoupBinTCP encapsulation):**

```
┌──────────────────────────────────────────────────────┐
│  2 bytes: frame length (big-endian)                  │
│  1 byte:  SoupBinTCP message type ('U' = unsequenced)│
│  N bytes: ITCH 5.0 payload                           │
│    byte 0: ITCH message type ('A','F','E','X','D','U')│
│    ...     message fields (all big-endian)           │
└──────────────────────────────────────────────────────┘
```

Cursor advancement: `cursor += 2 + frame_len` after each message.

**Handled ITCH message types:**

| Type | Name | Action |
|------|------|--------|
| `A` | Add Order (No MPID) | `insert_limit_order` |
| `F` | Add Order (With MPID) | `insert_limit_order` |
| `E` | Order Executed | `execute_order` |
| `X` | Order Cancel | `reduce_order_qty` |
| `D` | Order Delete | `cancel_order` |
| `U` | Order Replace | `cancel_order` + `insert_limit_order` |

Types `S`, `R`, `H`, `Y`, `P`, `Q`, `B`, `I`, `N` (system/admin/trade/NOII) are skipped.

**All multi-byte fields are big-endian** and decoded with `__builtin_bswap16` / `__builtin_bswap32` / `bswap64`.

**ITCH timestamp** is a 6-byte (48-bit) nanosecond value. The current implementation stores it as `uint8_t[6]` and ignores it; latency is measured with `rdtsc` at the point of book operation.

---

### Async Logger

**Files:** `src/logging/AsyncLogger.hpp` / `src/logging/AsyncLogger.cpp`

A wait-free single-producer single-consumer (SPSC) ring buffer for trade event logging. The matching engine enqueues `TradeEvent` structs on the hot path with a single atomic store; a dedicated background thread drains the buffer and writes to output.

```cpp
struct TradeEvent {
    OrderId  aggressor_id;
    OrderId  resting_id;
    Price    price;
    Quantity qty;
    uint64_t timestamp_cycles;  // rdtsc at point of match
};
```

Ring buffer size is fixed at compile time (`LOG_BUFFER_SIZE` in `Types.hpp`). If the producer outruns the consumer, `enqueue_trade()` silently drops — no blocking, no exceptions. Monitor drop rate in production by adding a `std::atomic<uint64_t> dropped_` counter.

The drain thread sleeps for 10µs when the buffer is empty to avoid spinning a full core. In production you may want to busy-spin or use `FUTEX_WAIT` if logging latency matters.

---

## NASDAQ ITCH 5.0 Protocol

NASDAQ TotalView-ITCH 5.0 is a unidirectional binary protocol carrying the full order book feed for all NASDAQ-listed securities. Key facts relevant to this implementation:

- **Prices** are in units of 1/10000 of a USD. $1.00 = `10000`, $100.00 = `1000000`.
- **`stock_locate`** is a 2-byte identifier (1–8191) assigned per security per trading session. Used here to index into `market_books`.
- **`order_id`** is a unique 8-byte reference number per resting order, assigned by NASDAQ. Not sequential.
- **Order Replace (`U`)** atomically cancels the original order and adds a new one at a (potentially) new price/size. The side is preserved from the original.
- All integers are **big-endian** on the wire.
- The feed is typically distributed via **MoldUDP64** (multicast) or **SoupBinTCP** (TCP). This implementation handles SoupBinTCP framing.

Official spec: [NASDAQ TotalView-ITCH 5.0](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf)

Sample data: [ftp.nasdaqtrader.com](ftp://ftp.nasdaqtrader.com/SymbolDirectory/) — look for `*.bin.gz` files in the ITCH section.

---

## Data Flow

### ITCH Replay Path

```
parse_itch_file(path, market_books)
  │
  ├─ mmap() full file
  │
  └─ while (cursor < end):
       frame_len = bswap16(cursor[0..1])
       itch_msg  = cursor + 3
       msg_type  = itch_msg[0]
       locate_id = bswap16(itch_msg[1..2])
       book      = market_books[locate_id]
       │
       ├─ 'A'/'F' → book->insert_limit_order(order_id, side, price, qty)
       │               IdMap::insert + OrderPool::allocate + level_append
       │               → update TOB if new best
       │
       ├─ 'E'     → book->execute_order(order_id, exec_qty)
       │               IdMap::find + qty decrement
       │               → if qty == 0: level_remove + IdMap::erase + pool::deallocate
       │               → update_best if level emptied
       │
       ├─ 'X'     → book->reduce_order_qty(order_id, cancel_qty)
       │               IdMap::find + qty decrement
       │               (level stays, order stays)
       │
       ├─ 'D'     → book->cancel_order(order_id)
       │               IdMap::find + level_remove + IdMap::erase + pool::deallocate
       │               → update_best if level emptied
       │
       └─ 'U'     → book->cancel_order(original_id)
                    book->insert_limit_order(new_id, original_side, new_price, new_qty)
```

### Synthetic Benchmark Path

```
Engine::run()
  │
  ├─ MmapParser iterates OrderRecord[]
  │    struct OrderRecord { OrderId id; Price price; Quantity qty; Side side; MsgType type; }
  │
  ├─ Per record:
  │    t0 = rdtsc()
  │    book.insert_limit_order / cancel_order / execute_order
  │    t1 = rdtsc()
  │    latency_samples.push_back(t1 - t0)
  │
  └─ On trade match: AsyncLogger::enqueue_trade(TradeEvent)
```

---

## Build Instructions

**Requirements:**
- GCC ≥ 12 or Clang ≥ 15 (C++20)
- CMake ≥ 3.16
- Linux (uses `mmap`, `O_RDONLY`, `__builtin_bswap*`, `rdtsc`)
- x86-64 (uses `rdtsc` and `-march=native`)

```bash
git clone https://github.com/raigandhiayush/Nanomatch.git
cd Nanomatch
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

This produces three binaries in `build/`:

| Binary | Purpose |
|--------|---------|
| `nanomatch` | Synthetic benchmark (reads `OrderRecord` binary file) |
| `itch_replay` | ITCH 5.0 replay (reads real NASDAQ `.bin` feed) |
| `benchmarks/engine_benchmark` | Latency percentile microbenchmark |
| `tests/order_book_tests` | Unit test suite |

**Compiler flags applied:**
```
-O3 -march=native -flto -Wall -Wextra -pthread
```

`-march=native` enables AVX2/AVX-512 autovectorization and native `bswap` instruction selection. Do not build on a different architecture than you intend to run on.

---

## Running

### Synthetic Benchmark Mode

The synthetic mode reads a flat binary file of `OrderRecord` structs and processes them through a single `OrderBook`. Use this to measure pure matching engine latency without the ITCH parsing overhead.

```bash
./nanomatch <path-to-orders.bin>
```

`orders.bin` is a flat array of:
```cpp
struct OrderRecord {
    OrderId  id;     // 8 bytes
    Price    price;  // 4 bytes
    Quantity qty;    // 4 bytes
    Side     side;   // 1 byte (BUY=0, SELL=1)
    uint8_t  type;   // 0=insert, 1=cancel, 2=execute
    uint8_t  pad[2]; // alignment
};                   // 20 bytes total
```

The file size must be a non-zero multiple of `sizeof(OrderRecord)` (the parser enforces this).

**Generating synthetic data:** You can produce a test file with any tool that writes packed structs — a small Python script or a dedicated generator binary works fine.

### ITCH Replay Mode

```bash
./itch_replay <path-to-itch-feed.bin>
```

Downloads a real ITCH sample file:
```bash
# Example: download and decompress a sample feed
wget ftp://ftp.nasdaqtrader.com/SymbolDirectory/nasdaqtrader.com.itch50
gunzip nasdaqtrader.com.itch50.gz
./itch_replay nasdaqtrader.com.itch50
```

After processing, the binary prints latency percentiles (p50, p95, p99, p99.9, p99.99, max) in CPU cycles and nanoseconds (using runtime TSC frequency estimation).

---

## Benchmarks

The microbenchmark in `benchmarks/EngineBenchmark.cpp` runs isolated latency measurements for:

- `insert_limit_order` (no match — passive resting order)
- `insert_limit_order` (with match — aggressive order crossing the spread)
- `cancel_order`
- `execute_order` (partial fill)

Each scenario is run for a configurable number of iterations with warm-up, and reports:

```
Scenario: insert (passive)
  Iterations : 1,000,000
  p50        : 48 cycles   (~16 ns @ 3.0 GHz)
  p95        : 61 cycles   (~20 ns)
  p99        : 74 cycles   (~25 ns)
  p99.9      : 112 cycles  (~37 ns)
  p99.99     : 198 cycles  (~66 ns)
  max        : 1,842 cycles (~614 ns)
```

Run with:
```bash
./benchmarks/engine_benchmark
```

**Interpreting cycle counts:** TSC frequency varies. Divide cycles by your CPU's base frequency (not boost) for a conservative nanosecond estimate. You can read it from `/proc/cpuinfo` (`cpu MHz`).

---

## Tests

Unit tests in `tests/OrderBookTests.cpp` cover:

- **Insert:** Single bid/ask, verify TOB update
- **Cancel:** Cancel resting order, verify IdMap erase and pool free
- **Partial cancel:** Reduce qty, verify order still live
- **Full match:** Aggressive order fully fills one resting order
- **Partial match:** Aggressive order partially fills, remainder rests
- **Multi-level sweep:** Aggressive order crosses multiple price levels
- **Order Replace:** Cancel + reinsert at new price, verify old id gone
- **Empty book guard:** Cancel/execute on non-existent id — no crash
- **TOB correctness:** After a sequence of inserts/cancels, verify best bid/ask is correct

Run with:
```bash
./tests/order_book_tests
```

All tests use `assert()` — failures abort with a message. A clean run prints `All tests passed.`

---

## Performance Design Decisions

| Decision | Rationale |
|---|---|
| Price-indexed flat arrays for levels | O(1) level access by price, no hashing, perfect cache locality for adjacent prices |
| Intrusive doubly-linked list in pool | Level operations (append/remove) are O(1) with no separate allocation |
| Robin Hood hashmap for IdMap | Better cache behavior than chaining; O(1) worst-case probe length with backshift-on-delete |
| Fixed slab allocator (OrderPool) | Zero syscalls on hot path; no fragmentation; deterministic allocation time |
| SPSC ring buffer for logging | Zero contention between matching thread and log thread; no mutex, no CAS loop |
| `mmap(MAP_POPULATE)` for feed | Kernel pre-faults all pages at open time; no page faults during parse loop |
| `alignas(64)` on TopOfBook | Prevents false sharing if multiple books are processed on adjacent cores |
| `-O3 -march=native -flto` | Enables loop unrolling, SIMD autovectorization, inlining across TUs |
| `rdtsc` with `lfence` | Serializing timestamp prevents CPU reordering from contaminating latency measurements |

---

## Known Limitations

The following bugs exist in the current codebase and require fixes before running against real ITCH data:

1. **`PRICE_BAND` too small** — The default value covers only ~$3.28/share. Must be set to at least `16'000'000` in `Types.hpp`.

2. **`IdMap::erase` backshift condition** — The Robin Hood backshift uses `>` where it should use `>=`, causing incorrect probe chain repair on delete. This corrupts the map for any order cancelled after a collision.

3. **`best_bid = 0` sentinel collision** — An empty book sets `best_bid = 0`, which collides with a real bid at price zero. Sentinel should be `UINT32_MAX`.

4. **ITCH struct layout off by one** — All ITCH message structs are missing the leading `message_type` byte, shifting every field by 1 byte. The parse loop must cast from `cursor + 3` (start of ITCH payload) and all structs must include `char message_type` as their first field.

5. **`stock_locate` bounds check missing** — Values above `market_books.size()` cause out-of-bounds vector access (segfault). Every switch case must guard with `if (locate_id >= market_books.size()) break`.

6. **TOB update uses `>=` instead of `>`** — `insert_limit_order` updates `best_bid` for any price `>= current best`, which incorrectly updates TOB on inserts below the current best when the sentinel is 0.

All fixes are documented in detail in the audit notes.

---

## Dependencies

No external libraries. The entire engine uses only:

- C++20 standard library (`<array>`, `<vector>`, `<atomic>`, `<thread>`, `<chrono>`)
- POSIX (`mmap`, `open`, `fstat`, `close`, `munmap`) — Linux only
- GCC/Clang builtins (`__builtin_bswap16/32`, `__builtin_expect`)
- x86 intrinsic (`rdtsc` via inline asm)

CMake `find_package(Threads)` links `-pthread` for the logger thread.
