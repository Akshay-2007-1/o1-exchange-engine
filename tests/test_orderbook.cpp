#include <gtest/gtest.h>
#include <functional>
#include "OrderBook.h"

class MockListener : public ITradeListener {
public:
    int trades = 0;
    std::function<void(const Trade&)> callback;

    void on_trade(const Trade& t) override {
        trades++;
        if (callback) callback(t);
    }
};

static uint32_t add_order(OrderBook &book, Order &order) {
    if (!book.can_process_order(order)) return order.quantity;
    return order.side ? book.process_buy_order(order) : book.process_sell_order(order);
}

// ── Test 1: a resting order that gets fully filled ──
TEST(OrderBook, FullFill) {
    OrderBook book;
    MockListener listener;
    book.trade_listener = &listener;

    Order sell_order = {0, 99, 1000, 10200, 150, 1, false};
    add_order(book, sell_order);
    uint64_t sell_id = sell_order.id;

    Order buy_order = {0, 88, 2000, 10200, 150, 1, true};
    listener.callback = [&](const Trade& t) {
        EXPECT_EQ(t.company_id, 1);
        EXPECT_EQ(t.quantity, 150);
        EXPECT_EQ(t.price, 10200);
        EXPECT_EQ(t.sell_order_id, sell_id);
    };

    add_order(book, buy_order);

    EXPECT_EQ(listener.trades, 1);
    EXPECT_TRUE(book.ask_depth().empty());
}

// ── Test 2: partial fill — buyer wants more than available ──
TEST(OrderBook, PartialFill) {
    OrderBook book;
    MockListener listener;
    book.trade_listener = &listener;

    Order sell_order = {0, 99, 1000, 10200, 100, 1, false};
    add_order(book, sell_order);

    listener.callback = [&](const Trade& t) {
        EXPECT_EQ(t.quantity, 100);
    };

    Order buy_order = {0, 88, 2000, 10200, 300, 1, true};
    add_order(book, buy_order);

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
    add_order(book, sell_order);

    Order buy_order = {0, 88, 2000, 10200, 100, 1, true};
    add_order(book, buy_order);

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
    add_order(book, sell_order1);
    uint64_t sell_id1 = sell_order1.id;

    Order sell_order2 = {0, 88, 2000, 10200, 100, 1, false};
    add_order(book, sell_order2);

    uint64_t first_matched_id = 0;
    listener.callback = [&](const Trade& t) {
        if (first_matched_id == 0) first_matched_id = t.sell_order_id;
    };

    Order buy_order = {0, 77, 3000, 10200, 100, 1, true};
    add_order(book, buy_order);

    EXPECT_EQ(first_matched_id, sell_id1);
}

TEST(OrderBook, RestingOrderSnapshotsPreservePriorityAndQuantity) {
    OrderBook book;

    Order o1 = {0, 99, 1000, 10100, 100, 1, true};
    Order o2 = {0, 88, 2000, 10200, 150, 1, true};
    Order o3 = {0, 77, 3000, 10200, 75, 1, true};
    Order o4 = {0, 66, 4000, 10400, 50, 1, false};

    add_order(book, o1);
    add_order(book, o2);
    add_order(book, o3);
    add_order(book, o4);

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

// ── Test 5: self-trade prevention ──
TEST(OrderBook, SelfTradeBlocked) {
    OrderBook book;
    MockListener listener;
    book.trade_listener = &listener;

    // User 42 places a sell
    Order sell = {0, 42, 1000, 10000, 100, 1, false};
    add_order(book, sell);

    // Same user places a crossing buy — should be rejected
    Order buy = {0, 42, 2000, 10000, 100, 1, true};
    uint32_t rejected = add_order(book, buy);

    EXPECT_EQ(listener.trades, 0);
    EXPECT_EQ(rejected, 100u);
    // Sell order should still be in the book
    EXPECT_FALSE(book.ask_depth().empty());
}

// ── Test 6: cancellation removes the order from the book ──
TEST(OrderBook, CancelRemovesOrder) {
    OrderBook book;

    Order o = {0, 55, 1000, 10500, 200, 1, true};
    add_order(book, o);
    ASSERT_FALSE(book.bid_depth().empty());

    Order cancelled;
    bool ok = book.cancel_order(o.id, 55, cancelled);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(book.bid_depth().empty());
    EXPECT_EQ(cancelled.quantity, 200u);
}

// ── Test 7: cancelling a partially-filled order only refunds the
//    unfilled remainder — Server.h relies on cancelled_order.quantity
//    to compute the wallet refund, so this is the critical invariant
//    behind issue #4 (partial-fill-cancel-no-refund) staying fixed. ──
TEST(OrderBook, CancelAfterPartialFillRefundsRemainderOnly) {
    OrderBook book;

    Order sell = {0, 1, 1000, 10000, 200, 1, false};
    add_order(book, sell);

    Order buy = {0, 2, 2000, 10000, 80, 1, true};
    add_order(book, buy); // fills 80 of the resting 200

    auto asks = book.ask_depth();
    ASSERT_FALSE(asks.empty());
    EXPECT_EQ(asks[0].quantity, 120u);

    Order cancelled;
    bool ok = book.cancel_order(sell.id, 1, cancelled);

    EXPECT_TRUE(ok);
    EXPECT_EQ(cancelled.quantity, 120u); // NOT the original 200
    EXPECT_TRUE(book.ask_depth().empty());
}

// ── Test 8: once the pre-allocated node pool (MAX_ORDERS) is exhausted,
//    can_process_order must reject further orders rather than silently
//    dropping them — see issue #2 (pool-exhaustion-funds-stolen); the
//    caller (Server.h) only refunds a rejected order's reservation if
//    can_process_order said no *before* any reservation-consuming side
//    effect happens inside the engine. ──
TEST(OrderBook, PoolExhaustionRejectsFurtherOrders) {
    OrderBook book;

    for (uint32_t i = 0; i < MAX_ORDERS; ++i) {
        Order o = {0, static_cast<int64_t>(i), i, (i % (MAX_PRICE - 1)) + 1, 1, 1, true};
        ASSERT_TRUE(book.can_process_order(o)) << "order " << i << " should still fit in the pool";
        book.process_buy_order(o);
    }

    Order overflow = {0, 999999, 999999, 500, 1, 1, true};
    EXPECT_FALSE(book.can_process_order(overflow));
}
