// Compares OrderBook (the O(1) hierarchical-bitmap "CURRENT" engine) against
// OrderBookLegacy (the std::map-based "LEGACY" baseline) across the same
// scenarios, at the same scales, so the speedup claim in MS1-3 docs is a
// measured number rather than a general assertion.
//
// Run: ./build/benchmarks --benchmark_out=benchmark_results.json
//                          --benchmark_out_format=json

#include <benchmark/benchmark.h>
#include "OrderBook.h"
#include "OrderBookLegacy.h"
#include <random>
#include <vector>

namespace
{

// Deterministic per-benchmark RNG so CURRENT and LEGACY see identical order
// sequences — the comparison isolates the engine, not the input.
std::vector<Order> make_crossing_orders(int n, uint32_t base_price)
{
    std::vector<Order> orders;
    orders.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        Order o{};
        o.user_id = i;
        o.timestamp = static_cast<uint64_t>(i);
        o.company_id = 1;
        o.side = (i % 2 == 0); // alternating buy/sell at the same price fully cross
        o.price = base_price;
        o.quantity = 100;
        orders.push_back(o);
    }
    return orders;
}

std::vector<Order> make_resting_orders(int n, std::mt19937 &rng)
{
    // Buys and sells occupy disjoint price ranges so nothing crosses — this
    // isolates insertion/depth-tracking cost from matching cost.
    std::uniform_int_distribution<uint32_t> bid_price(1, 40000);
    std::uniform_int_distribution<uint32_t> ask_price(60000, 99999);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 500);

    std::vector<Order> orders;
    orders.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        Order o{};
        o.user_id = i;
        o.timestamp = static_cast<uint64_t>(i);
        o.company_id = 1;
        o.side = (i % 2 == 0);
        o.price = o.side ? bid_price(rng) : ask_price(rng);
        o.quantity = qty_dist(rng);
        orders.push_back(o);
    }
    return orders;
}

template <typename Book>
void submit(Book &book, Order &order)
{
    if (!book.can_process_order(order))
        return;
    if (order.side)
        book.process_buy_order(order);
    else
        book.process_sell_order(order);
}

} // namespace

// ── Scenario 1: simple matching — every order fully crosses immediately ──
template <typename Book>
static void BM_SimpleMatching(benchmark::State &state)
{
    const int n = static_cast<int>(state.range(0));
    for (auto _ : state)
    {
        state.PauseTiming();
        auto orders = make_crossing_orders(n, 10000);
        Book book;
        state.ResumeTiming();

        for (auto &o : orders)
            submit(book, o);
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK_TEMPLATE(BM_SimpleMatching, OrderBook)->Arg(1000)->Arg(10000)->Arg(100000);
BENCHMARK_TEMPLATE(BM_SimpleMatching, OrderBookLegacy)->Arg(1000)->Arg(10000)->Arg(100000);

// ── Scenario 2: book building — resting orders across many price levels ──
template <typename Book>
static void BM_BookBuilding(benchmark::State &state)
{
    const int n = static_cast<int>(state.range(0));
    std::mt19937 rng(42);
    for (auto _ : state)
    {
        state.PauseTiming();
        auto orders = make_resting_orders(n, rng);
        Book book;
        state.ResumeTiming();

        for (auto &o : orders)
            submit(book, o);
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK_TEMPLATE(BM_BookBuilding, OrderBook)->Arg(1000)->Arg(10000)->Arg(100000);
BENCHMARK_TEMPLATE(BM_BookBuilding, OrderBookLegacy)->Arg(1000)->Arg(10000)->Arg(100000);

// ── Scenario 3: partial fills — one large resting order, many small takers ──
template <typename Book>
static void BM_PartialFills(benchmark::State &state)
{
    const int n = static_cast<int>(state.range(0));
    for (auto _ : state)
    {
        state.PauseTiming();
        Book book;
        Order big_sell{};
        big_sell.user_id = -1;
        big_sell.timestamp = 0;
        big_sell.company_id = 1;
        big_sell.side = false;
        big_sell.price = 10000;
        big_sell.quantity = static_cast<uint32_t>(n) * 10;
        submit(book, big_sell);

        std::vector<Order> takers;
        takers.reserve(n);
        for (int i = 0; i < n; ++i)
        {
            Order o{};
            o.user_id = i + 1;
            o.timestamp = static_cast<uint64_t>(i + 1);
            o.company_id = 1;
            o.side = true;
            o.price = 10000;
            o.quantity = 10;
            takers.push_back(o);
        }
        state.ResumeTiming();

        for (auto &o : takers)
            submit(book, o);
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK_TEMPLATE(BM_PartialFills, OrderBook)->Arg(1000)->Arg(10000)->Arg(100000);
BENCHMARK_TEMPLATE(BM_PartialFills, OrderBookLegacy)->Arg(1000)->Arg(10000)->Arg(100000);

// ── Scenario 4: cancellations — build a resting book, then cancel it all ──
template <typename Book>
static void BM_Cancellations(benchmark::State &state)
{
    const int n = static_cast<int>(state.range(0));
    std::mt19937 rng(42);
    for (auto _ : state)
    {
        state.PauseTiming();
        auto orders = make_resting_orders(n, rng);
        Book book;
        for (auto &o : orders)
            submit(book, o);
        state.ResumeTiming();

        Order cancelled;
        for (auto &o : orders)
            book.cancel_order(o.id, o.user_id, cancelled);
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK_TEMPLATE(BM_Cancellations, OrderBook)->Arg(1000)->Arg(10000)->Arg(100000);
BENCHMARK_TEMPLATE(BM_Cancellations, OrderBookLegacy)->Arg(1000)->Arg(10000)->Arg(100000);

BENCHMARK_MAIN();
