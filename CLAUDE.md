# CLAUDE.md

This file provides guidance to AI coding assistants working with code in this repository.

## Build & Run

**Backend (C++):**
```bash
mkdir -p build && cd build
cmake ..
make
./engine          # WebSocket server on port 9001
```

**Run tests:**
```bash
cd build && ctest --output-on-failure
# or directly:
./build/tests
```

**Run benchmarks (OrderBook vs OrderBookLegacy):**
```bash
cd build && ./benchmarks --benchmark_min_time=0.2s
# JSON output: ./benchmarks --benchmark_out=benchmark_results.json --benchmark_out_format=json
```

**Frontend (React):**
```bash
cd frontend
npm install       # first time only
npm start         # dev server on port 3000
npm test -- --watchAll=false
npm run build
```

**Docker:**
```bash
docker build -t exchange-engine .
docker run -d -p 9001:9001 exchange-engine
```

## Architecture

Three layers communicate one-way: React frontend → WebSocket server → matching engine.

**C++ Backend** (`include/`, `src/`):
- `include/IOrderBook.h` - Abstract matching-engine interface: `Order`, `Trade`, `ITradeListener`, the `DepthLevel`/`OrderSnapshot` types, and the pure-virtual methods (`can_process_order`, `process_buy_order`/`process_sell_order`, `cancel_order`, `bid_depth`/`ask_depth`, `bid_orders`/`ask_orders`, `best_bid`/`best_ask`, `engine_name`). Both engines below implement it.
- `include/OrderBook.h` - The `CURRENT` engine. 3-level hierarchical bitmap (L1/L2/L3) for O(1) best-bid/ask lookup. Pre-allocated `node_pool_` (100k orders, `MAX_ORDERS`) for zero heap allocation on the hot path. `OrderNode` is padded to 64 bytes for cache-line alignment.
- `include/OrderBookLegacy.h` - The `LEGACY` baseline engine. `std::map` price levels + `std::list` FIFO queues per level, no bitmap, no pre-allocated pool - deliberately naive O(log N), used only so the stress-test UI and Google Benchmark suite have something real to compare `OrderBook` against. Verified to produce identical trade sequences to `OrderBook` for identical input.
- `include/Market.h` - `MarketState` holds one `InstrumentState` per company; each wraps a `std::unique_ptr<IOrderBook> book` (not a mutex - see Concurrency below). `MarketState::set_engine_mode(EngineMode)` re-initializes every instrument's book (discarding resting orders), which is how the whole market switches between `CURRENT`/`LEGACY`.
- `include/Server.h` - Boost.Beast WebSocket server. A single `engine_thread_` owns all order book mutations (single-writer) via a lock-free SPSC queue of `EngineTask`s (`ORDER`, `CANCEL`, `SNAPSHOT`, `SWITCH_ENGINE`). The same thread times every `process_buy_order`/`process_sell_order` call and broadcasts a `metrics` message (engine_mode, throughput, p50/p99 latency in µs) every 500ms. SQLite I/O happens on a separate `db_worker_` thread using double-buffered trade vectors; `db_.mutex()` is held for the whole batch so a concurrent read can never observe a partially-applied batch.
- `include/Database.h` - SQLite with WAL mode. Session tokens, user wallets/portfolios, trade history (`trades` table - also backs the persisted price chart via `get_recent_prices`), and leaderboard queries.
- `src/main.cpp` - Bootstraps 3 companies (APL, BLZ, CRN), starts server.
- `src/server.cpp` - CMake compilation unit for `Server.h`.
- `benchmark/benchmark_orderbook.cpp` - Google Benchmark suite comparing `OrderBook` vs `OrderBookLegacy` (simple matching, book building, partial fills, cancellations) at 1K/10K/100K orders with identical input sequences.

**Frontend** (`frontend/src/`):
- `App.js` - Entire dashboard shell. State-based router: `LOADING → LOGIN/REGISTER → DASHBOARD`. Connects to `ws://host:9001` (binary trade frames + JSON control messages). Handles order entry (limit + market), cancellation, price chart (seeded from `price_history` on snapshot, then live), trade tape, leaderboard, my-open-orders/my-trades panels, and fill-notification toasts. Session persisted in `localStorage`.
- `StressTest.js` - Fires configurable bursts of synthetic orders - tagged `stress:true` with a `synthetic_user_id` so the server skips wallet reservation/settlement and self-trade prevention doesn't constantly trip against one real account - and toggles the engine between `CURRENT`/`LEGACY` via a `switch_engine` message.
- `PerformanceGraphs.js` - Live sparklines (throughput, p50/p99 latency) driven by the backend's `metrics` broadcast stream; embedded inside `StressTest.js`.

## Key Design Details

**Price units:** Internal matching uses **cents** (integers). Dollar ↔ cent conversion happens in the server/database layer (`price / 100.0`).

**Dual engine:** `IOrderBook` interface allows runtime switching between `OrderBook` (O(1) bitmap) and `OrderBookLegacy` (`std::map`). A `{"type":"switch_engine","mode":"CURRENT"|"LEGACY"}` message enqueues a `SWITCH_ENGINE` task; the engine thread applies it via `MarketState::set_engine_mode()` (safe without extra locking - the SPSC queue preserves order, so every previously-queued order/cancel has already landed on the old engine), then rebroadcasts every instrument's book plus an `engine_mode` notice.

**Ghost order prevention:** `add_order` works on a mutable copy; `match_buy`/`match_sell` update remaining quantity by reference so a fully-filled order is never inserted into the resting book.

**Concurrency:** No per-instrument mutex. `InstrumentState::book` is owned exclusively by the single `engine_thread_`; nothing else touches it directly - WebSocket sessions only ever talk to the engine via the SPSC queue.

**Dependencies:** Boost 1.74+ (Beast + Asio), nlohmann/json (fetched by CMake), GoogleTest (fetched by CMake), Google Benchmark (fetched by CMake), SQLite3, libsodium.

## Demo Credentials

| Username | Password | Starting Cash | Portfolio |
|----------|----------|---------------|-----------|
| `shrey`  | `pass123` | $10,000       | 500 APL   |
| `akshay` | `pass123` | $10,000       | None      |

## Orbital Docs

- `MS1_Project_Log.md` - Milestone 1 project log (NUS Orbital submission). Separate tables for Akshay (76h) and Shrey (76.5h) covering Apr 28 – Jun 1. Includes all coding, research/learning, poster, and video work. Format matches the NUS Orbital log sheet (Task / Date Range / Time Taken). Copy into a GDoc for submission.
- `MS2_Project_Log.md` - Milestone 2 project log, same format, covering Jun 1 – Jun 28 (market orders, trade history, price chart, leaderboard, fill notifications).
- `milestone1.md` - Milestone 1 ideation document: problem motivation, user stories, full technical design, architecture diagrams, and feature spec.

## Commit Messages

Do not mention AI tool usage in commit messages for this repo (no tool names, no "Generated with"/"Co-Authored-By" AI trailers, no references to AI-assisted edits). Describe the change itself, matching the existing `type(scope): summary` style used in the history.

## Deployment

Azure VM (`ws://20.205.25.160:9001`). Re-deploy:
```bash
git pull origin main
sudo docker build -t exchange-engine .
sudo docker stop $(sudo docker ps -q) && sudo docker rm $(sudo docker ps -aq)
sudo docker run -d --restart=always -p 9001:9001 exchange-engine
```
