// OrderBookLegacy exists purely as a benchmarking baseline (see
// benchmark/benchmark_orderbook.cpp and StressTest.js's engine toggle) — it
// is only useful if it actually behaves like OrderBook. These tests run
// identical order sequences through both engines and assert on identical
// trade output and identical resting-book state.
#include <gtest/gtest.h>
#include "OrderBook.h"
#include "OrderBookLegacy.h"
#include <functional>
#include <vector>

namespace
{

struct RecordedTrade
{
    uint32_t price;
    uint32_t quantity;
    int64_t buyer_user_id;
    int64_t seller_user_id;

    bool operator==(const RecordedTrade &other) const
    {
        return price == other.price && quantity == other.quantity &&
               buyer_user_id == other.buyer_user_id && seller_user_id == other.seller_user_id;
    }
};

class RecordingListener : public ITradeListener
{
public:
    std::vector<RecordedTrade> trades;
    void on_trade(const Trade &t) override
    {
        trades.push_back({t.price, t.quantity, t.buyer_user_id, t.seller_user_id});
    }
};

struct DepthSummary
{
    uint32_t price;
    uint32_t quantity;
    uint32_t orders;

    bool operator==(const DepthSummary &other) const
    {
        return price == other.price && quantity == other.quantity && orders == other.orders;
    }
};

std::vector<DepthSummary> summarize(const std::vector<IOrderBook::DepthLevel> &depth)
{
    std::vector<DepthSummary> out;
    for (const auto &d : depth)
        out.push_back({d.price, d.quantity, d.orders});
    return out;
}

struct ScenarioResult
{
    std::vector<RecordedTrade> trades;
    std::vector<DepthSummary> bids;
    std::vector<DepthSummary> asks;
};

template <typename Book>
ScenarioResult run_scenario(const std::function<void(Book &)> &script)
{
    Book book;
    RecordingListener listener;
    book.trade_listener = &listener;
    script(book);
    return {listener.trades, summarize(book.bid_depth()), summarize(book.ask_depth())};
}

uint32_t submit(IOrderBook &book, Order &order)
{
    if (!book.can_process_order(order))
        return order.quantity;
    return order.side ? book.process_buy_order(order) : book.process_sell_order(order);
}

} // namespace

TEST(OrderBookLegacyParity, BasicCrossingAndPartialFillsMatch)
{
    auto script = [](IOrderBook &book)
    {
        Order sell1 = {0, 1, 1000, 10200, 100, 1, false};
        submit(book, sell1);
        Order sell2 = {0, 2, 2000, 10200, 50, 1, false};
        submit(book, sell2);
        Order sell3 = {0, 3, 3000, 10400, 75, 1, false};
        submit(book, sell3);

        Order buy1 = {0, 4, 4000, 10300, 120, 1, true};
        submit(book, buy1); // fills sell1 fully, sell2 partially (20)

        Order buy2 = {0, 5, 5000, 10100, 10, 1, true};
        submit(book, buy2); // rests, doesn't cross
    };

    auto current = run_scenario<OrderBook>(script);
    auto legacy = run_scenario<OrderBookLegacy>(script);

    EXPECT_EQ(current.trades, legacy.trades);
    EXPECT_EQ(current.bids, legacy.bids);
    EXPECT_EQ(current.asks, legacy.asks);
}

TEST(OrderBookLegacyParity, TimePriorityMatches)
{
    auto script = [](IOrderBook &book)
    {
        Order sell1 = {0, 1, 1000, 10000, 50, 1, false};
        submit(book, sell1);
        Order sell2 = {0, 2, 2000, 10000, 50, 1, false};
        submit(book, sell2);
        Order sell3 = {0, 3, 3000, 10000, 50, 1, false};
        submit(book, sell3);

        Order buy = {0, 4, 4000, 10000, 75, 1, true}; // fills sell1 fully, sell2 half
        submit(book, buy);
    };

    auto current = run_scenario<OrderBook>(script);
    auto legacy = run_scenario<OrderBookLegacy>(script);

    ASSERT_EQ(current.trades.size(), 2u);
    ASSERT_EQ(legacy.trades.size(), 2u);
    EXPECT_EQ(current.trades, legacy.trades);
    EXPECT_EQ(current.asks, legacy.asks);
}

TEST(OrderBookLegacyParity, SelfTradePreventionMatches)
{
    auto script = [](IOrderBook &book)
    {
        Order sell = {0, 42, 1000, 10000, 100, 1, false};
        submit(book, sell);
        Order buy = {0, 42, 2000, 10000, 100, 1, true}; // same user — should reject, not fill
        submit(book, buy);
    };

    auto current = run_scenario<OrderBook>(script);
    auto legacy = run_scenario<OrderBookLegacy>(script);

    EXPECT_TRUE(current.trades.empty());
    EXPECT_TRUE(legacy.trades.empty());
    EXPECT_EQ(current.asks, legacy.asks);
    EXPECT_TRUE(current.bids.empty());
    EXPECT_TRUE(legacy.bids.empty());
}

TEST(OrderBookLegacyParity, CancellationMatches)
{
    auto make_and_cancel = [](auto &book_type_tag) -> std::vector<DepthSummary>
    {
        using Book = std::decay_t<decltype(book_type_tag)>;
        Book book;

        Order o1 = {0, 10, 1000, 10500, 200, 1, true};
        submit(book, o1);
        Order o2 = {0, 11, 2000, 10500, 100, 1, true};
        submit(book, o2);

        Order cancelled;
        bool ok = book.cancel_order(o1.id, 10, cancelled);
        EXPECT_TRUE(ok);
        EXPECT_EQ(cancelled.quantity, 200u);

        return summarize(book.bid_depth());
    };

    OrderBook current_tag;
    OrderBookLegacy legacy_tag;
    auto current_bids = make_and_cancel(current_tag);
    auto legacy_bids = make_and_cancel(legacy_tag);

    EXPECT_EQ(current_bids, legacy_bids);
    ASSERT_EQ(current_bids.size(), 1u);
    EXPECT_EQ(current_bids[0].quantity, 100u);
}
