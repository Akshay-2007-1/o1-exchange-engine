#include <gtest/gtest.h>
#include <functional>
#include "OrderBook.h"

// Mock implementation to capture the interface callbacks
class MockListener : public ITradeListener {
public:
    int trades = 0;
    std::function<void(const Trade&)> callback;
    
    void on_trade(const Trade& t) override {
        trades++;
        if (callback) callback(t);
    }
};

// ── Test 1: a resting order that gets fully filled ──
TEST(OrderBook, FullFill) {
    OrderBook book;
    MockListener listener;
    book.trade_listener = &listener;

    // {id, user_id, timestamp, price, quantity, company_id, side}
    book.add_order({1, 99, 1000, 10200, 150, 1, false});

    listener.callback = [&](const Trade& t) {
        EXPECT_EQ(t.company_id, 1);
        EXPECT_EQ(t.quantity, 150);
        EXPECT_EQ(t.price, 10200);
        EXPECT_EQ(t.buy_order_id, 2);
        EXPECT_EQ(t.sell_order_id, 1);
    };

    book.add_order({2, 88, 2000, 10200, 150, 1, true});

    EXPECT_EQ(listener.trades, 1);
    EXPECT_TRUE(book.ask_depth().empty());
}

// ── Test 2: partial fill — buyer wants more than available ──
TEST(OrderBook, PartialFill) {
    OrderBook book;
    MockListener listener;
    book.trade_listener = &listener;

    book.add_order({1, 99, 1000, 10200, 100, 1, false});

    listener.callback = [&](const Trade& t) {
        EXPECT_EQ(t.quantity, 100); 
    };

    book.add_order({2, 88, 2000, 10200, 300, 1, true});

    EXPECT_EQ(listener.trades, 1);
    EXPECT_TRUE(book.ask_depth().empty());           
    EXPECT_FALSE(book.bid_depth().empty());          
    
    auto bid_snaps = book.bid_orders();
    ASSERT_FALSE(bid_snaps.empty());
    EXPECT_EQ(bid_snaps[0].quantity, 200);
}

// ── Test 3: no match — prices don't cross ──
TEST(OrderBook, NoMatch) {
    OrderBook book;
    MockListener listener;
    book.trade_listener = &listener;

    book.add_order({1, 99, 1000, 10300, 100, 1, false});

    book.add_order({2, 88, 2000, 10200, 100, 1, true});

    EXPECT_EQ(listener.trades, 0);
    EXPECT_FALSE(book.bid_depth().empty());
    EXPECT_FALSE(book.ask_depth().empty());
}

// ── Test 4: time priority — earlier order fills first ──
TEST(OrderBook, TimePriority) {
    OrderBook book;
    MockListener listener;
    book.trade_listener = &listener;

    book.add_order({1, 99, 1000, 10200, 100, 1, false});  
    book.add_order({2, 88, 2000, 10200, 100, 1, false});  

    uint64_t first_matched_id = 0;
    listener.callback = [&](const Trade& t) {
        if (first_matched_id == 0) first_matched_id = t.sell_order_id;
    };

    book.add_order({3, 77, 3000, 10200, 100, 1, true});

    EXPECT_EQ(first_matched_id, 1); 
}

TEST(OrderBook, RestingOrderSnapshotsPreservePriorityAndQuantity) {
    OrderBook book;

    book.add_order({1, 99, 1000, 10100, 100, 1, true});
    book.add_order({2, 88, 2000, 10200, 150, 1, true});
    book.add_order({3, 77, 3000, 10200, 75, 1, true});
    book.add_order({4, 66, 4000, 10400, 50, 1, false});

    auto bids = book.bid_orders();
    auto asks = book.ask_orders();

    ASSERT_EQ(bids.size(), 3);
    
    EXPECT_EQ(bids[0].id, 2);
    EXPECT_EQ(bids[0].price, 10200);
    EXPECT_EQ(bids[0].quantity, 150);
    
    EXPECT_EQ(bids[1].id, 3);
    EXPECT_EQ(bids[1].price, 10200);
    EXPECT_EQ(bids[1].quantity, 75);
    
    EXPECT_EQ(bids[2].id, 1);
    EXPECT_EQ(bids[2].price, 10100);
    EXPECT_EQ(bids[2].quantity, 100);

    ASSERT_EQ(asks.size(), 1);
    EXPECT_EQ(asks[0].id, 4);
    EXPECT_EQ(asks[0].price, 10400);
}

TEST(OrderBook, CancelOrderRemovesSpecificOrderAtPriceLevel) {
    OrderBook book;

    book.add_order({1, 99, 1000, 10200, 100, 1, false});
    book.add_order({2, 88, 2000, 10200, 200, 1, false});
    book.add_order({3, 77, 3000, 10200, 300, 1, false});

    EXPECT_TRUE(book.cancel_order(false, 10200, 2, 88));

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

    book.add_order({1, 99, 1000, 10100, 100, 1, true});
    book.add_order({2, 88, 2000, 10200, 200, 1, true});

    EXPECT_TRUE(book.cancel_order(true, 10200, 2, 88));

    auto bid_depth = book.bid_depth();
    EXPECT_EQ(bid_depth.size(), 1);
    EXPECT_EQ(bid_depth[0].price, 10100);

    EXPECT_FALSE(book.cancel_order(true, 10200, 2, 88));
    EXPECT_FALSE(book.cancel_order(false, 10100, 1, 99));
}

TEST(OrderBook, UnauthorizedCancelRejected) {
    OrderBook book;

    // Order 1 belongs to user 99
    book.add_order({1, 99, 1000, 10100, 100, 1, true});

    // User 88 tries to cancel User 99's order
    EXPECT_FALSE(book.cancel_order(true, 10100, 1, 88));

    // Order should still be in the book
    auto bids = book.bid_orders();
    ASSERT_EQ(bids.size(), 1);
    EXPECT_EQ(bids[0].id, 1);
}