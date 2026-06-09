# O(1) Exchange — Matching Engine

A high-performance, in-memory limit order book and matching engine built in C++, with a real-time React trading dashboard and multi-instrument market support. Built for NUS Orbital 2026 (Apollo level).

---

## What it does

* Hosts multiple independent order books — one per listed company (APL, BLZ, CRN)
* Accepts limit buy/sell orders routed by company
* Matches orders on strict **price/time (FIFO) priority**
* Emits trade events on every fill, broadcast to all connected clients via WebSocket
* Serves a live React dashboard with order book depth, trade tape, and market stats

---

## Architecture

```
frontend/src/App.js         React trading UI (WebSocket client)
        │
        │  ws://host:9001 (JSON messages)
        │
include/Server.h            WebSocket server (Boost.Beast, multi-client)
        │
include/Market.h            Multi-instrument market state
        │  MarketState = map<company_id, InstrumentState>
        │  InstrumentState = { Company metadata + OrderBook + mutex }
        │
include/OrderBook.h         Core matching engine (per instrument)
        │  bids/asks: O(1) multi-level bitmaps + pre-allocated linked lists
        │
src/main.cpp                Bootstraps market with 3 companies, starts server
tests/test_orderbook.cpp    Google Test suite
```

---

## Core Engine (include/OrderBook.h)

### Data Structures

```cpp
enum class Side { BUY, SELL };

struct Order {
    uint64_t id;
    Side     side;
    double   price;
    uint32_t quantity;
    uint64_t timestamp;
    uint32_t company_id;
    std::string company_name;
};

struct Trade {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    double   price;
    uint32_t quantity;
    uint32_t company_id;
    std::string company_name;
};
```

### OrderBook internals

```cpp
class OrderBook {
    std::vector<OrderNode> node_pool_;
    OrderList bids_[MAX_PRICE];
    OrderList asks_[MAX_PRICE];

    // O(1) multi-level bitmaps for price discovery
    uint64_t bids_l1_[BITMAP_L1_SIZE], bids_l2_[BITMAP_L2_SIZE], bids_l3_;
    uint64_t asks_l1_[BITMAP_L1_SIZE], asks_l2_[BITMAP_L2_SIZE], asks_l3_;
};
```

**Price priority** is enforced by an O(1) multi-level bitmap allowing for instant retrieval of the best price level. **Time priority** is enforced by a linked list at each price level. `OrderNode` objects are pre-allocated and packed into exactly 64 bytes to perfectly fit cache lines and prevent false sharing.

### Matching algorithm

When a new order arrives, `add_order()` calls `match_buy()` or `match_sell()`:

```
while (incoming has quantity remaining AND best_price crosses):
    best_price = get_best_ask()                 // O(1) via hardware __builtin_ctzll on bitmaps
    consume from head of asks_[best_price] list // O(1), FIFO
    generate a Trade event
    fire on_trade callback
    if list is empty: clear_bit(best_price)     // O(1)
if incoming still has quantity: add to resting book
```

All hot-path operations are O(1). The only O(log n) operation is the map insertion when a new price level is created.

### Book snapshots

`bid_depth(limit)` and `ask_depth(limit)` return a `vector<DepthLevel>` summarising up to `limit` price levels, used to build the WebSocket `book` snapshot message.

---

## Multi-Instrument Market (include/Market.h)

The exchange is not one global order book. It is a **map of independent order books**, one per listed company.

```cpp
struct Company {
    uint32_t    id;
    std::string symbol;       // e.g. "APL"
    std::string name;         // e.g. "Apollo Technologies"
    uint64_t    total_shares;
};

struct InstrumentState {
    Company   company;
    OrderBook book;
    std::mutex mutex;         // per-instrument lock — instruments don't block each other
};

class MarketState {
    std::map<uint32_t, InstrumentState> instruments;
};
```

**Routing:** every incoming order carries a `company_id`. The server does one map lookup to find the right `InstrumentState`, then calls `instrument->book.add_order(order)`. A Crown Energy order never touches Apollo's book.

**Locking:** each instrument has its own mutex. Matching on Apollo does not block a snapshot read on Crown.

### Listed companies (bootstrapped at startup)

|ID|Symbol|Name|Total Shares|
|-|-|-|-|
|1|APL|Apollo Technologies|1,000,000|
|2|BLZ|Blaze Manufacturing|2,500,000|
|3|CRN|Crown Energy|1,750,000|

---

## WebSocket Server (include/Server.h)

Built with **Boost.Beast**. Each connected client gets its own thread (`std::thread(...).detach()`). A `SessionRegistry` (mutex-protected `std::set`) tracks all live sessions. Trade events and book snapshots are broadcast to all connected clients via `registry_.broadcast()`.

### Message protocol

**Client → Server (order submission):**

```json
{
  "type": "order",
  "company_id": 1,
  "id": 42,
  "side": "BUY",
  "price": 102.50,
  "quantity": 150,
  "timestamp": 1234567890
}
```

**Client → Server (snapshot request):**

```json
{ "type": "snapshot", "company_id": 1 }
```

**Client → Server (cancel order):**

```json
{ "type": "cancel", "company_id": 1, "order_id": 42 }
```

**Server → All clients (trade event):**

```json
{
  "type": "trade",
  "company_id": 1,
  "company_name": "Apollo Technologies",
  "price": 102.50,
  "quantity": 150,
  "buy_order_id": 42,
  "sell_order_id": 17
}
```

**Server → Client (book snapshot):**

```json
{
  "type": "book",
  "company_id": 1,
  "company_name": "Apollo Technologies",
  "company_symbol": "APL",
  "total_shares": 1000000,
  "bids": \[{"price": 101.50, "quantity": 200, "orders": 1}],
  "asks": \[{"price": 102.00, "quantity": 150, "orders": 1}],
  "companies": \[
    {"id": 1, "symbol": "APL", "name": "Apollo Technologies"},
    {"id": 2, "symbol": "BLZ", "name": "Blaze Manufacturing"},
    {"id": 3, "symbol": "CRN", "name": "Crown Energy"}
  ]
}
```

---

## React Frontend (frontend/src/App.js)

Single-page trading dashboard. Connects to the WebSocket server on mount. Handles three message types: `trade` (appends to trade tape), `book` (updates bid/ask depth tables), `history` (pre-populates trade tape on connect).

**Two scopes of state:**

* **Instrument-specific:** order ticket, bid/ask depth ladder, best bid/ask/spread for the selected company
* **Global:** trade tape aggregates executions across all instruments

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
mkdir build \&\& cd build
cmake ..
make
./engine         # starts WebSocket server on port 9001
./tests          # runs the Google Test suite
```

**Docker:**

```bash
docker build -t exchange-engine .
docker run -d -p 9001:9001 exchange-engine
```

---

## Testing

Google Test suite in `tests/test_orderbook.cpp` covers:

|Test|What it verifies|
|-|-|
|`FullFill`|Buy and sell at same price, exact quantities — one trade, book empty|
|`PartialFill`|Buyer wants more than available — partial trade, remainder rests|
|`NoMatch`|Prices don't cross — both orders rest, no trade|
|`TimePriority`|Two sells at same price — earlier order fills first|

CI/CD via GitHub Actions runs the full test suite on every push to `main`.

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
│   ├── OrderBook.h       core matching engine — Order, Trade, OrderBook
│   ├── Market.h          multi-instrument market — Company, InstrumentState, MarketState
│   └── Server.h          WebSocket server — Session, SessionRegistry, Server
├── src/
│   ├── main.cpp          bootstraps market with 3 companies, starts server
│   └── server.cpp        CMake compilation unit
├── tests/
│   └── test_orderbook.cpp Google Test suite
├── frontend/
│   └── src/
│       └── App.js        React trading dashboard
├── CMakeLists.txt        cross-platform build (Mac + Linux/WSL)
├── Dockerfile            containerised build for deployment
├── .dockerignore
└── .github/
    └── workflows/
        └── ci.yml        GitHub Actions CI/CD pipeline
```

