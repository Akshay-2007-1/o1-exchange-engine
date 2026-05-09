# O(1) Exchange — Architecture

This document explains how the exchange works end to end: the data structures, the matching algorithm, the multi-instrument design, the WebSocket protocol, and how the React frontend fits in.

---

## Table of Contents

1. [System overview](#1-system-overview)
2. [The matching engine](#2-the-matching-engine)
3. [Price/time priority — how and why](#3-pricetime-priority--how-and-why)
4. [Multi-instrument market design](#4-multi-instrument-market-design)
5. [The WebSocket server](#5-the-websocket-server)
6. [The JSON protocol](#6-the-json-protocol)
7. [The React frontend](#7-the-react-frontend)
8. [Data flow — a full order lifecycle](#8-data-flow--a-full-order-lifecycle)
9. [Concurrency model](#9-concurrency-model)
10. [Build system and deployment](#10-build-system-and-deployment)
11. [Testing strategy](#11-testing-strategy)
12. [Known limitations and future work](#12-known-limitations-and-future-work)

---

## 1. System overview

The exchange has three layers that communicate in one direction only — the frontend talks to the server via WebSocket, and the server owns the engine. The engine never reaches out; it only reacts.

```
┌─────────────────────────────────────────────────────────────┐
│                     React Frontend                          │
│  (runs in browser, connects via WebSocket on mount)         │
└───────────────────────┬─────────────────────────────────────┘
                        │  ws://host:9001  (JSON messages)
                        │
┌───────────────────────▼─────────────────────────────────────┐
│                  WebSocket Server                           │
│  Boost.Beast + Boost.Asio                                   │
│  SessionRegistry — tracks all live clients                  │
│  routes messages to the right instrument                    │
└───────────────────────┬─────────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────────┐
│                    MarketState                              │
│  map<company_id, InstrumentState>                           │
│                                                             │
│   InstrumentState (APL)    InstrumentState (BLZ)  ...      │
│   ┌──────────────────┐     ┌──────────────────┐            │
│   │  Company info    │     │  Company info    │            │
│   │  OrderBook       │     │  OrderBook       │            │
│   │  mutex           │     │  mutex           │            │
│   └──────────────────┘     └──────────────────┘            │
└─────────────────────────────────────────────────────────────┘
```

Everything is in-memory. There is no database. The order book lives in RAM for the lifetime of the process. This is intentional — real matching engines work this way because RAM access is nanoseconds, disk access is microseconds. The tradeoff is that the book resets if the process restarts.

---

## 2. The matching engine

The engine lives entirely in `include/OrderBook.h`. It is a single class with two data members and one callback.

### The two sides

```cpp
// bids: sorted highest-first
// bids.begin() is always the best (highest) bid — O(1)
std::map<double, PriceLevel, std::greater<double>> bids;

// asks: sorted lowest-first
// asks.begin() is always the best (lowest) ask — O(1)
std::map<double, PriceLevel> asks;
```

`PriceLevel` is `std::queue<Order>`. Each price level holds a queue of every order resting at that price, in the order they arrived.

So the full structure looks like this:

```
bids
  $101.50 → [Order#3 (200 shares), Order#7 (100 shares)]   ← best bid
  $101.00 → [Order#1 (500 shares)]
  $100.50 → [Order#5 (300 shares)]

asks
  $102.00 → [Order#2 (150 shares)]                          ← best ask
  $102.50 → [Order#4 (400 shares)]
  $103.00 → [Order#6 (600 shares)]
```

### The callback

```cpp
std::function<void(const Trade&)> on_trade;
```

The engine itself knows nothing about WebSockets, JSON, or clients. When a match happens, it calls `on_trade` with a `Trade` struct. Whatever is listening on the other end (the server) decides what to do with it. This keeps the engine completely decoupled from the network layer.

### The matching loop

`add_order()` is the only public mutating function. It decides which side to match against and calls `match_buy` or `match_sell`.

```
match_buy(incoming):
    while incoming.quantity > 0 AND asks is not empty:
        best_ask = asks.begin()              // O(1)
        if best_ask.price > incoming.price:
            break                            // prices don't cross, stop
        fill from front of best_ask queue   // O(1), FIFO
        generate Trade, fire on_trade
        if best_ask queue is now empty:
            erase that price level from map // O(log n)
    if incoming still has quantity:
        add to bids[incoming.price] queue   // O(log n) first time, O(1) after
```

`match_sell` is the mirror image — it checks bids instead of asks, and the crossing condition is reversed.

---

## 3. Price/time priority — how and why

Every real exchange enforces two rules when deciding which order gets matched first.

**Rule 1 — Price priority:** the order offering the best price gets served first. For buyers, the highest bidder wins. For sellers, the lowest asker wins.

**Rule 2 — Time priority:** if two orders are at exactly the same price, the one that arrived first gets served first. This is FIFO.

These two rules map directly onto the data structures:

| Rule | Mechanism | Complexity |
|------|-----------|-----------|
| Price priority | `std::map` with `std::greater` for bids, default ascending for asks | `begin()` is O(1) |
| Time priority | `std::queue<Order>` at each price level, `front()` is earliest | `front()` is O(1) |

The result is that the entire hot path — check best price, get earliest order at that price — is O(1). This is why the project is called O(1) Exchange.

### Partial fills

If the incoming order quantity exceeds what is available at the best price, the engine fills as much as possible and continues to the next price level. This is called a partial fill. The remaining quantity of the incoming order keeps being matched until either it is fully filled or no more crossing prices exist. If quantity remains after matching, the order rests in the book.

---

## 4. Multi-instrument market design

### The problem with one global book

A single global `OrderBook` cannot support multiple companies correctly. A buy order for Apollo at $150 would match against a sell order for Crown at $150. That is nonsensical — they are different securities. Price only means something relative to a specific instrument.

A naive fix — filtering by company during matching — would destroy the O(1) property. You would have to skip over non-matching companies to find the best crossing price, turning the hot path into O(n).

### The correct solution

One completely independent `OrderBook` per company. The market is a map from company ID to instrument state.

```cpp
// include/Market.h

struct Company {
    uint32_t    id;
    std::string symbol;        // "APL", "BLZ", "CRN"
    std::string name;          // "Apollo Technologies"
    uint64_t    total_shares;
};

struct InstrumentState {
    Company   company;
    OrderBook book;
    std::mutex mutex;          // per-instrument, not global
};

class MarketState {
    std::map<uint32_t, InstrumentState> instruments;
};
```

When an order for Crown arrives:

```
company_id = 3
instrument = market.find_instrument(3)    // one map lookup, O(1)
instrument->book.add_order(order)         // matches only against Crown orders
```

Apollo's book is completely untouched. The matching complexity inside each book is unchanged — still O(1) for the hot path.

### Listed companies

Three companies are hard-coded at startup in `src/main.cpp`:

| ID | Symbol | Name | Total Shares |
|----|--------|------|-------------|
| 1 | APL | Apollo Technologies | 1,000,000 |
| 2 | BLZ | Blaze Manufacturing | 2,500,000 |
| 3 | CRN | Crown Energy | 1,750,000 |

This shall be changed to support dynamic admin view for each company and a method for new companies' addition in the future.

---

## 5. The WebSocket server

The server lives in `include/Server.h` and is built on Boost.Beast (WebSocket) and Boost.Asio (async I/O and TCP).

### Three classes

**`SessionRegistry`** — a `std::set<shared_ptr<Session>>` protected by a `std::mutex`. Tracks every live client connection. `broadcast(msg)` iterates the set and calls `send()` on each session. Adding and removing sessions is O(log n).

**`Session`** — one instance per connected client. Each session runs on its own `std::thread` (detached). It accepts the WebSocket handshake, registers itself in the registry, and enters a read loop. When the client disconnects for any reason (clean close, broken pipe, EOF, connection reset), the exception is caught, the read loop exits, and the session removes itself from the registry. The server does not crash on disconnection.

**`Server`** — owns the TCP acceptor. Runs a simple `while(true)` loop: accept a connection, create a `Session`, detach a thread for it, repeat. The accept loop itself catches exceptions so a bad handshake does not kill the server.

### On-connect behaviour

When a new client connects, the server immediately sends:
1. The current book snapshot for the default company (APL) for now
2. The full company list so the frontend can populate the dropdown

This means a client that connects mid-session sees the current state of the market, not a blank book which is very important.

---

## 6. The JSON protocol

All communication is plain JSON over the WebSocket connection. There is no binary encoding, no schema validation, no versioning. Messages are newline-terminated strings.

### Client → Server

**Submit an order:**
```json
{
  "type": "order",
  "company_id": 1,
  "id": 42,
  "side": "BUY",
  "price": 102.50,
  "quantity": 150,
  "timestamp": 1715000000000
}
```

**Cancel an order:**
```json
{
  "type": "cancel",
  "company_id": 1,
  "side": "SELL",
  "order_id": 42,
  "price": 102.00
}
```

**Request a book snapshot:**
```json
{
  "type": "snapshot",
  "company_id": 3
}
```

### Server → Client

**Trade event (broadcast to all clients):**
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

**Book snapshot:**
```json
{
  "type": "book",
  "company_id": 1,
  "company_name": "Apollo Technologies",
  "company_symbol": "APL",
  "total_shares": 1000000,
  "bids": [
    {"price": 101.50, "quantity": 200, "orders": 2},
    {"price": 101.00, "quantity": 500, "orders": 1}
  ],
  "asks": [
    {"price": 102.00, "quantity": 150, "orders": 1},
    {"price": 102.50, "quantity": 400, "orders": 1}
  ],
  "companies": [
    {"id": 1, "symbol": "APL", "name": "Apollo Technologies"},
    {"id": 2, "symbol": "BLZ", "name": "Blaze Manufacturing"},
    {"id": 3, "symbol": "CRN", "name": "Crown Energy"}
  ]
}
```

A book snapshot is sent after every order and after every cancellation so all clients always see the current state.

---

## 7. The React frontend

The frontend is a single-page React application in `frontend/src/App.js`. It has no backend of its own — it is purely a WebSocket client that renders state.

### State model

```
companies           []         — populated from first snapshot message
selectedCompanyId   number     — which instrument the user is viewing
bids                []         — resting buy orders for selected company
asks                []         — resting sell orders for selected company
trades              []         — last 20 executions across all instruments
connected           bool       — WebSocket connection status
```

### Two scopes

**Instrument-specific state** (changes when user switches company):
- order ticket (price, quantity, side inputs)
- bid/ask depth tables
- best bid, best ask, spread, bid volume, ask volume

**Global state** (never resets on company switch):
- trade tape — aggregates executions across all instruments

This distinction matters for the demo. Switching from APL to CRN changes the order book view but the trade tape keeps the full history of everything that executed on the exchange. This is also shown subtly on the frontend by making a clear distinction and grouping between the rest of the elements and the Trade tape alone!

### Depth table rendering

Each price level in the depth table has a coloured bar whose width is proportional to its quantity relative to the largest level:

```
width = (level.quantity / max_quantity) * 100%
```

This gives an immediate visual sense of where liquidity is concentrated without needing a separate chart.

### Reconnection

If the WebSocket closes unexpectedly, the frontend attempts to reconnect with exponential backoff:

```javascript
const delay = Math.min(1000 * 2 ** attempts, 30000);
```

This means: 1s, 2s, 4s, 8s ... capped at 30s. The user sees a DISCONNECTED badge and the UI reconnects automatically without a page refresh.

---

## 8. Data flow — a full order lifecycle

Here is what happens, in order, when a user submits a buy order for 150 shares of APL at $102.50.

```
1. User fills in the order ticket and clicks "Send BUY Order"

2. App.js constructs the JSON message:
   { type:"order", company_id:1, side:"BUY", price:102.50, quantity:150, ... }

3. WebSocket.send() transmits it to the server

4. Server receives the message in Session::handle_message()
   → parses JSON
   → reads company_id = 1
   → calls market_.find_instrument(1) → gets Apollo's InstrumentState
   → locks Apollo's mutex
   → calls instrument->book.add_order(order)

5. OrderBook::match_buy() runs:
   → checks asks.begin() — best ask is $102.00 (150 shares, Order#17)
   → $102.00 ≤ $102.50 → prices cross → match
   → fill_qty = min(150, 150) = 150
   → Trade { price:102.00, quantity:150, buy:#42, sell:#17 } created
   → on_trade(trade) fires

6. on_trade callback (wired in Server constructor):
   → records trade in history
   → constructs trade JSON
   → calls registry_.broadcast(trade_json)
   → ALL connected clients receive the trade event simultaneously

7. After add_order() returns:
   → server constructs book snapshot for APL
   → calls registry_.broadcast(snapshot_json)
   → all clients receive updated bids/asks

8. React receives trade message:
   → prepends to trades[] state (capped at 20)
   → TradeFeed re-renders

9. React receives book message:
   → updates bids[] and asks[] state
   → DepthTable re-renders
   → best bid/ask/spread recalculates via useMemo
   → market strip updates
```

Total round trip from click to UI update: sub-millisecond on local network.

---

## 9. Concurrency model

The server is multi-threaded. Each client session runs on its own detached thread. This means multiple clients can submit orders simultaneously.

### Potential race condition

If two clients submit orders to the same instrument at the same time, two threads will call `instrument->book.add_order()` concurrently. Without synchronisation, this is a data race — undefined behaviour.

### Solution: per-instrument mutex

Each `InstrumentState` has its own `std::mutex`. The server locks it before calling `add_order()` and unlocks after the snapshot is broadcast.

```cpp
std::lock_guard<std::mutex> lock(instrument->mutex);
instrument->book.add_order(order);
registry_.broadcast(instrument->book.snapshot().dump());
```

**Key property:** locking Apollo's mutex does not block Crown. Two clients trading different instruments proceed in parallel. Only clients trading the same instrument are serialised against each other — which is correct, because order matching must be atomic per instrument.

### SessionRegistry mutex

The registry has its own separate mutex protecting the session set. This ensures that `broadcast()` (which iterates the set) does not race with `add()` or `remove()` (which modify the set) when clients connect or disconnect mid-broadcast.

---

## 10. Build system and deployment

### CMake

`CMakeLists.txt` supports two environments:

**Local development (Mac/WSL):** fetches GoogleTest and nlohmann/json from GitHub via `FetchContent`. Detects Apple Silicon and sets Boost path accordingly.

**Docker (CI/deployment):** uses `FETCH_GTEST=OFF` and `FETCH_JSON=OFF` flags. CMake finds system-installed packages instead of downloading. This avoids needing internet access during the Docker build.

Cross-platform Boost detection:

```cmake
if(APPLE)
    set(CMAKE_PREFIX_PATH /opt/homebrew/opt/boost)
    find_package(Boost 1.74 REQUIRED)
    set(BOOST_LINK_LIB Boost::headers)
else()
    find_package(Boost 1.74 REQUIRED COMPONENTS system)
    set(BOOST_LINK_LIB Boost::system)
endif()
```

### Docker

The `Dockerfile` uses Ubuntu 22.04 as base, installs all dependencies via apt, copies source, and runs CMake with system packages. The final image contains only the compiled `engine` binary and its runtime libraries.

```
docker build -t exchange-engine .
docker run -d --restart=always -p 9001:9001 exchange-engine
```

`--restart=always` ensures the container survives VM reboots and crash-restarts automatically.

### CI/CD

GitHub Actions runs on every push to `main`:
1. Install dependencies (including `libboost-all-dev`)
2. `cmake -S . -B build`
3. `cmake --build build`
4. `ctest --test-dir build --output-on-failure`

Every commit in the repo history has a green or red badge proving whether tests passed at that point in time. This is part of th0e Apollo/Artemis-level software engineering evidence.

### Azure deployment

The engine runs on an Azure VM (Ubuntu 24, B2s_v2, Southeast Asia). Static public IP at `20.205.25.160`, port 9001 open via inbound port rule.

Redeployment after a C++ change:
```bash
ssh azureuser@20.205.25.160
cd ~/exchange-engine
git pull origin main
sudo docker build -t exchange-engine .
sudo docker stop $(sudo docker ps -q)
sudo docker rm $(sudo docker ps -aq)
sudo docker run -d --restart=always -p 9001:9001 exchange-engine
```

React changes do not require Azure redeployment — the frontend runs locally on each user's machine.

---

## 11. Testing strategy

Tests live in `tests/test_orderbook.cpp` and use Google Test.

| Test | Scenario | What it verifies |
|------|----------|-----------------|
| `FullFill` | Buy 150 @ $102, Sell 150 @ $102 | One trade fires, book is empty, quantities correct |
| `PartialFill` | Sell 100 @ $102, Buy 300 @ $102 | One trade of 100 fires, 200 shares rest as bid |
| `NoMatch` | Sell 100 @ $103, Buy 100 @ $102 | No trade fires, both orders rest in book |
| `TimePriority` | Two sells @ $102, one buy @ $102 | Earlier sell fills first, later sell stays resting |

The test suite validates the engine in complete isolation — no WebSocket, no JSON, no threads. Tests call `add_order()` directly and assert on trade callbacks and book state.

This isolation is intentional. If a bug appears in the server or frontend, the engine tests still pass. If an engine test fails, the bug is definitively in the matching logic.

---

## 12. Known limitations and future work

### Floating point prices

`std::map<double>` uses floating point equality for key comparison. This is technically unsafe — `102.10` represented as a double may not equal `102.10` submitted from JavaScript. For production use, prices should be stored as integers (e.g. price in cents or ticks). For Orbital scope this is acceptable.

### In-memory only

The order book resets on container restart. Adding SQLite persistence — writing every order to disk on arrival, replaying on startup — would make the system survive restarts. Estimated effort: one day.

### Single on_trade callback

`OrderBook::on_trade` is a single `std::function`. Assigning it in a new session overwrites the previous assignment. For correctness with multiple clients, this should be refactored to a `vector<std::function>` or the callback should be set once at startup (which is what the current server constructor does). Worth fixing before Splashdown.

### No order cancellation persistence

Cancelled orders are removed from the in-memory book but not logged. A trade history exists but a cancel history does not. Adding cancel logging would be straightforward.

### No authentication

Any WebSocket client can submit orders as any user. For a real exchange this is obviously unacceptable. Login auth is an important TO-DO.

### Multi-instrument extension

Adding a new company requires a code change in `main.cpp` and a recompile. A production system would load the company catalog from a config file or database. This is a trivial extension as stated earlier already!