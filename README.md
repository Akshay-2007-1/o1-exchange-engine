# O(1) Exchange — Matching Engine

A high-performance, in-memory limit order book and matching engine built in C++, with a real-time React trading dashboard and multi-instrument market support. Built for NUS Orbital 2026 (Artemis level).

\---

## What it does

* Hosts multiple independent order books — one per listed company (APL, BLZ, CRN)
* Accepts limit buy/sell orders routed by company
* Matches orders on strict **price/time (FIFO) priority**
* Emits trade events on every fill, broadcast to all connected clients via WebSocket
* Serves a live React dashboard with order book depth, trade tape, and market stats

\---

## Architecture

```
frontend/src/App.js         React trading UI (WebSocket client)
        │
        │  ws://host:9001 (JSON messages)
        │
include/Server.h            WebSocket server (Boost.Beast, multi-client)
        │
include/Market.h            Multi-instrument market state
        │  MarketState = map<company\_id, InstrumentState>
        │  InstrumentState = { Company metadata + OrderBook + mutex }
        │
include/OrderBook.h         Core matching engine (per instrument)
        │  bids: map<price, queue<Order>, descending>
        │  asks: map<price, queue<Order>, ascending>
        │
src/main.cpp                Bootstraps market with 3 companies, starts server
tests/test\_orderbook.cpp    Google Test suite
```

\---

## Core Engine (include/OrderBook.h)

### Data Structures

```cpp
enum class Side { BUY, SELL };

struct Order {
    uint64\_t id;
    Side     side;
    double   price;
    uint32\_t quantity;
    uint64\_t timestamp;
    uint32\_t company\_id;
    std::string company\_name;
};

struct Trade {
    uint64\_t buy\_order\_id;
    uint64\_t sell\_order\_id;
    double   price;
    uint32\_t quantity;
    uint32\_t company\_id;
    std::string company\_name;
};
```

### OrderBook internals

```cpp
class OrderBook {
    // bids sorted descending — bids.begin() is always the best (highest) bid — O(1)
    std::map<double, PriceLevel, std::greater<double>> bids;

    // asks sorted ascending — asks.begin() is always the best (lowest) ask — O(1)
    std::map<double, PriceLevel> asks;

    // PriceLevel = std::queue<Order> — FIFO gives time priority for free
};
```

**Price priority** is enforced by the map sort order. **Time priority** is enforced by the queue at each price level — the earliest order is always at `front()`.

### Matching algorithm

When a new order arrives, `add\_order()` calls `match\_buy()` or `match\_sell()`:

```
while (incoming has quantity remaining AND opposing side is not empty):
    best\_opposing = opposing\_side.begin()       // O(1)
    if prices don't cross: stop
    consume from front of that price level's queue  // O(1), FIFO
    generate a Trade event
    fire on\_trade callback
    if price level is empty: erase it
if incoming still has quantity: add to resting book
```

All hot-path operations are O(1). The only O(log n) operation is the map insertion when a new price level is created.

### Book snapshots

`bid\_depth(limit)` and `ask\_depth(limit)` return a `vector<DepthLevel>` summarising up to `limit` price levels, used to build the WebSocket `book` snapshot message.

\---

## Multi-Instrument Market (include/Market.h)

The exchange is not one global order book. It is a **map of independent order books**, one per listed company.

```cpp
struct Company {
    uint32\_t    id;
    std::string symbol;       // e.g. "APL"
    std::string name;         // e.g. "Apollo Technologies"
    uint64\_t    total\_shares;
};

struct InstrumentState {
    Company   company;
    OrderBook book;
    std::mutex mutex;         // per-instrument lock — instruments don't block each other
};

class MarketState {
    std::map<uint32\_t, InstrumentState> instruments;
};
```

**Routing:** every incoming order carries a `company\_id`. The server does one map lookup to find the right `InstrumentState`, then calls `instrument->book.add\_order(order)`. A Crown Energy order never touches Apollo's book.

**Locking:** each instrument has its own mutex. Matching on Apollo does not block a snapshot read on Crown.

### Listed companies (bootstrapped at startup)

|ID|Symbol|Name|Total Shares|
|-|-|-|-|
|1|APL|Apollo Technologies|1,000,000|
|2|BLZ|Blaze Manufacturing|2,500,000|
|3|CRN|Crown Energy|1,750,000|

\---

## WebSocket Server (include/Server.h)

Built with **Boost.Beast**. Each connected client gets its own thread (`std::thread(...).detach()`). A `SessionRegistry` (mutex-protected `std::set`) tracks all live sessions. Trade events and book snapshots are broadcast to all connected clients via `registry\_.broadcast()`.

### Message protocol

**Client → Server (order submission):**

```json
{
  "type": "order",
  "company\_id": 1,
  "id": 42,
  "side": "BUY",
  "price": 102.50,
  "quantity": 150,
  "timestamp": 1234567890
}
```

**Client → Server (snapshot request):**

```json
{ "type": "snapshot", "company\_id": 1 }
```

**Client → Server (cancel order):**

```json
{ "type": "cancel", "company\_id": 1, "order\_id": 42 }
```

**Server → All clients (trade event):**

```json
{
  "type": "trade",
  "company\_id": 1,
  "company\_name": "Apollo Technologies",
  "price": 102.50,
  "quantity": 150,
  "buy\_order\_id": 42,
  "sell\_order\_id": 17
}
```

**Server → Client (book snapshot):**

```json
{
  "type": "book",
  "company\_id": 1,
  "company\_name": "Apollo Technologies",
  "company\_symbol": "APL",
  "total\_shares": 1000000,
  "bids": \[{"price": 101.50, "quantity": 200, "orders": 1}],
  "asks": \[{"price": 102.00, "quantity": 150, "orders": 1}],
  "companies": \[
    {"id": 1, "symbol": "APL", "name": "Apollo Technologies"},
    {"id": 2, "symbol": "BLZ", "name": "Blaze Manufacturing"},
    {"id": 3, "symbol": "CRN", "name": "Crown Energy"}
  ]
}
```

\---

## React Frontend (frontend/src/App.js)

Single-page trading dashboard. Connects to the WebSocket server on mount. Handles three message types: `trade` (appends to trade tape), `book` (updates bid/ask depth tables), `history` (pre-populates trade tape on connect).

**Two scopes of state:**

* **Instrument-specific:** order ticket, bid/ask depth ladder, best bid/ask/spread for the selected company
* **Global:** trade tape aggregates executions across all instruments

Switching the company dropdown sends a `snapshot` request for that `company\_id`. Order submission and cancellation both include `company\_id` in the payload.

\---

## Demo Credentials

The exchange is pre-populated with two registered users for immediate testing:

|Username|Password|Starting Cash|Starting Portfolio|
|-|-|-|-|
|`shrey`|`pass123`|$10,000.00|500 APL shares|
|`akshay`|`pass123`|$10,000.00|None|

\---

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
curl -fsSL https://deb.nodesource.com/setup\_20.x | sudo -E bash -
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

\---

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

\---

## Testing

Google Test suite in `tests/test\_orderbook.cpp` covers:

|Test|What it verifies|
|-|-|
|`FullFill`|Buy and sell at same price, exact quantities — one trade, book empty|
|`PartialFill`|Buyer wants more than available — partial trade, remainder rests|
|`NoMatch`|Prices don't cross — both orders rest, no trade|
|`TimePriority`|Two sells at same price — earlier order fills first|

CI/CD via GitHub Actions runs the full test suite on every push to `main`.

\---

## Deployment

Deployed on **Azure VM** (Ubuntu 24, B2s\_v2, Southeast Asia) via Docker.

```bash
# on the Azure VM
git pull origin main
sudo docker build -t exchange-engine .
sudo docker stop $(sudo docker ps -q)
sudo docker rm $(sudo docker ps -aq)
sudo docker run -d --restart=always -p 9001:9001 exchange-engine
```

Engine accessible at `ws://20.205.25.160:9001`.

\---

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
│   └── test\_orderbook.cpp Google Test suite
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

