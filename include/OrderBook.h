#pragma once

#include <map>
#include <queue>
#include <functional>
#include <algorithm>
#include <cstdint>
#include <iostream>

// ─────────────────────────────────────────
//  Side: which direction is this order?
// ─────────────────────────────────────────
enum class Side { BUY, SELL };

// ─────────────────────────────────────────
//  Order: one instruction from a trader
// ─────────────────────────────────────────
struct Order {
    uint64_t id;
    Side     side;
    double   price;
    uint32_t quantity;
    uint64_t timestamp;
};

// ─────────────────────────────────────────
//  Trade: produced when two orders match
// ─────────────────────────────────────────
struct Trade {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    double   price;
    uint32_t quantity;
};

// ─────────────────────────────────────────
//  OrderBook: the matching engine itself
// ─────────────────────────────────────────
class OrderBook {
public:
    using PriceLevel = std::queue<Order>;

    // bids: highest price first (best bid at begin())
    std::map<double, PriceLevel, std::greater<double>> bids;

    // asks: lowest price first (best ask at begin())
    std::map<double, PriceLevel> asks;

    // fires every time a match happens
    std::function<void(const Trade&)> on_trade;

    void add_order(Order order) {
        if (order.side == Side::BUY) {
            match_buy(order);
            if (order.quantity > 0)
                bids[order.price].push(order);
        } else {
            match_sell(order);
            if (order.quantity > 0)
                asks[order.price].push(order);
        }
    }

private:
    void match_buy(Order& incoming) {
        while (incoming.quantity > 0 && !asks.empty()) {
            auto it = asks.begin();
            if (it->first > incoming.price) break;

            execute_match(incoming, it->second, it->first);
            if (it->second.empty()) asks.erase(it);
        }
    }

    void match_sell(Order& incoming) {
        while (incoming.quantity > 0 && !bids.empty()) {
            auto it = bids.begin();
            if (it->first < incoming.price) break;

            execute_match(incoming, it->second, it->first);
            if (it->second.empty()) bids.erase(it);
        }
    }

    void execute_match(Order& incoming, PriceLevel& level, double exec_price) {
        while (incoming.quantity > 0 && !level.empty()) {
            Order& resting = level.front();
            uint32_t fill_qty = std::min(incoming.quantity, resting.quantity);

            Trade trade;
            trade.price         = exec_price;
            trade.quantity      = fill_qty;
            trade.buy_order_id  = (incoming.side == Side::BUY)  ? incoming.id : resting.id;
            trade.sell_order_id = (incoming.side == Side::SELL) ? incoming.id : resting.id;

            incoming.quantity -= fill_qty;
            resting.quantity  -= fill_qty;

            if (on_trade) on_trade(trade);

            if (resting.quantity == 0) level.pop();
        }
    }
};