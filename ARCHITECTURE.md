# O(1) Exchange - Architecture

This document explains how the exchange works end to end: the data structures, the matching algorithm, the multi-instrument design, the dual-engine (CURRENT vs LEGACY) comparison, the WebSocket protocol, and how the React frontend fits in.

---

## Table of Contents

1. [System overview](#1-system-overview)
2. [The matching engine](#2-the-matching-engine)
3. [Price/time priority](#3-pricetime-priority)
4. [Dual engine: CURRENT vs LEGACY](#4-dual-engine-current-vs-legacy)
5. [Multi-instrument market design](#5-multi-instrument-market-design)
6. [The WebSocket server](#6-the-websocket-server)
7. [The JSON protocol](#7-the-json-protocol)
8. [The React frontend](#8-the-react-frontend)
9. [Data flow: a full order lifecycle](#9-data-flow-a-full-order-lifecycle)
10. [Concurrency model](#10-concurrency-model)
11. [Build system and deployment](#11-build-system-and-deployment)
12. [Testing strategy](#12-testing-strategy)
13. [Known limitations and future work](#13-known-limitations-and-future-work)

---

## 1. System overview

The exchange has three layers that communicate in one direction only: the frontend talks to the server over WebSocket, and the server owns everything else. Inside the server, a single dedicated thread owns all order-book mutation; nothing else is allowed to touch it directly.

```
┌─────────────────────────────────────────────────────────────┐
│                     React Frontend                          │
│  App.js + StressTest.js + PerformanceGraphs.js               │
└───────────────────────┬─────────────────────────────────────┘
                         │  ws://host:9001 (JSON + binary trade frames)
┌───────────────────────▼─────────────────────────────────────┐
│                  WebSocket Server (Server.h)                │
│  Boost.Beast + Boost.Asio, single io_context thread          │
│  Sessions never touch the book - they enqueue EngineTasks    │
└──────────┬──────────────────────────────┬───────────────────┘
           │ lock-free SPSC queue          │ double-buffered DbTask vectors
┌──────────▼──────────────────┐  ┌─────────▼─────────────────┐
│   engine_thread_             │  │   db_worker_ thread        │
│   sole writer of every       │  │   settles trades in        │
│   InstrumentState::book      │  │   batches, one SQLite      │
│   (MarketState)              │  │   transaction per batch    │
└───────────────────────────────┘  └─────────────────────────┘
                                              │
                                    ┌─────────▼─────────────────┐
                                    │  Database (SQLite, WAL)   │
                                    │  users, balances,          │
                                    │  portfolios, sessions,     │
                                    │  trades                    │
                                    └─────────────────────────┘
```

Order books live entirely in memory for the lifetime of the process - real matching engines work this way because RAM access is nanoseconds and disk access is microseconds. Everything that needs to survive a restart (accounts, balances, portfolios, trade history, session tokens) is persisted separately through the `Database` layer, off the matching hot path.

---

## 2. The matching engine

The engine lives in `include/OrderBook.h`. It replaced an earlier `std::map`-based design with a specialized, cache-conscious structure built around three ideas: a preallocated node pool, intrusive linked lists, and a hierarchical bitmap index.

### The node pool

```cpp
std::vector<OrderNode> node_pool_;      // MAX_ORDERS (100,000), preallocated
std::vector<uint32_t>  free_indices_;   // stack of unused pool slots
```

`OrderNode` is explicitly packed and padded to exactly 64 bytes - one cache line:

```cpp
struct OrderNode {
    Order order;          // 40 bytes
    uint32_t prev_idx;    // 4 bytes
    uint32_t next_idx;    // 4 bytes
    uint8_t padding[16];  // -> 64 bytes total
};
```

No `new`/`delete` happens once the pool is allocated at startup. A new order pops a free slot off `free_indices_`; a filled or cancelled order pushes its slot back. `MAX_ORDERS` is deliberately kept at 100,000 rather than millions specifically so the whole pool fits in L3 cache.

### The price-indexed arrays

```cpp
OrderList bids_[MAX_PRICE];   // MAX_PRICE = 100,000 (cents, i.e. $0.00-$999.99)
OrderList asks_[MAX_PRICE];
```

Each `OrderList` is a doubly-linked list of pool indices (`head_idx`/`tail_idx`), not a container of objects - the nodes themselves live in `node_pool_`. Walking a price level means following `next_idx` through the pool; inserting or removing a node is pure pointer (index) surgery, O(1) regardless of how many orders rest at that price.

### The hierarchical bitmap

Finding the *best* price without scanning 100,000 array slots is the other half of the design: a 3-level bitmap per side (`bids_l1_`/`bids_l2_`/`bids_l3_`, mirrored for asks) where each bit says "this price (or range of prices) has at least one resting order." The best bid is the highest set bit; the best ask is the lowest. Both are found with `__builtin_clzll`/`__builtin_ctzll` (count leading/trailing zero bits) - a handful of CPU instructions regardless of book depth.

### Order IDs double as pool addresses

```cpp
order.id = ((++order_id_counter) << 32) | pool_idx;
```

The low 32 bits of every order ID *are* its pool index - cancellation is a direct array lookup (`order_id & 0xFFFFFFFF`), not a hash-map lookup. The high 32 bits are a monotonically increasing counter, so `cancel_order` can verify `node.order.id == order_id` exactly before touching anything: if a pool slot has since been recycled for a different order, the ID won't match and the cancel is safely rejected instead of silently operating on the wrong order.

---

## 3. Price/time priority

Every real exchange enforces two rules when deciding which resting order gets matched first.

**Rule 1 - Price priority:** the best-priced order wins. Highest bid for buyers, lowest ask for sellers.

**Rule 2 - Time priority:** among orders at the same price, first in, first served (FIFO).

| Rule | Mechanism | Complexity |
|------|-----------|-----------|
| Price priority | 3-level bitmap scan (`__builtin_clzll`/`ctzll`) | O(1) |
| Time priority | Doubly-linked `OrderList` per price, `head_idx` is earliest | O(1) |

Both halves of the hot path - find the best price, take the earliest order resting there - are O(1). This is the origin of the project's name.

### Partial fills

If an incoming order's quantity exceeds what's available at the best price, the engine fills what it can and moves to the next crossing price level, repeating until the order is fully filled or no price levels cross anymore. Whatever quantity remains rests in the book.

### Self-trade prevention

Before executing a match, the engine checks `incoming.user_id == resting.user_id`. If they're equal, the match is refused: the *entire remaining* incoming quantity is rejected (not just skipped past that one resting order), the session gets an error notification, and any cash/shares reserved for the rejected portion are refunded. This is a deliberate "cancel-on-self" policy rather than "skip-on-self" - simpler to reason about, and sufficient for this project's needs.

---

## 4. Dual engine: CURRENT vs LEGACY

The matching engine sits behind an abstract interface, `IOrderBook` (`include/IOrderBook.h`), so the rest of the system never depends on which concrete implementation is active:

```cpp
class IOrderBook {
public:
    virtual bool can_process_order(Order&) = 0;
    virtual uint32_t process_buy_order(Order&) = 0;
    virtual uint32_t process_sell_order(Order&) = 0;
    virtual bool cancel_order(uint64_t order_id, int64_t user_id, Order& out) = 0;
    virtual std::vector<DepthLevel> bid_depth(...) const = 0;
    // ... ask_depth, bid_orders, ask_orders, best_bid, best_ask, engine_name
};
```

Two implementations exist:

- **`OrderBook`** ("CURRENT") - the bitmap-and-pool engine described above.
- **`OrderBookLegacy`** ("LEGACY", `include/OrderBookLegacy.h`) - a deliberately naive baseline: `std::map<uint32_t, PriceLevel>` per side, `std::list<Order>` per price level, heap-allocated nodes, no pool. Same matching semantics, including self-trade prevention. `tests/test_orderbook_legacy_parity.cpp` verifies both engines produce identical trade sequences and identical resting-book state for identical input - LEGACY exists purely as a real, correctness-verified comparison point, not a toy.

`MarketState::set_engine_mode(EngineMode)` swaps every instrument's book at once by constructing a fresh `IOrderBook` of the requested kind (`make_order_book()` in `Market.h`), discarding whatever was resting. This is safe without extra locking because it only ever runs on `engine_thread_` - the same single-writer thread that processes every order and cancel - so by the time a `SWITCH_ENGINE` task is dequeued, every task queued before it has already been applied to the old engine.

Two ways to compare them:

- **`benchmark/benchmark_orderbook.cpp`** (Google Benchmark) - controlled, repeatable, native measurements across both engines at 1K/10K/100K orders: simple matching, book building, partial fills, cancellations. Run with `./benchmarks --benchmark_min_time=0.2s`.
- **The Stress Test panel** (`StressTest.js` + `PerformanceGraphs.js`) - fires configurable synthetic order bursts from the browser (tagged `stress:true` with a synthetic negative `user_id` so wallet reservation/settlement and self-trade prevention don't get in the way of load generation) and plots live throughput/p50/p99 latency for whichever engine is active, sourced from the `metrics` broadcast the server sends every ~500ms.

---

## 5. Multi-instrument market design

### The problem with one global book

A single global order book can't support multiple companies correctly - a buy for Apollo at $150 would happily match a sell for Crown at $150, which is nonsensical; price only means something relative to one instrument. Filtering by company during matching would work but destroys the O(1) property, turning the hot path into an O(n) scan over companies.

### The actual design

One completely independent `IOrderBook` per company, addressed directly by array index:

```cpp
struct InstrumentState {
    Company company;
    std::unique_ptr<IOrderBook> book;
    // no mutex: the single engine thread has exclusive ownership
};

class MarketState {
    std::vector<std::unique_ptr<InstrumentState>> instruments_; // direct-mapped by company_id
};
```

`find_instrument(company_id)` is a bounds-checked array index, not a map lookup. Routing an order is one lookup, then `instrument->book->process_buy_order(...)`/`process_sell_order(...)` - Apollo's book is completely untouched by a Crown order, and the matching complexity inside each book is unchanged.

### Listed companies

Three companies are hard-coded at startup in `src/main.cpp`:

| ID | Symbol | Name | Total Shares |
|----|--------|------|-------------|
| 1 | APL | Apollo Technologies | 1,000,000 |
| 2 | BLZ | Blaze Manufacturing | 2,500,000 |
| 3 | CRN | Crown Energy | 1,750,000 |

Adding a company today means editing that list and recompiling - see [§13](#13-known-limitations-and-future-work).

---

## 6. The WebSocket server

The server lives in `include/Server.h`, built on Boost.Beast (WebSocket) and Boost.Asio (async I/O). All session I/O - accepting connections, reading, writing - runs as async callbacks on the single `io_context` thread driven by `ioc.run()` in `main.cpp`. Nothing here blocks a thread per client.

### Sessions never touch the book

A `Session` parses an incoming message, validates it, and - for anything that mutates or reads the book - builds an `EngineTask` (`ORDER`, `CANCEL`, `SNAPSHOT`, or `SWITCH_ENGINE`) and pushes it onto a lock-free single-producer/single-consumer queue:

```cpp
template <typename T, size_t Size>
class SPSCQueue { /* atomic head/tail, power-of-2 ring buffer */ };
using EngineQueue = SPSCQueue<EngineTask, 65536>;
```

`engine_thread_` is the queue's sole consumer and the sole writer of every `InstrumentState::book`. It pops tasks, calls into the right instrument's `IOrderBook`, and is also where matching latency is timed for the `metrics` broadcast.

### Outbound writes

Each session has its own `std::deque<OutboundMsg> write_queue_` (a deque specifically for O(1) `pop_front()` - an earlier `std::vector` + `erase(begin())` version was O(n) per dequeue). `send()` posts onto the session's executor via `net::post(...)`; since every session shares the one `io_context` thread, this is safe to call from `engine_thread_` or `db_worker_` without any additional locking - `post()` itself is the thread-safety boundary, and the actual queue push/write always executes back on the single I/O thread.

### Settlement is off the matching path

`Server` implements `ITradeListener`; every instrument's book points `trade_listener` at it. `on_trade()` records the trade, broadcasts a 27-byte binary frame to every connected client, and pushes `DbTask`s onto a pair of double-buffered vectors (`db_queue_front_`/`db_queue_back_`). `db_worker_` swaps the buffers under a condition variable, then settles the whole batch inside one SQLite transaction, holding `db_.mutex()` for the entire begin→loop→commit sequence so no concurrent read can observe a partially-applied batch.

### On-connect behavior

A newly connected session immediately gets a `SNAPSHOT` task queued for the default company, so it sees the market's current state - not a blank book - before it does anything else.

---

## 7. The JSON protocol

All communication is JSON over the WebSocket connection, except trade fills, which use a compact 27-byte binary frame for throughput.

### Client → Server

| Type | Purpose |
|---|---|
| `register` / `login` | Account creation / session-token auth |
| `order` | Submit a limit order (`price`, `quantity`, `side`, `timestamp`; `stress`/`synthetic_user_id` to bypass wallet handling for load tests) |
| `market_order` | Submit a market order, fills at the current best opposing price |
| `cancel` | Cancel a resting order by `order_id`, ownership-verified server-side |
| `snapshot` | Request full book + price history + engine mode for a company |
| `my_trades` | Request this user's trade history |
| `leaderboard` | Request the cash-ranked leaderboard |
| `switch_engine` | `{"mode":"CURRENT"\|"LEGACY"}` - swap the whole market's engine |

### Server → Client

| Type | Purpose |
|---|---|
| `trade` (binary) | A fill: company_id, price, quantity, buy/sell order ids |
| `book` | Bid/ask depth + open orders for one instrument |
| `snapshot` | `book` plus `companies`, recent trade history, and persisted `price_history` |
| `user_update` | Cash/portfolio after a settlement affecting this user |
| `leaderboard` | Ranked list of users by cash |
| `my_trades` | This user's trade history |
| `engine_mode` | Broadcast after a `switch_engine` completes |
| `metrics` | Every ~500ms: engine_mode, orders processed, throughput, p50/p99 match latency (matching call only, not WS/DB time) |
| `error` | Rejection reason: insufficient funds, missing fields, self-trade prevention, capacity limit, etc. |

---

## 8. The React frontend

`frontend/src/App.js` is a single-page dashboard: a WebSocket client with no backend of its own. It renders whatever state the server sends.

**Per-instrument state** (replaced on company switch): bid/ask depth, best bid/ask/spread, selected company's order book.

**Cross-instrument state** (persists across company switches): the trade tape, and - critically - the user's own open orders. `myOrdersByCompany` is keyed by company ID and *merged*, not overwritten, on every incoming `book` message, so switching companies doesn't make a user's resting orders on other instruments disappear from the "My Open Orders" panel.

**Reconnection:** if the socket closes unexpectedly, the frontend retries with exponential backoff (`1s, 2s, 4s, ... capped at 30s`) and shows a `DISCONNECTED` badge, no page refresh needed.

**Embedded panels:** `StressTest.js` (engine toggle, load generator, live send-rate stats) and `PerformanceGraphs.js` (throughput/latency sparklines driven by the `metrics` stream) mount inside the dashboard rather than as a separate tool.

---

## 9. Data flow: a full order lifecycle

What happens when a user submits a buy order for 150 shares of APL at $102.50 (client-side cents: 10250):

```
1. App.js sends { type:"order", token, company_id:1, side:"BUY",
                   price:10250, quantity:150, timestamp }

2. Session::handle_message() validates required fields are present,
   validates the price ceiling, validates the session token, then
   (for a non-stress order) reserves $153.75 of cash via
   Database::reserve_cash() - before the order ever reaches the book.

3. An EngineTask{ORDER} is pushed onto the SPSC queue.

4. engine_thread_ pops it, calls instrument->book->can_process_order():
   rejects only if the price is out of range or the node pool is full.
   On rejection, the earlier reservation is refunded via a queued
   DB_REFUND_CASH task and the session gets an error.

5. On success, process_buy_order() runs match_buy():
   - reads the best ask off the bitmap, O(1)
   - if it crosses (best_ask <= 10250) and isn't a self-trade, fills
     from the head of that price level's linked list, generates a
     Trade, and calls trade_listener->on_trade(trade)
   - repeats until the incoming order is filled or nothing more crosses
   - any unfilled remainder is inserted into the book (pool slot
     popped, node linked in, bitmap bit set if this is a new price level)

6. Server::on_trade() fires per fill:
   - records it in the rolling trade history
   - broadcasts a 27-byte binary trade frame to every connected client
   - queues a DB_SETTLE + DB_LOG_TRADE task for the batch DB worker

7. engine_thread_ broadcasts an updated `book` snapshot for APL to
   every client.

8. db_worker_ later drains its batch: one SQLite transaction credits
   the seller's cash, credits the buyer's shares, refunds any
   price-improvement difference to the buyer, and logs the trade row.
   Affected users then get a fresh `user_update` push.

9. React applies the binary trade frame (trade tape + price history)
   and the `book` message (depth tables, best bid/ask, spread) as they
   arrive, independently, whenever each one lands.
```

---

## 10. Concurrency model

Three long-lived threads, each with a narrow, explicit responsibility:

- **The `io_context` thread** (`ioc.run()` in `main.cpp`): every session's accept/read/write callback, and every `net::post`-scheduled write triggered from elsewhere. Single-threaded by construction, so no session-level data race is even possible.
- **`engine_thread_`**: the *only* thread that ever calls into an `IOrderBook`. No per-instrument mutex exists anymore - ownership is structural (only this thread has the call path), not lock-enforced. It also owns the engine-side metrics window (latency samples, order counts) with no locking needed for the same reason.
- **`db_worker_`**: the only thread that writes to SQLite. Reads that need immediate consistency with an in-flight batch (`get_user_profile`, `reserve_cash`, `reserve_shares`) go through the same `db_mutex_`, which the worker holds for an entire batch's transaction, not per-statement - so a concurrent read can never observe a half-applied trade.

The `SessionRegistry` has its own separate mutex protecting the live-session set, so `broadcast()` iterating it can't race with a client connecting or disconnecting mid-broadcast.

---

## 11. Build system and deployment

### CMake

`CMakeLists.txt` fetches GoogleTest, nlohmann/json, and Google Benchmark via `FetchContent` by default (`FETCH_GTEST`/`FETCH_JSON`/`FETCH_BENCHMARK`, all `ON`), with an `OFF` path for each that falls back to system packages - used in Docker, where there's no guarantee of internet access mid-build. Boost is located differently per platform: `Boost::headers` via a Homebrew prefix path on Apple Silicon, `Boost::system` via `find_package` elsewhere.

Three build targets: `engine` (the server), `tests` (all three test files, one binary, registered with `ctest`), `benchmarks` (the Google Benchmark suite).

### Docker

`Dockerfile` builds on Ubuntu 22.04 with system-installed dependencies (`FETCH_GTEST=OFF -DFETCH_JSON=OFF`), producing a single image containing the compiled `engine` binary and its runtime libraries.

```bash
docker build -t exchange-engine .
docker run -d --restart=always -p 9001:9001 exchange-engine
```

`--restart=always` survives VM reboots and crash-restarts. It does **not** by itself persist the SQLite database across a `docker rm` - see [§13](#13-known-limitations-and-future-work).

### CI

GitHub Actions (`.github/workflows/ci.yml`) runs on every push to `main`: builds and `ctest`s the C++ side, and separately installs, tests, and builds the frontend.

### Azure deployment

Engine runs on an Azure VM (Ubuntu 24, B2s_v2, Southeast Asia) at a static IP, port 9001 open inbound. The frontend is not deployed anywhere; each user runs it locally against the deployed engine.

---

## 12. Testing strategy

22 Google Test cases across three files, all built into one `tests` binary (`ctest` target `EngineTests`).

**`tests/test_orderbook.cpp`** - `OrderBook` in isolation: full/partial fills, no-match, time priority, resting-order snapshots, self-trade blocking, cancellation (including refunding only the unfilled remainder of a partially-filled order), and pool-exhaustion rejection.

**`tests/test_orderbook_legacy_parity.cpp`** - `OrderBook` vs `OrderBookLegacy` given identical input: matching trades, identical time-priority resolution, identical self-trade rejection, identical post-cancellation state.

**`tests/test_database.cpp`** - wallet/settlement correctness against a fresh in-memory SQLite instance each time: registration and login, default balances, insufficient-funds/insufficient-shares rejection, exact-amount refunds, buyer/seller settlement, price-improvement refunds, leaderboard ordering.

The engine tests deliberately never touch WebSocket, JSON, or threads - if a bug shows up in the server or frontend, the engine tests still passing tells you the matching logic itself is not where the bug is.

The frontend has a single smoke test (`App.test.js`) confirming the dashboard renders; validation logic, reconnect/backoff, and the per-company order-merging described in [§8](#8-the-react-frontend) have no dedicated coverage yet.

---

## 13. Known limitations and future work

**No TLS.** Every message, including login/register credentials, travels over `ws://`, not `wss://`. Password storage is solid (libsodium `crypto_pwhash`, Argon2id) but that only protects the database, not the wire. Acceptable for a demo account; not for anything holding real value.

**No rate limiting.** Login attempts and order submission are both unbounded. Not a concern for the matching engine's own design goals, but worth knowing given the engine sits on a public IP.

**Adding a company requires a recompile.** `MarketState`'s instrument list is fixed at construction from a hard-coded vector in `main.cpp`. A production system would load the company catalog from config or a database table instead.

**No persistent volume in the documented deploy flow.** The Azure redeploy sequence stops and removes the old container before starting a new one, with no `-v` volume mount for the SQLite file - so the database does not currently survive a redeploy on its own. Mounting a named volume (and pointing `Database`'s constructor at a path inside it) would fix this without any other change.
