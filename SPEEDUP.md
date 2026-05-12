# Recent Commits Combined Detailed Summary

This document combines and verifies the three recent Shrey-authored commits:

- `5bb424a` - `perf(engine): implement O(1) matching engine via hierarchical bitmaps`
- `777473a` - `perf: order structs are now preallocated on the heap and are not allocated during runtime, websocket optimisations, SQL optimisations`
- `b391f40` - `fix: some final fixes to the previous commit`

Together, these three commits form one coherent progression:

1. Replace the old order-book data structures with a specialized bitmap-indexed engine.
2. Remove major runtime overhead around allocation, socket I/O, and SQLite calls.
3. Fix startup, build, and test integration fallout from the larger refactor.

## High-Level Story

Before this sequence, the system used more conventional dynamic containers and a more blocking server/database interaction style.

After this sequence:

- the order book is price-indexed by integer cents
- active price levels are tracked by hierarchical bitmaps
- resting orders are stored in intrusive per-price queues
- arbitrary cancellation is O(1)
- resting order nodes are preallocated and recycled from a fixed pool
- WebSocket sessions are asynchronous instead of thread-per-connection blocking loops
- trade settlements are queued and batched to SQLite in a background worker
- the frontend and tests are aligned to the new integer-cent API

That is a substantial shift in architecture, not a local optimization.

## Commit 1: `5bb424a` - Engine Rewrite

This commit introduces the new matching-engine shape.

### What changed

#### `include/OrderBook.h`

The old book design based on ordered STL maps and queue-like price levels is replaced by:

- `OrderList bids_[MAX_PRICE]`
- `OrderList asks_[MAX_PRICE]`
- a three-level bitmap index per side
- explicit `OrderNode` and `OrderList` structs
- an `order_map_` for direct order-id lookup

Key model changes:

- prices move from `double` to `uint32_t` cents
- `company_name` is removed from `Order` and `Trade`
- side becomes a compact `bool`

Key algorithm changes:

- best bid is found by scanning the highest set bid bits
- best ask is found by scanning the lowest set ask bits
- order matching operates directly on intrusive linked lists at each price
- cancellation unlinks a node in O(1) and clears bitmap state when a level becomes empty
- depth and snapshot APIs are rebuilt on top of bitmap traversal plus linked-list traversal

This is the core data-structure redesign.

#### `include/Server.h`

The server-side order handling is updated to match the new book API:

- prices are parsed as integer cents
- side becomes a boolean
- `trade_to_json()` no longer emits `company_name`
- buy reservation uses integer-cent price values
- order submission now checks a hard price ceiling of `100000` cents

This is the protocol adaptation layer that makes the new book usable from clients.

#### `frontend/src/App.js`

The frontend is updated to:

- send prices as integer cents
- display prices by dividing cents by 100
- enforce a UI max price of `999.99`
- stop expecting `trade.company_name`
- resolve company display text via `company_id` and the company list

Without this, the backend change would have broken both display and order submission.

#### `include/UserManager.h`

A new in-memory user/account helper is added with seeded demo users and simple account checks. It is not the main architectural focus of the commit and appears separate from the SQLite-backed persistence flow.

### Why this commit matters

This is the commit that turns the order book into a specialized low-latency structure:

- no tree traversal for best price
- no floating-point price logic in the core engine
- no queue rebuild for arbitrary cancellation

### What it does not do yet

Despite how performance-oriented it is, this commit still stores resting nodes through `std::unique_ptr<OrderNode>`, so it has not yet eliminated runtime node allocation.

## Commit 2: `777473a` - Runtime Overhead Reduction

This commit builds on the engine rewrite and removes several remaining performance bottlenecks.

### What changed

#### `include/OrderBook.h`

The node representation is reworked again:

- raw `prev` and `next` pointers become pool indices
- `head` and `tail` become `head_idx` and `tail_idx`
- a one-million-entry `node_pool_` is preallocated
- `free_indices_` becomes the reusable free-list stack
- `order_map_` now maps order id to pool index instead of owning a `unique_ptr`

This means:

- new resting orders are drawn from a preallocated pool
- filled or cancelled orders return their slots to the pool
- the order book stops allocating per order after startup

This is the real "preallocated order structs" change.

#### `include/Database.h`

The database layer is heavily optimized:

- a mutex is added for thread-safe shared DB access
- reusable prepared statements are introduced for all hot operations
- begin/commit helpers are added
- WAL and `synchronous=NORMAL` pragmas are enabled
- reserve and settlement flows are rewritten around prepared statements

This avoids repeated statement compilation and prepares the DB layer for the background worker.

#### `include/Server.h`

The server undergoes a large async/concurrency refactor:

- sessions move from blocking `run()` loops to `async_accept`, `async_read`, and queued `async_write`
- outgoing writes are serialized through `write_queue_`
- the accept loop becomes `do_accept()`
- a `DbTask` queue and `db_worker_` thread are added
- trade settlement is moved off the matching path into a background queue
- the worker drains queued settlements in batches under a single SQLite transaction

This is the commit that turns the engine from "fast data structures" into a more end-to-end low-latency architecture.

### Why this commit matters

Even with a fast order book, the system would still stall if it:

- allocated every resting order dynamically
- blocked matching on SQLite writes
- spun up blocking thread-per-client websocket sessions

This commit addresses all three.

### New tradeoffs introduced

- open-order capacity becomes fixed by `MAX_ORDERS`
- async websocket logic is more complex than blocking code
- DB access is safe but funneled through one mutex
- durability is delayed until the worker flushes a batch

## Commit 3: `b391f40` - Integration And Cleanup Fixes

This commit is smaller, but it is important because it repairs practical breakage caused by the earlier architectural shifts.

### What changed

#### `src/main.cpp`

This is the most important functional fix in the commit.

After `777473a`, the server is event-loop-driven. The old `server.run()` startup path no longer matched the server design. This commit switches `main()` to:

- print a startup log line
- call `ioc.run()`

That is the correct way to drive the async server callbacks introduced in the previous commit.

#### `tests/test_orderbook.cpp`

The tests are rewritten to match the new engine API:

- orders now use integer cents
- side is now boolean
- trade assertions no longer expect `company_name`
- direct inspection of `bids` and `asks` maps is removed
- tests now use `bid_depth()`, `ask_depth()`, `bid_orders()`, and `ask_orders()`
- cancellation tests are updated to the new signature and semantics

This is not just syntax churn. It reorients the tests around the new public interface and new data model.

#### `CMakeLists.txt`

Two build fixes are added:

- Homebrew include and link paths for `/opt/homebrew` and `/usr/local`
- `CMAKE_PREFIX_PATH` is appended to for Boost on Apple instead of overwritten

This helps macOS builds survive the dependency setup required by the broader refactor.

#### `.DS_Store`

The commit also adds `.DS_Store`, which appears to be accidental macOS metadata and not a meaningful source change.

### Why this commit matters

The previous two commits are architectural. This one is integrational:

- the server now starts correctly
- the tests now compile against the new API
- macOS dependency discovery is less brittle

## Biggest Net Changes Across All Three Commits

### 1. The matching engine is completely different

The sequence replaces:

- ordered maps
- queue-based price levels
- floating-point prices

with:

- fixed arrays indexed by cents
- bitmap-based active price discovery
- intrusive linked lists
- O(1) cancellation
- preallocated recyclable node storage

### 2. Price handling is standardized around integer cents

This propagates through:

- backend order structures
- trade structures
- WebSocket parsing
- frontend submission
- frontend rendering
- tests

That is a major correctness improvement because it removes floating-point ambiguity from the matching core.

### 3. Persistence moves off the matching hot path

Trade settlement no longer blocks the engine directly. Instead:

- trades are matched in memory
- history is recorded and broadcast immediately
- ownership is resolved
- DB tasks are queued
- a worker batches settlements into one transaction

This is one of the most meaningful end-to-end latency improvements in the sequence.

### 4. Server concurrency changes shape

The system moves away from:

- synchronous accept
- detached thread-per-session websocket handling

and toward:

- async accept
- async read
- async write with serialized queueing
- event-loop-driven lifecycle

This is why `main()` had to change in the third commit.

## Notes On Accuracy Relative To The Original Markdown Files

The original markdown summaries were broadly on the right track, but they mixed up some responsibilities between commits.

Most importantly:

- `5bb424a` introduces the O(1)-style array + bitmap engine, but not the preallocated order-node pool.
- `777473a` is where preallocation, async websocket flow, prepared statements, and background DB batching are actually introduced.
- `b391f40` is not only about `5bb424a` cleanup; it also fixes integration gaps created by the async server work in `777473a`.
- `b391f40` also includes an accidental `.DS_Store`.

## Final Assessment

These three commits together represent one large refactor delivered in stages:

1. `5bb424a` changes the core matching engine data structures and protocol assumptions.
2. `777473a` reduces runtime overhead across memory allocation, socket handling, and SQLite usage.
3. `b391f40` makes the new architecture buildable, runnable, and testable.

The overall effect is a shift from a more conventional educational exchange implementation to a much more specialized performance-oriented design with fixed bounds, explicit memory management, asynchronous networking, and batched persistence.

## Commit 4: `b5ae976` - Vectorization And Dedicated Engine Concurrency

This later Shrey commit continues the same performance arc, but it pushes even further on lookup costs and concurrency boundaries:

- `b5ae976046ec17f831d50e43c14b6c48cd45d6c8`
- `perf: hashmaps in OrderBook.h and Market.h replaced with vectors, optimisations made in Server.h to allow for better concurrency`

### What changed

#### `include/Market.h`

The market-level instrument registry stops using a hash map and becomes a direct-mapped vector indexed by company id.

Key changes:

- `Company::id` is narrowed from `int` to `uint16_t`
- `std::unordered_map<int, std::unique_ptr<InstrumentState>> instruments_` becomes `std::vector<std::unique_ptr<InstrumentState>> instruments_`
- the constructor computes the maximum company id, resizes the vector once, and stores each instrument directly at `instruments_[company.id]`
- `find_instrument()` becomes an indexed bounds-checked lookup instead of a hash-map lookup
- the per-instrument mutex is removed with the comment that the dedicated matching engine thread now has exclusive ownership

Why this matters:

- market instrument lookup becomes direct array indexing
- one more hash-based structure is removed from the hot path
- ownership of order-book mutation becomes more explicit: the engine thread, not arbitrary session threads, is now the writer

#### `include/OrderBook.h`

This commit replaces the remaining hash-map lookup inside the order book with a direct-mapped vector and also embeds user ownership directly into orders and trades.

Key structural changes:

- `Order` gains `int64_t user_id`
- `Trade` gains `buyer_user_id` and `seller_user_id`
- `std::unordered_map<uint64_t, uint32_t> order_map_` is replaced by `std::vector<uint32_t> order_map_array_`
- `order_map_array_` is sized once to `MAX_ORDERS` and used as a direct-mapped lookup table
- `Order` and `OrderNode` are explicitly packed
- `OrderNode` is padded so `sizeof(OrderNode) == 64`
- a `static_assert` enforces the 64-byte cache-line-sized node layout

The callback model also changes:

- `std::function<void(const Trade&)> on_trade` is removed
- a zero-allocation interface `ITradeListener` is introduced
- `OrderBook` now exposes `ITradeListener* trade_listener`

Why this matters:

- the external `order_id -> user_id` ownership map in the server is no longer needed because user ids travel with the order itself
- order lookup avoids hash-map overhead and rehashing concerns
- the 64-byte node target is an explicit cache-locality optimization
- the listener interface removes `std::function` overhead from the trade callback path

Important nuance:

- the direct-mapped order lookup uses `order_id % MAX_ORDERS`, so the implementation adds a collision check by verifying `node.order.id == order_id`
- that means lookup is faster, but it relies on bounded active-order count and careful id handling rather than true unbounded hashing

#### `include/Server.h`

This is the biggest concurrency change in the commit.

The earlier async session model is reworked so sessions no longer touch the order books directly. Instead, they enqueue engine work onto a dedicated engine thread.

Key changes:

- a lock-free `SPSCQueue<T, Size>` template is introduced
- `EngineTask` is added with `ORDER`, `CANCEL`, and `SNAPSHOT` task types
- `EngineQueue` is defined as `SPSCQueue<EngineTask, 65536>`
- `Session` no longer keeps `history_`, `order_user_map_`, or `order_map_mutex_`
- session handlers now enqueue order, cancel, and snapshot tasks instead of directly mutating the book or reading snapshots under locks
- `Server` now implements `ITradeListener`
- each instrument book sets `trade_listener = this`
- `Server` starts a dedicated `engine_thread_` running `run_engine_worker()`

The engine worker:

- pops tasks from the SPSC queue
- resolves the target instrument
- executes `book.add_order(...)`, `book.cancel_order(...)`, or snapshot generation centrally
- broadcasts updated books or sends session-specific snapshots from the engine side

This is a major architectural simplification because it creates a clear single-writer rule for the matching engine.

The DB path is also tightened further:

- DB tasks move from `std::queue<DbTask>` to double-buffered vectors
- `db_queue_front_` collects work while the engine is producing
- the DB worker swaps `db_queue_front_` with `db_queue_back_` in O(1)
- settlement then runs over the back buffer while the front buffer immediately becomes available for new trades

Why this matters:

- session threads stop contending on per-instrument mutexes because those mutexes are gone
- order-book mutation becomes serialized through one dedicated engine thread
- snapshot generation is also centralized with engine ownership
- DB handoff becomes cheaper because the worker uses vector swap instead of repeatedly popping queue nodes

#### `tests/test_orderbook.cpp`

The tests are updated to reflect the new order-book interface and richer trade model.

Key changes:

- a `MockListener` implementing `ITradeListener` replaces direct `book.on_trade = ...`
- orders now include `user_id`
- tests continue validating matching, time priority, snapshots, and cancellation, but now through the listener interface

This confirms that the API has shifted again:

- trade callbacks are now interface-based
- user ownership is part of the order/trade flow, not external server bookkeeping

### Why this commit matters

This commit removes two more important sources of overhead:

- hash-based lookup in `MarketState`
- hash-based lookup for `order_id -> node index` in `OrderBook`

It also sharpens the concurrency model:

- sessions become producers of engine tasks
- one dedicated engine thread becomes the exclusive consumer and book mutator
- the DB worker consumes completed trade settlements via double-buffered vectors

That is a cleaner and more performance-oriented separation than the previous version, where async sessions still directly interacted with instrument state.

### New tradeoffs introduced

- direct-mapped vector lookup is faster, but it is less general than a true hash map and depends on bounded ids/capacity assumptions
- a spin/yield push loop on the engine queue can waste CPU under sustained backpressure
- the `order_id % MAX_ORDERS` scheme needs collision protection, which the commit adds via explicit id verification
- removing per-instrument mutexes is only safe because the dedicated engine thread now owns mutation

### Net effect relative to the earlier three commits

If the earlier three commits:

1. introduced the specialized bitmap-based engine
2. preallocated order nodes and batched DB work
3. fixed startup/build/test integration

then `b5ae976` goes one step further by:

4. replacing remaining hash-based hot-path lookups with vectors
5. embedding user ownership directly into the order/trade pipeline
6. moving all order-book mutation behind a dedicated engine thread with lock-free task handoff

This makes the system even more specialized and lower overhead, while also making the single-writer concurrency model much clearer.
