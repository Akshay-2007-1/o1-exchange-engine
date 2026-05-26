# PR: Add latency benchmarking, dual-engine comparison, and stress-test observability

## Title

Latency Test: add benchmark harness, legacy/current engine switching, metrics plumbing, and frontend stress-test dashboard

## Summary

This PR turns the exchange engine into a performance-comparison environment for order book experimentation.

The branch introduces:

- a shared `IOrderBook` abstraction so the system can swap matching-engine implementations without changing the higher-level server flow
- a `OrderBookLegacy` baseline implementation to compare against the current optimized engine
- engine metrics collection and periodic metrics broadcasting from the backend
- a Google Benchmark harness for repeatable native benchmarking
- a frontend stress-test dashboard that can switch between engine modes, fire synthetic order flow, and visualize performance over time
- supporting build and dependency changes required to compile benchmarks and render charts

In practical terms, this branch makes it possible to compare the current optimized order book against a legacy implementation using both:

- backend-native benchmarks for controlled repeatable performance testing
- UI-driven stress testing for end-to-end experimentation from the browser

## Why

The project already has a high-performance in-memory exchange engine, but before this branch there was no clean way to:

- benchmark engine behavior in isolation
- compare an optimized engine against a baseline implementation
- switch engine implementations at runtime from the app layer
- expose latency and throughput-oriented metrics to users
- visually inspect how the engine behaves during load generation

This PR addresses those gaps by adding both infrastructure and developer-facing tooling for latency/performance evaluation.

## Problem Being Solved

Before this PR:

- the server was tightly coupled to a single `OrderBook` implementation
- there was no explicit interface for alternative matching engine backends
- there was no legacy reference implementation preserved for apples-to-apples comparison
- performance testing relied on ad hoc observation rather than a benchmark harness
- the frontend did not offer a dedicated stress-test workflow or performance visualization

After this PR:

- the market can host either the current optimized engine or the legacy engine
- the active engine can be switched at runtime via WebSocket
- both engines can report metrics through a shared interface
- benchmark binaries can be built and run via CMake
- the frontend can submit sustained load and display performance panels/graphs

## High-Level Design

This PR introduces a layered comparison workflow:

1. `IOrderBook` abstracts common order book behavior.
2. `OrderBook` becomes the optimized implementation behind that interface.
3. `OrderBookLegacy` provides a baseline implementation using more traditional STL structures.
4. `MarketState` owns `unique_ptr<IOrderBook>` per instrument and can rebuild books when engine mode changes.
5. `Server` accepts a `set_engine_mode` message, routes orders through the selected implementation, and periodically broadcasts metrics.
6. The React frontend exposes engine selection, stress-test controls, comparison panels, and charts.
7. A Google Benchmark executable provides a more controlled native performance suite.

## Detailed Changes

### 1. Order book abstraction via `IOrderBook`

New file:

- `include/IOrderBook.h`

What changed:

- introduces a common interface for order submission, cancellation, depth queries, order snapshots, trade listener wiring, and metrics access
- promotes `DepthLevel` and `OrderSnapshot` into shared structs so both engine implementations can expose the same API shape

Why it matters:

- removes direct dependency on one concrete order book type
- makes engine substitution possible in `MarketState` and `Server`
- creates a cleaner seam for benchmarking and future experimentation

### 2. Metrics model for latency/performance analysis

New file:

- `include/Metrics.h`

What changed:

- adds `EngineMetrics` with counters for submitted orders and matched orders
- stores individual latencies
- provides P50 and P99 helpers
- establishes reset behavior and placeholders for throughput calculation

Why it matters:

- gives both backend engines a shared metrics contract
- enables periodic reporting to the UI
- provides the foundation for comparing current versus legacy behavior using common indicators

### 3. Optimized engine updated to implement the interface

Modified file:

- `include/OrderBook.h`

What changed:

- `OrderBook` now implements `IOrderBook`
- metrics hooks were added to track submitted orders and matched orders
- latency capture is performed when matches occur
- trade listener access is now routed through interface methods

Why it matters:

- preserves the optimized engine while making it pluggable
- ensures the current engine participates in the same observability path as the legacy engine
- allows upper layers to treat both engines uniformly

### 4. Legacy baseline engine added

New file:

- `include/OrderBookLegacy.h`

What changed:

- adds a baseline order book implementation intended to reflect an older / more conventional design
- uses STL maps and queues rather than the optimized structure used by the current engine
- implements the same `IOrderBook` interface and metrics hooks

Why it matters:

- provides a baseline for direct performance comparison
- preserves a slower but easier-to-reason-about implementation for benchmarking and validation
- gives benchmark runs and UI tests a second comparison target

### 5. Market layer refactor for dynamic engine ownership

Modified file:

- `include/Market.h`

What changed:

- `InstrumentState` now stores `std::unique_ptr<IOrderBook>` instead of an embedded concrete `OrderBook`
- adds `EngineMode` with `CURRENT` and `LEGACY`
- `MarketState` can initialize all instruments with either implementation
- `set_engine_mode()` rebuilds instrument order books using the selected mode
- separate metric stores are maintained for current and legacy engines

Why it matters:

- centralizes engine selection logic in one place
- lets the server switch the active engine without reworking the rest of the order-routing path
- isolates metrics by engine mode instead of mixing them together

### 6. Server integration for engine switching and metrics broadcasting

Modified file:

- `include/Server.h`

What changed:

- updates JSON serialization helpers to work with shared `DepthLevel` and `OrderSnapshot` types
- updates book snapshot serialization to dereference `instrument.book` through the interface pointer
- adds `metrics_to_json()` for outbound metrics payloads
- adds a new WebSocket message type: `set_engine_mode`
- starts a periodic timer to broadcast metrics for both current and legacy engines
- updates engine task handling to call methods on `IOrderBook`
- sends immediate user updates after order placement to keep UI balances responsive during stress runs

Why it matters:

- connects the backend comparison infrastructure to the frontend
- makes engine mode switching a first-class runtime capability
- enables near-real-time observability during stress tests

### 7. Benchmark build support

Modified file:

- `CMakeLists.txt`

What changed:

- adds `FETCH_BENCHMARK` option
- fetches Google Benchmark when enabled
- creates a new benchmark executable: `exchange_benchmark`

Why it matters:

- makes native benchmarks part of the project build instead of a side experiment
- lowers friction for reproducible performance testing in CI or local development

### 8. Native benchmark suite for order book comparison

New file:

- `benchmark/benchmark_orderbook.cpp`

What changed:

- adds Google Benchmark coverage for both current and legacy engines
- includes multiple benchmark scenarios:
  - simple matching
  - book building
  - partial fills
  - multi-instrument order flow
- standardizes random generation with a fixed seed

Why it matters:

- provides repeatable backend-centric measurements independent of frontend or WebSocket overhead
- allows faster iteration on engine internals
- makes it easier to justify optimizations with real numbers

### 9. Frontend stress-test entry point

Modified file:

- `frontend/src/App.js`

What changed:

- mounts the new `StressTest` component into the application
- resets local trade/order UI state when the stress-test component requests it

Why it matters:

- exposes the new latency-testing functionality from the main app
- keeps the stress-test workflow integrated rather than hidden behind a separate tool

### 10. Stress-test UI and comparison panels

New files:

- `frontend/src/StressTest.js`
- `frontend/src/StressTest.css`

What changed:

- adds engine-mode selection between current and legacy implementations
- adds controls for total order count and orders-per-second rate
- adds preset scale buttons for quick load selection
- adds start/stop test controls
- tracks submitted/matched orders and latency estimates on the frontend
- renders side-by-side metric panels for the two engines
- shows an overall speedup ratio when both modes have data

Why it matters:

- creates an end-to-end test surface for human experimentation
- enables visually comparing engine behavior from the browser
- helps product/demo workflows in addition to backend benchmarking workflows

### 11. Performance graphs for live visualization

New files:

- `frontend/src/PerformanceGraphs.js`
- `frontend/src/PerformanceGraphs.css`

What changed:

- adds charts for:
  - latency over time
  - throughput
  - memory usage
  - order processing status
- styles the visualization separately for current versus legacy engine modes
- provides a richer observability experience during stress runs

Why it matters:

- helps developers and reviewers interpret performance behavior faster than reading raw counters
- supports demos and exploratory testing
- provides a foundation for future charting against real backend-fed time series

### 12. Frontend dependency updates

Modified files:

- `frontend/package.json`
- `frontend/package-lock.json`

What changed:

- adds `recharts` for chart rendering

Why it matters:

- enables the new graph-based performance UI

### 13. Environment / workflow support changes

Modified files:

- `.gitignore`
- `include/Database.h`

What changed:

- ignores `node_modules/` and `plan.md`
- increases the default user cash balance from `10000.0` to `1000000.0`

Why it matters:

- the ignore updates reduce local noise from frontend dependencies and planning artifacts
- the higher default cash balance better supports large stress-test runs without quickly exhausting account funds

## Commit-by-Commit Narrative

This branch evolved in a logical sequence:

1. Introduced the interface and metrics foundation.
2. Added a legacy baseline implementation.
3. Refactored market ownership so the active engine could be swapped.
4. Integrated engine switching and metrics broadcasting into the server.
5. Added Google Benchmark support and a benchmark executable.
6. Added the frontend stress-test UI and charting support.
7. Increased default account balances and updated ignore rules to support the new workflow.

This progression keeps the architecture changes under the backend first, then layers tooling and UI on top.

## API / Protocol Changes

### New inbound WebSocket message

```json
{
  "type": "set_engine_mode",
  "mode": "current"
}
```

or

```json
{
  "type": "set_engine_mode",
  "mode": "legacy"
}
```

### New outbound message types / payload extensions

The backend now broadcasts metrics payloads shaped like:

```json
{
  "type": "metrics_update",
  "mode": "current",
  "orders_submitted": 0,
  "orders_matched": 0,
  "latency_p50_us": 0,
  "latency_p99_us": 0,
  "throughput_ops": 0.0,
  "resting_orders": 0,
  "total_market_value": 0
}
```

This is additive behavior intended to support the new UI and future analysis tooling.

## Expected Impact

### Developer impact

- easier performance benchmarking
- easier current-vs-legacy comparison
- cleaner architecture for future engine variants

### Product / demo impact

- new stress-test dashboard in the frontend
- more compelling demonstration of engine performance characteristics

### Runtime impact

- additional metrics collection and periodic broadcasting overhead
- ability to rebuild active books when switching engine mode
- added frontend bundle size from charting dependency

## Risks and Tradeoffs

### 1. Engine switching resets in-memory book state

`MarketState::set_engine_mode()` rebuilds instrument order books. That means switching modes replaces the active in-memory book instead of migrating open orders across implementations.

Why this is acceptable here:

- the feature is intended for benchmarking/stress testing
- a clean reset helps avoid cross-engine contamination during comparison

### 2. Metrics are early-stage rather than fully comprehensive

The metrics structure includes fields such as throughput, resting orders, and market value, but not every field is fully computed end to end in this branch.

Implication:

- the plumbing is in place
- some metric fields may still need refinement before being treated as authoritative production telemetry

### 3. Frontend charting is a new dependency

Adding `recharts` increases frontend dependency surface and bundle footprint.

### 4. Stress tests may generate heavy UI and WebSocket traffic

Large runs combine:

- high-frequency order submission
- snapshot refreshes
- trade events
- metrics broadcasts

This is useful for testing, but it may increase noise or resource use in local environments.

### 5. Default account balance semantics changed

Raising starting cash from `10,000` to `1,000,000` changes the economics of local test accounts. This is likely intentional for stress testing, but reviewers should be aware this is not a neutral config change.

## Testing

### What this PR enables

- native engine benchmarking via `exchange_benchmark`
- manual end-to-end stress testing through the React UI
- runtime switching between current and legacy engine modes

### Suggested verification plan

1. Build the backend and confirm the benchmark target compiles.
2. Run the existing unit tests to confirm baseline correctness still holds.
3. Launch the server and frontend.
4. Log in and confirm the stress-test panel renders.
5. Run a stress test in `current` mode and confirm orders/trades/book updates continue working.
6. Switch to `legacy` mode and repeat.
7. Confirm metrics broadcasts are visible and charts update during a run.
8. Execute `exchange_benchmark` and compare current vs legacy timings across all included scenarios.

### Validation status for this PR description

This PR markdown is based on the branch diff and commit history.

Benchmark results and runtime observations should be added to the final GitHub PR if you run the benchmarks before opening or updating the PR.

## Benchmark Results

Results not yet captured in this document.

Recommended follow-up before merge:

- run `exchange_benchmark`
- paste a concise results table here
- highlight the largest improvement areas and any regressions

Suggested table format:

| Scenario | Current | Legacy | Speedup |
|---|---:|---:|---:|
| SimpleMatching/100K | TBD | TBD | TBD |
| BookBuilding/100K | TBD | TBD | TBD |
| PartialFills/100K | TBD | TBD | TBD |
| MultiInstrument/100K | TBD | TBD | TBD |

## Files of Interest for Reviewers

Backend architecture:

- `include/IOrderBook.h`
- `include/Metrics.h`
- `include/OrderBook.h`
- `include/OrderBookLegacy.h`
- `include/Market.h`
- `include/Server.h`

Benchmarking:

- `CMakeLists.txt`
- `benchmark/benchmark_orderbook.cpp`

Frontend:

- `frontend/src/App.js`
- `frontend/src/StressTest.js`
- `frontend/src/StressTest.css`
- `frontend/src/PerformanceGraphs.js`
- `frontend/src/PerformanceGraphs.css`
- `frontend/package.json`

Supporting config:

- `.gitignore`
- `include/Database.h`

## Review Focus Areas

Reviewer attention is especially valuable on:

- correctness of engine parity between `OrderBook` and `OrderBookLegacy`
- whether rebuilding books on engine switch matches intended product behavior
- accuracy and meaning of the latency measurements
- whether the metrics payload is sufficient for the frontend use case
- whether the frontend should consume live backend metric streams directly versus simulating parts of the visualization
- whether the increased default cash balance should remain permanent or be isolated to a stress-test environment

## Rollout / Merge Notes

This branch is best understood as a performance-testing and observability feature set, not just a pure engine optimization patch.

The safest rollout assumption is:

- use it for benchmark comparison and local/demo testing first
- gather benchmark numbers
- validate whether any stress-test-only behavior should be gated, isolated, or refined before treating it as standard product behavior

## Copy-Paste Short PR Description

This PR adds a full latency/performance comparison workflow to the exchange engine. It introduces an `IOrderBook` abstraction, a `OrderBookLegacy` baseline implementation, backend metrics plumbing, runtime engine mode switching, Google Benchmark support, and a frontend stress-test dashboard with side-by-side metrics and graphs. The goal is to make it easy to compare the current optimized engine against a legacy baseline using both native benchmarks and browser-driven load tests.

## Checklist

- [x] Added shared order book abstraction
- [x] Added legacy engine implementation
- [x] Added backend metrics model
- [x] Added runtime engine mode switching
- [x] Added benchmark build target
- [x] Added native benchmark scenarios
- [x] Added frontend stress-test UI
- [x] Added performance graph visualization
- [x] Added frontend charting dependency
- [x] Updated defaults to better support large stress tests
- [ ] Capture benchmark numbers before merge
- [ ] Run/record backend + frontend verification before merge
