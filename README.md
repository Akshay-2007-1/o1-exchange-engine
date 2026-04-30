# O(1) Exchange — Matching Engine

A high-performance limit order book and matching engine built in C++.

## What it does
- Accepts limit buy/sell orders
- Matches on strict price/time (FIFO) priority
- Emits trade events on every fill

## Prerequisites
Install these once on your machine:
```bash
sudo apt-get install -y build-essential cmake libboost-all-dev
```

## Build
```bash
mkdir build && cd build
cmake ..
make
./engine
```

## Architecture
- `include/OrderBook.h` — core engine (Order, Trade, OrderBook)
- `src/main.cpp` — entry point
- `tests/` — Google Test suite (coming next)