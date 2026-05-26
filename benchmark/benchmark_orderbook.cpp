#include <benchmark/benchmark.h>
#include "OrderBook.h"
#include "OrderBookLegacy.h"
#include <random>
#include <memory>

// Simple trade listener for benchmarks
class BenchmarkTradeListener : public ITradeListener {
public:
    uint64_t trade_count = 0;
    void on_trade(const Trade& trade) override {
        trade_count++;
    }
};

// Seed for reproducibility
constexpr uint64_t BENCHMARK_SEED = 12345;

// Benchmark: Current OrderBook - Simple Matching
static void BM_Current_SimpleMatching_100K(benchmark::State& state) {
    auto book = std::make_unique<OrderBook>();
    BenchmarkTradeListener listener;
    book->set_trade_listener(&listener);

    std::mt19937_64 gen(BENCHMARK_SEED);
    std::uniform_int_distribution<uint32_t> price_dist(50000, 75000);
    std::uniform_int_distribution<uint32_t> qty_dist(10, 500);

    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < 50000; ++i) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .user_id = 1,
                .timestamp = static_cast<uint64_t>(1000000 + i),
                .price = price_dist(gen),
                .quantity = qty_dist(gen),
                .company_id = 1,
                .side = (i % 2 == 0)  // Alternate buy/sell
            };
            book->add_order(order);
        }
        state.ResumeTiming();

        // Submit 50K orders
        for (int i = 50000; i < 100000; ++i) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .user_id = 1,
                .timestamp = static_cast<uint64_t>(1000000 + i),
                .price = price_dist(gen),
                .quantity = qty_dist(gen),
                .company_id = 1,
                .side = (i % 2 == 0)
            };
            book->add_order(order);
        }
        state.PauseTiming();
        book = std::make_unique<OrderBook>();
        book->set_trade_listener(&listener);
        state.ResumeTiming();
    }
}
BENCHMARK(BM_Current_SimpleMatching_100K)->Name("Current/SimpleMatching/100K");

// Benchmark: Legacy OrderBook - Simple Matching
static void BM_Legacy_SimpleMatching_100K(benchmark::State& state) {
    auto book = std::make_unique<OrderBookLegacy>();
    BenchmarkTradeListener listener;
    book->set_trade_listener(&listener);

    std::mt19937_64 gen(BENCHMARK_SEED);
    std::uniform_int_distribution<uint32_t> price_dist(50000, 75000);
    std::uniform_int_distribution<uint32_t> qty_dist(10, 500);

    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < 50000; ++i) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .user_id = 1,
                .timestamp = static_cast<uint64_t>(1000000 + i),
                .price = price_dist(gen),
                .quantity = qty_dist(gen),
                .company_id = 1,
                .side = (i % 2 == 0)
            };
            book->add_order(order);
        }
        state.ResumeTiming();

        for (int i = 50000; i < 100000; ++i) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .user_id = 1,
                .timestamp = static_cast<uint64_t>(1000000 + i),
                .price = price_dist(gen),
                .quantity = qty_dist(gen),
                .company_id = 1,
                .side = (i % 2 == 0)
            };
            book->add_order(order);
        }
        state.PauseTiming();
        book = std::make_unique<OrderBookLegacy>();
        book->set_trade_listener(&listener);
        state.ResumeTiming();
    }
}
BENCHMARK(BM_Legacy_SimpleMatching_100K)->Name("Legacy/SimpleMatching/100K");

// Benchmark: Current OrderBook - Book Building
static void BM_Current_BookBuilding_100K(benchmark::State& state) {
    auto book = std::make_unique<OrderBook>();
    BenchmarkTradeListener listener;
    book->set_trade_listener(&listener);

    std::mt19937_64 gen(BENCHMARK_SEED);
    std::uniform_int_distribution<uint32_t> price_dist(50000, 99900);
    std::uniform_int_distribution<uint32_t> qty_dist(10, 100);

    for (auto _ : state) {
        state.PauseTiming();
        book = std::make_unique<OrderBook>();
        book->set_trade_listener(&listener);
        state.ResumeTiming();

        // All buy orders at different prices
        for (int i = 0; i < 100000; ++i) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .user_id = 1,
                .timestamp = static_cast<uint64_t>(1000000 + i),
                .price = static_cast<uint32_t>(50000 + (i % 40000)),
                .quantity = qty_dist(gen),
                .company_id = 1,
                .side = true  // All BUY - rest in book
            };
            book->add_order(order);
        }
    }
}
BENCHMARK(BM_Current_BookBuilding_100K)->Name("Current/BookBuilding/100K");

// Benchmark: Legacy OrderBook - Book Building
static void BM_Legacy_BookBuilding_100K(benchmark::State& state) {
    auto book = std::make_unique<OrderBookLegacy>();
    BenchmarkTradeListener listener;
    book->set_trade_listener(&listener);

    std::mt19937_64 gen(BENCHMARK_SEED);
    std::uniform_int_distribution<uint32_t> qty_dist(10, 100);

    for (auto _ : state) {
        state.PauseTiming();
        book = std::make_unique<OrderBookLegacy>();
        book->set_trade_listener(&listener);
        state.ResumeTiming();

        for (int i = 0; i < 100000; ++i) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .user_id = 1,
                .timestamp = static_cast<uint64_t>(1000000 + i),
                .price = static_cast<uint32_t>(50000 + (i % 40000)),
                .quantity = qty_dist(gen),
                .company_id = 1,
                .side = true
            };
            book->add_order(order);
        }
    }
}
BENCHMARK(BM_Legacy_BookBuilding_100K)->Name("Legacy/BookBuilding/100K");

// Benchmark: Current OrderBook - Partial Fills
static void BM_Current_PartialFills_100K(benchmark::State& state) {
    auto book = std::make_unique<OrderBook>();
    BenchmarkTradeListener listener;
    book->set_trade_listener(&listener);

    std::mt19937_64 gen(BENCHMARK_SEED);

    for (auto _ : state) {
        state.PauseTiming();
        // Add 50K buy orders at price 60000
        for (int i = 0; i < 50000; ++i) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .user_id = 1,
                .timestamp = static_cast<uint64_t>(1000000 + i),
                .price = static_cast<uint32_t>(60000),
                .quantity = 100,
                .company_id = 1,
                .side = true
            };
            book->add_order(order);
        }
        state.ResumeTiming();

        // Add 50K sell orders at varying prices that cross the book
        for (int i = 50000; i < 100000; ++i) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .user_id = 2,
                .timestamp = static_cast<uint64_t>(1000000 + i),
                .price = static_cast<uint32_t>(60000 - (i % 1000)),  // Different prices, will match multiple levels
                .quantity = 50,
                .company_id = 1,
                .side = false  // SELL
            };
            book->add_order(order);
        }
        state.PauseTiming();
        book = std::make_unique<OrderBook>();
        book->set_trade_listener(&listener);
        state.ResumeTiming();
    }
}
BENCHMARK(BM_Current_PartialFills_100K)->Name("Current/PartialFills/100K");

// Benchmark: Legacy OrderBook - Partial Fills
static void BM_Legacy_PartialFills_100K(benchmark::State& state) {
    auto book = std::make_unique<OrderBookLegacy>();
    BenchmarkTradeListener listener;
    book->set_trade_listener(&listener);

    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < 50000; ++i) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .user_id = 1,
                .timestamp = static_cast<uint64_t>(1000000 + i),
                .price = static_cast<uint32_t>(60000),
                .quantity = 100,
                .company_id = 1,
                .side = true
            };
            book->add_order(order);
        }
        state.ResumeTiming();

        for (int i = 50000; i < 100000; ++i) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .user_id = 2,
                .timestamp = static_cast<uint64_t>(1000000 + i),
                .price = static_cast<uint32_t>(60000 - (i % 1000)),
                .quantity = 50,
                .company_id = 1,
                .side = false
            };
            book->add_order(order);
        }
        state.PauseTiming();
        book = std::make_unique<OrderBookLegacy>();
        book->set_trade_listener(&listener);
        state.ResumeTiming();
    }
}
BENCHMARK(BM_Legacy_PartialFills_100K)->Name("Legacy/PartialFills/100K");

// Benchmark: Current OrderBook - Multi-Instrument
static void BM_Current_MultiInstrument_100K(benchmark::State& state) {
    auto book = std::make_unique<OrderBook>();
    BenchmarkTradeListener listener;
    book->set_trade_listener(&listener);

    std::mt19937_64 gen(BENCHMARK_SEED);
    std::uniform_int_distribution<uint32_t> price_dist(50000, 75000);
    std::uniform_int_distribution<uint32_t> qty_dist(10, 500);
    std::uniform_int_distribution<uint16_t> company_dist(1, 3);

    for (auto _ : state) {
        state.PauseTiming();
        book = std::make_unique<OrderBook>();
        book->set_trade_listener(&listener);
        state.ResumeTiming();

        for (int i = 0; i < 100000; ++i) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .user_id = 1,
                .timestamp = static_cast<uint64_t>(1000000 + i),
                .price = price_dist(gen),
                .quantity = qty_dist(gen),
                .company_id = company_dist(gen),  // Random company
                .side = (i % 2 == 0)
            };
            book->add_order(order);
        }
    }
}
BENCHMARK(BM_Current_MultiInstrument_100K)->Name("Current/MultiInstrument/100K");

// Benchmark: Legacy OrderBook - Multi-Instrument
static void BM_Legacy_MultiInstrument_100K(benchmark::State& state) {
    auto book = std::make_unique<OrderBookLegacy>();
    BenchmarkTradeListener listener;
    book->set_trade_listener(&listener);

    std::mt19937_64 gen(BENCHMARK_SEED);
    std::uniform_int_distribution<uint32_t> price_dist(50000, 75000);
    std::uniform_int_distribution<uint32_t> qty_dist(10, 500);
    std::uniform_int_distribution<uint16_t> company_dist(1, 3);

    for (auto _ : state) {
        state.PauseTiming();
        book = std::make_unique<OrderBookLegacy>();
        book->set_trade_listener(&listener);
        state.ResumeTiming();

        for (int i = 0; i < 100000; ++i) {
            Order order{
                .id = static_cast<uint64_t>(i),
                .user_id = 1,
                .timestamp = static_cast<uint64_t>(1000000 + i),
                .price = price_dist(gen),
                .quantity = qty_dist(gen),
                .company_id = company_dist(gen),
                .side = (i % 2 == 0)
            };
            book->add_order(order);
        }
    }
}
BENCHMARK(BM_Legacy_MultiInstrument_100K)->Name("Legacy/MultiInstrument/100K");

BENCHMARK_MAIN();