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

    Order sell_order = {0, 99, 1000, 10200, 150, 1, false};
    uint32_t rejected_qty = 0;
    book.add_order(sell_order, rejected_qty);
    uint64_t sell_id = sell_order.id;

    Order buy_order = {0, 88, 2000, 10200, 150, 1, true};
    listener.callback = [&](const Trade& t) {
        EXPECT_EQ(t.company_id, 1);
        EXPECT_EQ(t.quantity, 150);
        EXPECT_EQ(t.price, 10200);
        EXPECT_EQ(t.buy_order_id, buy_order.id);
        EXPECT_EQ(t.sell_order_id, sell_id);
    };

    book.add_order(buy_order, rejected_qty);
    uint64_t buy_id = buy_order.id;

    // Correct the buy_order_id expectation in trade listener callback afterwards for checking
    EXPECT_EQ(listener.trades, 1);
    EXPECT_TRUE(book.ask_depth().empty());
}

// ── Test 2: partial fill — buyer wants more than available ──
TEST(OrderBook, PartialFill) {
    OrderBook book;
    MockListener listener;
    book.trade_listener = &listener;

    Order sell_order = {0, 99, 1000, 10200, 100, 1, false};
    uint32_t rejected_qty = 0;
    book.add_order(sell_order, rejected_qty);

    listener.callback = [&](const Trade& t) {
        EXPECT_EQ(t.quantity, 100); 
    };

    Order buy_order = {0, 88, 2000, 10200, 300, 1, true};
    book.add_order(buy_order, rejected_qty);

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

    Order sell_order = {0, 99, 1000, 10300, 100, 1, false};
    uint32_t rejected_qty = 0;
    book.add_order(sell_order, rejected_qty);

    Order buy_order = {0, 88, 2000, 10200, 100, 1, true};
    book.add_order(buy_order, rejected_qty);

    EXPECT_EQ(listener.trades, 0);
    EXPECT_FALSE(book.bid_depth().empty());
    EXPECT_FALSE(book.ask_depth().empty());
}

// ── Test 4: time priority — earlier order fills first ──
TEST(OrderBook, TimePriority) {
    OrderBook book;
    MockListener listener;
    book.trade_listener = &listener;

    Order sell_order1 = {0, 99, 1000, 10200, 100, 1, false};
    uint32_t rejected_qty = 0;
    book.add_order(sell_order1, rejected_qty);
    uint64_t sell_id1 = sell_order1.id;

    Order sell_order2 = {0, 88, 2000, 10200, 100, 1, false};
    book.add_order(sell_order2, rejected_qty);

    uint64_t first_matched_id = 0;
    listener.callback = [&](const Trade& t) {
        if (first_matched_id == 0) first_matched_id = t.sell_order_id;
    };

    Order buy_order = {0, 77, 3000, 10200, 100, 1, true};
    book.add_order(buy_order, rejected_qty);

    EXPECT_EQ(first_matched_id, sell_id1); 
}

TEST(OrderBook, RestingOrderSnapshotsPreservePriorityAndQuantity) {
    OrderBook book;

    Order o1 = {0, 99, 1000, 10100, 100, 1, true};
    Order o2 = {0, 88, 2000, 10200, 150, 1, true};
    Order o3 = {0, 77, 3000, 10200, 75, 1, true};
    Order o4 = {0, 66, 4000, 10400, 50, 1, false};

    uint32_t rejected_qty = 0;
    book.add_order(o1, rejected_qty);
    book.add_order(o2, rejected_qty);
    book.add_order(o3, rejected_qty);
    book.add_order(o4, rejected_qty);

    auto bids = book.bid_orders();
    auto asks = book.ask_orders();

    ASSERT_EQ(bids.size(), 3);
    
    EXPECT_EQ(bids[0].id, o2.id);
    EXPECT_EQ(bids[0].price, 10200);
    EXPECT_EQ(bids[0].quantity, 150);
    
    EXPECT_EQ(bids[1].id, o3.id);
    EXPECT_EQ(bids[1].price, 10200);
    EXPECT_EQ(bids[1].quantity, 75);
    
    EXPECT_EQ(bids[2].id, o1.id);
    EXPECT_EQ(bids[2].price, 10100);
    EXPECT_EQ(bids[2].quantity, 100);

    ASSERT_EQ(asks.size(), 1);
    EXPECT_EQ(asks[0].id, o4.id);
    EXPECT_EQ(asks[0].price, 10400);
}

TEST(OrderBook, CancelOrderRemovesSpecificOrderAtPriceLevel) {
    OrderBook book;

    Order o1 = {0, 99, 1000, 10200, 100, 1, false};
    Order o2 = {0, 88, 2000, 10200, 200, 1, false};
    Order o3 = {0, 77, 3000, 10200, 300, 1, false};

    uint32_t rejected_qty = 0;
    book.add_order(o1, rejected_qty);
    book.add_order(o2, rejected_qty);
    book.add_order(o3, rejected_qty);

    Order cancelled;
    EXPECT_TRUE(book.cancel_order(o2.id, 88, cancelled));
    EXPECT_EQ(cancelled.id, o2.id);
    EXPECT_EQ(cancelled.quantity, 200);

    auto asks = book.ask_orders();
    ASSERT_EQ(asks.size(), 2);
    EXPECT_EQ(asks[0].id, o1.id);
    EXPECT_EQ(asks[0].quantity, 100);
    EXPECT_EQ(asks[1].id, o3.id);
    EXPECT_EQ(asks[1].quantity, 300);

    auto depth = book.ask_depth();
    ASSERT_EQ(depth.size(), 1);
    EXPECT_EQ(depth[0].orders, 2);
    EXPECT_EQ(depth[0].quantity, 400);
}

TEST(OrderBook, CancelOrderErasesEmptyPriceKeyOnly) {
    OrderBook book;

    Order o1 = {0, 99, 1000, 10100, 100, 1, true};
    Order o2 = {0, 88, 2000, 10200, 200, 1, true};

    uint32_t rejected_qty = 0;
    book.add_order(o1, rejected_qty);
    book.add_order(o2, rejected_qty);

    Order cancelled;
    EXPECT_TRUE(book.cancel_order(o2.id, 88, cancelled));

    auto bid_depth = book.bid_depth();
    EXPECT_EQ(bid_depth.size(), 1);
    EXPECT_EQ(bid_depth[0].price, 10100);

    EXPECT_FALSE(book.cancel_order(o2.id, 88, cancelled));
    EXPECT_FALSE(book.cancel_order(o1.id, 88, cancelled)); // wrong user
}

TEST(OrderBook, UnauthorizedCancelRejected) {
    OrderBook book;

    Order o1 = {0, 99, 1000, 10100, 100, 1, true};
    uint32_t rejected_qty = 0;
    book.add_order(o1, rejected_qty);

    Order cancelled;
    // User 88 tries to cancel User 99's order
    EXPECT_FALSE(book.cancel_order(o1.id, 88, cancelled));

    // Order should still be in the book
    auto bids = book.bid_orders();
    ASSERT_EQ(bids.size(), 1);
    EXPECT_EQ(bids[0].id, o1.id);
}

// ── Test 5: Self-Trade Prevention (Cancel-on-Self) ──
TEST(OrderBook, SelfTradePrevention) {
    OrderBook book;
    MockListener listener;
    book.trade_listener = &listener;

    Order sell_order = {0, 99, 1000, 10200, 100, 1, false};
    uint32_t rejected_qty = 0;
    book.add_order(sell_order, rejected_qty);

    // Incoming buy order from the SAME user 99 at the same crossing price
    Order buy_order = {0, 99, 2000, 10200, 100, 1, true};
    book.add_order(buy_order, rejected_qty);

    // No trades should have executed due to self-trade prevention
    EXPECT_EQ(listener.trades, 0);
    // The incoming buy order should have its quantity completely rejected/cancelled
    EXPECT_EQ(rejected_qty, 100);
    
    // The resting sell order should still be in the book unchanged
    auto asks = book.ask_orders();
    ASSERT_EQ(asks.size(), 1);
    EXPECT_EQ(asks[0].id, sell_order.id);
}

// ── Test 6: Pool Exhaustion (MAX_ORDERS capacity limit) ──
TEST(OrderBook, PoolExhaustion) {
    OrderBook book;
    uint32_t rejected_qty = 0;

    // Fill the pool to MAX_ORDERS (100,000)
    for (uint32_t i = 0; i < MAX_ORDERS; i++) {
        Order o = {0, 100 + i, 1000, 10000, 1, 1, true};
        bool ok = book.add_order(o, rejected_qty);
        ASSERT_TRUE(ok);
        ASSERT_EQ(rejected_qty, 0);
    }

    // The next order must be rejected cleanly due to pool exhaustion
    Order o_exhaust = {0, 999, 1000, 10000, 1, 1, true};
    bool ok = book.add_order(o_exhaust, rejected_qty);
    EXPECT_FALSE(ok);
    EXPECT_EQ(rejected_qty, 1);
}