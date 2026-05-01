#pragma once

#include <map>
#include <queue>
#include <functional>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

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

    struct DepthLevel {
        double price;
        uint64_t quantity;
        uint32_t orders;
    };

    // bids: highest price first (best bid at begin())
    std::map<double, PriceLevel, std::greater<double>> bids;

    // asks: lowest price first (best ask at begin())
    std::map<double, PriceLevel> asks;

    // fires every time a match happens
    std::function<void(const Trade&)> on_trade;

    std::vector<DepthLevel> bid_depth(std::size_t limit = 20) const {
        std::vector<DepthLevel> depth;
        depth.reserve(std::min(limit, bids.size()));

        for (const auto& [price, level] : bids) {
            if (depth.size() == limit) break;
            depth.push_back({price, total_quantity(level), static_cast<uint32_t>(level.size())});
        }

        return depth;
    }

    std::vector<DepthLevel> ask_depth(std::size_t limit = 20) const {
        std::vector<DepthLevel> depth;
        depth.reserve(std::min(limit, asks.size()));

        for (const auto& [price, level] : asks) {
            if (depth.size() == limit) break;
            depth.push_back({price, total_quantity(level), static_cast<uint32_t>(level.size())});
        }

        return depth;
    }

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
    static uint64_t total_quantity(PriceLevel level) {
        uint64_t total = 0;
        while (!level.empty()) {
            total += level.front().quantity;
            level.pop();
        }
        return total;
    }

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
