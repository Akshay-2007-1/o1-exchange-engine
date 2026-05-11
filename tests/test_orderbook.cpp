#include <gtest/gtest.h>
#include "OrderBook.h"

// ── Test 1: a resting order that gets fully filled ──
TEST(OrderBook, FullFill) {
    OrderBook book;
    // {id, timestamp, price, quantity, company_id, side}
    book.add_order({1, 1000, 10200, 150, 1, false}); // false = SELL

    int trades = 0;
    book.on_trade = [&](const Trade& t) {
        trades++;
        EXPECT_EQ(t.company_id, 1);
        EXPECT_EQ(t.quantity, 150);
        EXPECT_EQ(t.price, 10200);
        EXPECT_EQ(t.buy_order_id, 2);
        EXPECT_EQ(t.sell_order_id, 1);
    };

    book.add_order({2, 2000, 10200, 150, 1, true}); // true = BUY

    EXPECT_EQ(trades, 1);
    EXPECT_TRUE(book.ask_depth().empty());
}

// ── Test 2: partial fill — buyer wants more than available ──
TEST(OrderBook, PartialFill) {
    OrderBook book;
    book.add_order({1, 1000, 10200, 100, 1, false});

    int trades = 0;
    book.on_trade = [&](const Trade& t) {
        trades++;
        EXPECT_EQ(t.quantity, 100);  // only 100 available
    };

    book.add_order({2, 2000, 10200, 300, 1, true});

    EXPECT_EQ(trades, 1);
    EXPECT_TRUE(book.ask_depth().empty());           // sell fully consumed
    EXPECT_FALSE(book.bid_depth().empty());          // 200 shares still resting
    
    auto bid_snaps = book.bid_orders();
    ASSERT_FALSE(bid_snaps.empty());
    EXPECT_EQ(bid_snaps[0].quantity, 200);
}

// ── Test 3: no match — prices don't cross ──
TEST(OrderBook, NoMatch) {
    OrderBook book;
    book.add_order({1, 1000, 10300, 100, 1, false});

    int trades = 0;
    book.on_trade = [&](const Trade& t) { trades++; };

    book.add_order({2, 2000, 10200, 100, 1, true});

    EXPECT_EQ(trades, 0);
    EXPECT_FALSE(book.bid_depth().empty());
    EXPECT_FALSE(book.ask_depth().empty());
}

// ── Test 4: time priority — earlier order fills first ──
TEST(OrderBook, TimePriority) {
    OrderBook book;

    // Alice and Bob both sell at $102, Alice was first
    book.add_order({1, 1000, 10200, 100, 1, false});  // Alice
    book.add_order({2, 2000, 10200, 100, 1, false});  // Bob

    uint64_t first_matched_id = 0;
    book.on_trade = [&](const Trade& t) {
        if (first_matched_id == 0) first_matched_id = t.sell_order_id;
    };

    book.add_order({3, 3000, 10200, 100, 1, true});

    EXPECT_EQ(first_matched_id, 1);  // Alice, not Bob
}

TEST(OrderBook, RestingOrderSnapshotsPreservePriorityAndQuantity) {
    OrderBook book;

    book.add_order({1, 1000, 10100, 100, 1, true});
    book.add_order({2, 2000, 10200, 150, 1, true});
    book.add_order({3, 3000, 10200, 75, 1, true});
    book.add_order({4, 4000, 10400, 50, 1, false});

    auto bids = book.bid_orders();
    auto asks = book.ask_orders();

    ASSERT_EQ(bids.size(), 3);
    
    // Best bid ($102.00) first, ordered by time
    EXPECT_EQ(bids[0].id, 2);
    EXPECT_EQ(bids[0].price, 10200);
    EXPECT_EQ(bids[0].quantity, 150);
    
    EXPECT_EQ(bids[1].id, 3);
    EXPECT_EQ(bids[1].price, 10200);
    EXPECT_EQ(bids[1].quantity, 75);
    
    // Next best bid ($101.00)
    EXPECT_EQ(bids[2].id, 1);
    EXPECT_EQ(bids[2].price, 10100);
    EXPECT_EQ(bids[2].quantity, 100);

    ASSERT_EQ(asks.size(), 1);
    EXPECT_EQ(asks[0].id, 4);
    EXPECT_EQ(asks[0].price, 10400);
}

TEST(OrderBook, CancelOrderRemovesSpecificOrderAtPriceLevel) {
    OrderBook book;

    book.add_order({1, 1000, 10200, 100, 1, false});
    book.add_order({2, 2000, 10200, 200, 1, false});
    book.add_order({3, 3000, 10200, 300, 1, false});

    EXPECT_TRUE(book.cancel_order(false, 10200, 2));

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

    book.add_order({1, 1000, 10100, 100, 1, true});
    book.add_order({2, 2000, 10200, 200, 1, true});

    // Cancel the only order at $102.00
    EXPECT_TRUE(book.cancel_order(true, 10200, 2));
    
    auto bid_depth = book.bid_depth();
    EXPECT_EQ(bid_depth.size(), 1);
    EXPECT_EQ(bid_depth[0].price, 10100);

    // Try cancelling an already cancelled order
    EXPECT_FALSE(book.cancel_order(true, 10200, 2));
    
    // Try cancelling with wrong side
    EXPECT_FALSE(book.cancel_order(false, 10100, 1));
}