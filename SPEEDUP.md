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
