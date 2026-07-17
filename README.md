# O(1) Exchange - Matching Engine

A high-performance, in-memory limit order book and matching engine built in C++, with a real-time React trading dashboard, multi-instrument market support, user accounts/wallets, and a live CURRENT-vs-LEGACY performance benchmark. Built for NUS Orbital 2026 (Apollo level).

---

## What it does

* Hosts multiple independent order books - one per listed company (APL, BLZ, CRN)
* Accepts limit **and** market buy/sell orders routed by company
* Matches orders on strict **price/time (FIFO) priority**, with self-trade prevention
* Emits trade events on every fill, broadcast to all connected clients via WebSocket
* User accounts with cash wallets and share portfolios (libsodium password hashing, session tokens)
* Order cancellation with ownership verification and full refund of the unfilled remainder
* Serves a live React dashboard: order book depth, trade tape, price chart (persisted across restarts), leaderboard, my-open-orders, my-trade-history, and fill-notification toasts
* A built-in stress-test UI that fires bursts of synthetic orders and can switch the whole market between the O(1) bitmap engine (`CURRENT`) and a `std::map`-based baseline (`LEGACY`) at runtime, with live throughput/latency charts

---

## Architecture

```
frontend/src/App.js         React trading UI (WebSocket client)
frontend/src/StressTest.js  Stress-test UI + CURRENT/LEGACY engine toggle
frontend/src/PerformanceGraphs.js  Live throughput/latency sparklines
        │
        │  ws://host:9001 (JSON control messages + binary trade frames)
        │
include/Server.h            WebSocket server (Boost.Beast), single-writer engine_thread_,
                             lock-free SPSC task queue, periodic metrics broadcast
        │
include/Market.h            Multi-instrument market state
        │  MarketState → one InstrumentState per company
        │  InstrumentState = { Company metadata + unique_ptr<IOrderBook> book }
        │
include/IOrderBook.h        Abstract matching-engine interface (Order, Trade, ...)
include/OrderBook.h         CURRENT engine - O(1) hierarchical bitmap + pre-allocated pool
include/OrderBookLegacy.h   LEGACY engine - std::map + std::list baseline, for comparison
        │
include/Database.h          SQLite (WAL) - users, wallets, portfolios, trades, sessions
        │
src/main.cpp                Bootstraps market with 3 companies, starts server
tests/test_orderbook.cpp    Google Test suite
benchmark/benchmark_orderbook.cpp  Google Benchmark: OrderBook vs OrderBookLegacy
```

---

## Core Engine (`include/OrderBook.h`, `include/OrderBookLegacy.h`)

### Data structures (`include/IOrderBook.h`)

```cpp
struct Order {
    uint64_t id;
    int64_t  user_id;
    uint64_t timestamp;
    uint32_t price;      // cents
    uint32_t quantity;
    uint16_t company_id;
    bool     side;       // true = BUY, false = SELL
};

struct Trade {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    int64_t  buyer_user_id;
    int64_t  seller_user_id;
    uint32_t price;      // cents
    uint32_t quantity;
    uint32_t buyer_limit_price;
    uint16_t company_id;
};
```

Both structs (plus `ITradeListener`) live in `IOrderBook.h` so `OrderBook` and `OrderBookLegacy` share one vocabulary. Prices are integer cents throughout the matching path - dollar formatting only happens at the WebSocket/UI boundary.

### `OrderBook` internals (the `CURRENT` engine)

```cpp
class OrderBook : public IOrderBook {
    std::vector<OrderNode> node_pool_;      // pre-allocated, 64-byte aligned
    OrderList bids_[MAX_PRICE];
    OrderList asks_[MAX_PRICE];

    // O(1) 3-level bitmaps for price discovery
    uint64_t bids_l1_[BITMAP_L1_SIZE], bids_l2_[BITMAP_L2_SIZE], bids_l3_;
    uint64_t asks_l1_[BITMAP_L1_SIZE], asks_l2_[BITMAP_L2_SIZE], asks_l3_;
};
```

**Price priority** is enforced by the 3-level bitmap, giving O(1) retrieval of the best price level via `__builtin_clzll`/`__builtin_ctzll`. **Time priority** is enforced by a doubly-linked list at each price level. `OrderNode` is packed into exactly 64 bytes to fit one cache line and avoid false sharing.

### `OrderBookLegacy` internals (the `LEGACY` baseline)

```cpp
class OrderBookLegacy : public IOrderBook {
    std::map<uint32_t, PriceLevel, std::greater<uint32_t>> bids_; // highest first
    std::map<uint32_t, PriceLevel, std::less<uint32_t>>    asks_; // lowest first
    // PriceLevel = { std::list<Order> orders; uint32_t total_quantity; }
};
```

Deliberately naive: an O(log N) map lookup per price level, heap-allocated list nodes, no pool. It exists purely as a real baseline to benchmark `OrderBook` against - same matching semantics (including self-trade prevention), verified to produce identical trade sequences for identical input.

### Matching algorithm

```
while (incoming has quantity remaining AND best_price crosses):
    best_price = get_best_ask()                 // O(1) via bitmap scan (CURRENT) / map begin() (LEGACY)
    if incoming.user_id == resting.user_id: reject remaining qty, stop  // self-trade prevention
    consume from head of asks_[best_price] list  // FIFO
    generate a Trade event, fire on_trade callback
    if list is empty: clear the price level
if incoming still has quantity: add to resting book
```

### Book snapshots

`bid_depth(limit)`/`ask_depth(limit)` return a `vector<IOrderBook::DepthLevel>` summarising up to `limit` price levels; `bid_orders(limit)`/`ask_orders(limit)` return individual resting orders (for the "My Open Orders" panel). Both back the WebSocket `book`/`snapshot` messages.

---

## Performance: CURRENT vs LEGACY

`benchmark/benchmark_orderbook.cpp` (Google Benchmark) runs both engines through identical input at 1K/10K/100K orders across 4 scenarios. Representative numbers from one run (`--benchmark_min_time=0.2s`, will vary by machine):

| Scenario (100K orders) | OrderBook (CURRENT) | OrderBookLegacy | Speedup |
|---|---|---|---|
| Book building (resting orders across many price levels) | 3.89 ms | 94.2 ms | ~24x |
| Cancellations | 2.49 ms | 75.6 ms | ~30x |
| Simple matching (one price level) | 0.91 ms | 2.84 ms | ~3x |
| Partial fills (one price level) | 0.70 ms | 0.54 ms | Legacy edges this one |

The gap widens specifically with the **number of distinct price levels** in play - exactly the architectural story: the bitmap avoids the O(log N) map lookup `OrderBookLegacy` pays per price level. When only one price level is involved (simple matching, partial fills), both engines are close to O(1) and the bitmap's bookkeeping overhead can even lose narrowly. Reproduce with:

```bash
cd build && ./benchmarks --benchmark_min_time=0.2s
```

Or drive it live from the browser: the **Stress Test** panel in the dashboard fires configurable order bursts and plots live throughput/p50/p99 latency for whichever engine is active.

---

## Multi-Instrument Market (`include/Market.h`)

The exchange is not one global order book. It is a **map of independent order books**, one per listed company.

```cpp
struct Company {
    uint16_t    id;
    std::string symbol;       // e.g. "APL"
    std::string name;         // e.g. "Apollo Technologies"
    uint64_t    total_shares;
};

struct InstrumentState {
    Company                    company;
    std::unique_ptr<IOrderBook> book;   // OrderBook or OrderBookLegacy
};

class MarketState {
    // one InstrumentState per company, direct-mapped array by company_id
    void set_engine_mode(EngineMode mode); // swaps every book, discards resting orders
};
```

**Routing:** every incoming order carries a `company_id`. The server does one lookup to find the right `InstrumentState`, then routes to `instrument->book->process_buy_order(...)`/`process_sell_order(...)`. A Crown Energy order never touches Apollo's book.

**Concurrency:** no per-instrument mutex. A single `engine_thread_` in `Server.h` is the *only* thing that ever touches `InstrumentState::book` - all other threads communicate with it exclusively through a lock-free SPSC queue.

### Listed companies (bootstrapped at startup)

|ID|Symbol|Name|Total Shares|
|-|-|-|-|
|1|APL|Apollo Technologies|1,000,000|
|2|BLZ|Blaze Manufacturing|2,500,000|
|3|CRN|Crown Energy|1,750,000|

---

## WebSocket Server (`include/Server.h`)

Built with **Boost.Beast**. Each connected client gets its own `Session`. A `SessionRegistry` (mutex-protected `std::set`) tracks all live sessions and handles broadcast. Trade fills go out as compact 27-byte binary frames; everything else is JSON.

### Message protocol (client → server)

| Type | Purpose |
|---|---|
| `register` / `login` | Account creation / session-token auth |
| `order` | Submit a limit order. Add `"is_market": true`-equivalent via `market_order` type for market orders. Add `"stress": true, "synthetic_user_id": -N` to bypass wallet reservation for load testing. |
| `market_order` | Submit a market order (fills at best available price) |
| `cancel` | Cancel a resting order (ownership-verified) |
| `snapshot` | Request full book + price history + engine mode for an instrument |
| `my_trades` | Request this user's trade history |
| `leaderboard` | Request the cash-ranked leaderboard |
| `switch_engine` | `{"mode":"CURRENT"\|"LEGACY"}` - swap the whole market's engine |

### Message protocol (server → client)

| Type | Purpose |
|---|---|
| `trade` (binary) | A fill occurred - company_id, price, quantity, order ids |
| `book` | Full bid/ask depth + open orders for an instrument, includes `engine_mode` |
| `snapshot` | Same as `book` plus `companies`, recent trade history, and persisted `price_history` |
| `user_update` | Updated cash/portfolio after settlement |
| `leaderboard` | Ranked list of users by cash |
| `my_trades` | This user's trade history |
| `engine_mode` | Broadcast after a `switch_engine` completes |
| `metrics` | Every ~500ms: `engine_mode`, `orders_processed`, `throughput_ops_sec`, `latency_p50_us`, `latency_p99_us` - timed around the matching call only, not WS/DB overhead |
| `error` | Rejection reason (insufficient funds, invalid message, self-trade prevention, ...) |

---

## React Frontend (`frontend/src/`)

- **`App.js`** - Single-page trading dashboard: auth flow, order ticket (limit + market), order book depth ladder, price chart (seeded from persisted history, then live), trade tape, leaderboard, my-open-orders, my-trade-history, and fill-notification toasts.
- **`StressTest.js`** - Engine toggle (`CURRENT`/`LEGACY`), order-count presets, rate-limited synthetic order generator, live submit/elapsed/send-rate stats.
- **`PerformanceGraphs.js`** - SVG sparklines for throughput and p50/p99 match latency, driven by the `metrics` broadcast stream; embedded in `StressTest.js`.

Switching the company dropdown sends a `snapshot` request for that `company_id`. Order submission and cancellation both include `company_id` in the payload.

---

## Demo Credentials

The exchange is pre-populated with two registered users for immediate testing:

|Username|Password|Starting Cash|Starting Portfolio|
|-|-|-|-|
|`shrey`|`pass123`|$10,000.00|500 APL shares|
|`akshay`|`pass123`|$10,000.00|None|

---

## Prerequisites

**Linux / WSL:**

```bash
sudo apt-get install -y build-essential cmake libboost-all-dev libsqlite3-dev libsodium-dev
```

**Mac (Apple Silicon):**

```bash
brew install cmake boost sqlite libsodium
```

**Node.js (for the React frontend):**

Linux / WSL:

```bash
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt-get install -y nodejs
```

Mac:

```bash
brew install node
```

Then install frontend dependencies:

```bash
cd frontend
npm install
```

---

## Build

```bash
mkdir build && cd build
cmake ..
make
./engine         # starts WebSocket server on port 9001
./tests          # runs the Google Test suite
./benchmarks     # runs the OrderBook vs OrderBookLegacy benchmark suite
```

**Docker:**

```bash
docker build -t exchange-engine .
docker run -d -p 9001:9001 exchange-engine
```

---

## Testing

22 Google Test cases across 3 files, all built into one `tests` binary (`ctest` target `EngineTests`):

**`tests/test_orderbook.cpp`** - `OrderBook` unit tests:

|Test|What it verifies|
|-|-|
|`FullFill`|Buy and sell at same price, exact quantities - one trade, book empty|
|`PartialFill`|Buyer wants more than available - partial trade, remainder rests|
|`NoMatch`|Prices don't cross - both orders rest, no trade|
|`TimePriority`|Two sells at same price - earlier order fills first|
|`RestingOrderSnapshotsPreservePriorityAndQuantity`|Depth/order snapshots reflect correct price-time ordering and remaining quantity|
|`SelfTradeBlocked`|A user can't trade against their own resting order|
|`CancelRemovesOrder`|Cancellation removes the order and returns its quantity|
|`CancelAfterPartialFillRefundsRemainderOnly`|Cancelling a partially-filled order returns only the unfilled remainder, not the original quantity|
|`PoolExhaustionRejectsFurtherOrders`|Once the pre-allocated pool (`MAX_ORDERS`) is full, `can_process_order` rejects further orders instead of silently dropping them|

**`tests/test_orderbook_legacy_parity.cpp`** - `OrderBook` vs `OrderBookLegacy`, identical input sequences:

|Test|What it verifies|
|-|-|
|`BasicCrossingAndPartialFillsMatch`|Both engines produce identical trades and resting-book state|
|`TimePriorityMatches`|Both engines resolve FIFO priority identically|
|`SelfTradePreventionMatches`|Both engines reject self-trades identically|
|`CancellationMatches`|Both engines produce identical post-cancellation book state|

**`tests/test_database.cpp`** - `Database` wallet/settlement tests, each against a fresh `:memory:` SQLite instance:

|Test|What it verifies|
|-|-|
|`CreateUserAndLogin`|Registration, duplicate-username rejection, login, session validation|
|`NewUserStartsWithDefaultCashAndNoShares`|Default $10,000 balance, empty portfolio|
|`ReserveCashRejectsInsufficientFunds`|Over-reservation is rejected and leaves the balance untouched|
|`ReserveSharesRejectsWhenUserHoldsNone`|Can't reserve shares you don't own|
|`ReleaseCashRefundsExactAmountReserved`|Refund restores exactly what was reserved|
|`ReleaseSharesRefundsExactQuantityReserved`|Same, for share reservations|
|`SettleTradeCreditsBuyerSharesAndSellerCash`|Settlement moves shares to buyer, cash to seller|
|`SettleTradeRefundsBuyerOnPriceImprovement`|Buyer gets refunded the difference when filled better than their limit|
|`LeaderboardOrdersByCashDescending`|Leaderboard ranks strictly by cash, highest first|

CI/CD via GitHub Actions runs the full test suite (and builds the `benchmarks` target) on every push to `main`.

---

## Deployment

Deployed on **Azure VM** (Ubuntu 24, B2s_v2, Southeast Asia) via Docker.

```bash
# on the Azure VM
git pull origin main
sudo docker build -t exchange-engine .
sudo docker stop $(sudo docker ps -q)
sudo docker rm $(sudo docker ps -aq)
sudo docker run -d --restart=always -p 9001:9001 exchange-engine
```

Engine accessible at `ws://20.205.25.160:9001`.

---

## Repository structure

```
exchange-engine/
├── include/
│   ├── IOrderBook.h      abstract matching-engine interface - Order, Trade, ITradeListener
│   ├── OrderBook.h       CURRENT engine - O(1) bitmap + pre-allocated pool
│   ├── OrderBookLegacy.h LEGACY engine - std::map baseline, for comparison
│   ├── Market.h          multi-instrument market - Company, InstrumentState, MarketState
│   ├── Server.h          WebSocket server - Session, SessionRegistry, Server, metrics
│   └── Database.h        SQLite (WAL) - users, wallets, portfolios, trades, leaderboard
├── src/
│   ├── main.cpp          bootstraps market with 3 companies, starts server
│   └── server.cpp        CMake compilation unit
├── tests/
│   └── test_orderbook.cpp  Google Test suite
├── benchmark/
│   └── benchmark_orderbook.cpp  Google Benchmark: OrderBook vs OrderBookLegacy
├── frontend/
│   └── src/
│       ├── App.js               React trading dashboard
│       ├── StressTest.js        stress-test UI + engine toggle
│       └── PerformanceGraphs.js live throughput/latency sparklines
├── CMakeLists.txt        cross-platform build (Mac + Linux/WSL)
├── Dockerfile            containerised build for deployment
├── .dockerignore
└── .github/
    └── workflows/
        └── ci.yml        GitHub Actions CI/CD pipeline
```
