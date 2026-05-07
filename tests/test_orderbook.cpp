#include <gtest/gtest.h>
#include "OrderBook.h"

// ── Test 1: a resting order that gets fully filled ──
TEST(OrderBook, FullFill) {
    OrderBook book;
    book.add_order({1, 1, "Apollo Technologies", Side::SELL, 102.00, 150, 1000});

    int trades = 0;
    book.on_trade = [&](const Trade& t) {
        trades++;
        EXPECT_EQ(t.company_id, 1);
        EXPECT_EQ(t.company_name, "Apollo Technologies");
        EXPECT_EQ(t.quantity, 150);
        EXPECT_DOUBLE_EQ(t.price, 102.00);
        EXPECT_EQ(t.buy_order_id, 2);
        EXPECT_EQ(t.sell_order_id, 1);
    };

    book.add_order({2, 1, "Apollo Technologies", Side::BUY, 102.00, 150, 2000});

    EXPECT_EQ(trades, 1);
    EXPECT_TRUE(book.asks.empty());
}

// ── Test 2: partial fill — buyer wants more than available ──
TEST(OrderBook, PartialFill) {
    OrderBook book;
    book.add_order({1, 1, "Apollo Technologies", Side::SELL, 102.00, 100, 1000});

    int trades = 0;
    book.on_trade = [&](const Trade& t) {
        trades++;
        EXPECT_EQ(t.quantity, 100);  // only 100 available
    };

    book.add_order({2, 1, "Apollo Technologies", Side::BUY, 102.00, 300, 2000});

    EXPECT_EQ(trades, 1);
    EXPECT_TRUE(book.asks.empty());          // sell fully consumed
    EXPECT_FALSE(book.bids.empty());         // 200 shares still resting
    EXPECT_EQ(book.bids.begin()->second.front().quantity, 200);
}

// ── Test 3: no match — prices don't cross ──
TEST(OrderBook, NoMatch) {
    OrderBook book;
    book.add_order({1, 1, "Apollo Technologies", Side::SELL, 103.00, 100, 1000});

    int trades = 0;
    book.on_trade = [&](const Trade& t) { trades++; };

    book.add_order({2, 1, "Apollo Technologies", Side::BUY, 102.00, 100, 2000});

    EXPECT_EQ(trades, 0);
    EXPECT_FALSE(book.bids.empty());
    EXPECT_FALSE(book.asks.empty());
}

// ── Test 4: time priority — earlier order fills first ──
TEST(OrderBook, TimePriority) {
    OrderBook book;

    // Alice and Bob both sell at $102, Alice was first
    book.add_order({1, 1, "Apollo Technologies", Side::SELL, 102.00, 100, 1000});  // Alice
    book.add_order({2, 1, "Apollo Technologies", Side::SELL, 102.00, 100, 2000});  // Bob

    uint64_t first_matched_id = 0;
    book.on_trade = [&](const Trade& t) {
        if (first_matched_id == 0) first_matched_id = t.sell_order_id;
    };

    book.add_order({3, 1, "Apollo Technologies", Side::BUY, 102.00, 100, 3000});

    EXPECT_EQ(first_matched_id, 1);  // Alice, not Bob
}

TEST(OrderBook, RestingOrderSnapshotsPreservePriorityAndQuantity) {
    OrderBook book;

    book.add_order({1, 1, "Apollo Technologies", Side::BUY, 101.00, 100, 1000});
    book.add_order({2, 1, "Apollo Technologies", Side::BUY, 102.00, 150, 2000});
    book.add_order({3, 1, "Apollo Technologies", Side::BUY, 102.00, 75, 3000});
    book.add_order({4, 1, "Apollo Technologies", Side::SELL, 104.00, 50, 4000});

    auto bids = book.bid_orders();
    auto asks = book.ask_orders();

    ASSERT_EQ(bids.size(), 3);
    EXPECT_EQ(bids[0].id, 2);
    EXPECT_EQ(bids[0].price, 102.00);
    EXPECT_EQ(bids[0].quantity, 150);
    EXPECT_EQ(bids[1].id, 3);
    EXPECT_EQ(bids[1].price, 102.00);
    EXPECT_EQ(bids[1].quantity, 75);
    EXPECT_EQ(bids[2].id, 1);
    EXPECT_EQ(bids[2].price, 101.00);
    EXPECT_EQ(bids[2].quantity, 100);

    ASSERT_EQ(asks.size(), 1);
    EXPECT_EQ(asks[0].id, 4);
}

TEST(OrderBook, CancelOrderRemovesSpecificOrderAtPriceLevel) {
    OrderBook book;

    book.add_order({1, 1, "Apollo Technologies", Side::SELL, 102.00, 100, 1000});
    book.add_order({2, 1, "Apollo Technologies", Side::SELL, 102.00, 200, 2000});
    book.add_order({3, 1, "Apollo Technologies", Side::SELL, 102.00, 300, 3000});

    EXPECT_TRUE(book.cancel_order(Side::SELL, 102.00, 2));

    auto asks = book.ask_orders();
    ASSERT_EQ(asks.size(), 2);
    EXPECT_EQ(asks[0].id, 1);
    EXPECT_EQ(asks[0].quantity, 100);
    EXPECT_EQ(asks[1].id, 3);
    EXPECT_EQ(asks[1].quantity, 300);

    auto depth = book.ask_depth();
    ASSERT_EQ(depth.size(), 1);
    EXPECT_EQ(depth[0].orders, 2);
    EXPECT_EQ(depth[0].quantity, 400);
}

TEST(OrderBook, CancelOrderErasesEmptyPriceKeyOnly) {
    OrderBook book;

    book.add_order({1, 1, "Apollo Technologies", Side::BUY, 101.00, 100, 1000});
    book.add_order({2, 1, "Apollo Technologies", Side::BUY, 102.00, 200, 2000});

    EXPECT_TRUE(book.cancel_order(Side::BUY, 102.00, 2));
    EXPECT_EQ(book.bids.size(), 1);
    EXPECT_EQ(book.bids.begin()->first, 101.00);

    EXPECT_FALSE(book.cancel_order(Side::BUY, 102.00, 2));
    EXPECT_FALSE(book.cancel_order(Side::SELL, 101.00, 1));
}
