#pragma once

#include "IOrderBook.h"
#include <list>
#include <map>
#include <unordered_map>

// Deliberately naive O(log N) matching engine (the "LEGACY" baseline).
// std::map price levels + std::list FIFO queues + heap-allocated nodes via
// the STL containers themselves - no bitmap, no pre-allocated pool. Exists
// only so the stress-test UI can benchmark it against the O(1) OrderBook
// and show the speedup concretely. Must produce identical trade sequences
// to OrderBook for identical input, including the self-trade-prevention
// "stop processing the rest of this order" semantics.
class OrderBookLegacy : public IOrderBook
{
public:
    const char *engine_name() const override { return "LEGACY"; }

    bool can_process_order(Order &order) override
    {
        return order.price < MAX_PRICE;
    }

    uint32_t process_buy_order(Order &order) override
    {
        order.id = ++order_id_counter_;

        uint32_t rejected_qty = match_buy(order);

        if (order.quantity > 0)
            add_order(order, bids_);

        return rejected_qty;
    }

    uint32_t process_sell_order(Order &order) override
    {
        order.id = ++order_id_counter_;

        uint32_t rejected_qty = match_sell(order);

        if (order.quantity > 0)
            add_order(order, asks_);

        return rejected_qty;
    }

    bool cancel_order(uint64_t order_id, int64_t user_id, Order &cancelled_order) override
    {
        auto loc_it = locations_.find(order_id);
        if (loc_it == locations_.end())
            return false;

        Location loc = loc_it->second;
        if (loc.node_it->user_id != user_id)
            return false;
        cancelled_order = *loc.node_it;

        if (loc.side)
            erase_from_book(bids_, loc);
        else
            erase_from_book(asks_, loc);

        locations_.erase(loc_it);
        return true;
    }

    std::vector<DepthLevel> bid_depth(std::size_t limit = 20) const override { return depth_of(bids_, limit); }
    std::vector<DepthLevel> ask_depth(std::size_t limit = 20) const override { return depth_of(asks_, limit); }
    std::vector<OrderSnapshot> bid_orders(std::size_t limit = 100) const override { return orders_of(bids_, limit); }
    std::vector<OrderSnapshot> ask_orders(std::size_t limit = 100) const override { return orders_of(asks_, limit); }

    uint32_t best_bid() const override { return bids_.empty() ? NULL_PRICE : bids_.begin()->first; }
    uint32_t best_ask() const override { return asks_.empty() ? NULL_PRICE : asks_.begin()->first; }

private:
    static constexpr uint32_t NULL_PRICE = static_cast<uint32_t>(-1);

    struct PriceLevel
    {
        std::list<Order> orders;
        uint32_t total_quantity = 0;
    };

    struct Location
    {
        bool side; // true = bid, false = ask
        uint32_t price;
        std::list<Order>::iterator node_it;
    };

    // Highest price first for bids, lowest price first for asks - matches
    // OrderBook's best-price-first iteration order.
    std::map<uint32_t, PriceLevel, std::greater<uint32_t>> bids_;
    std::map<uint32_t, PriceLevel, std::less<uint32_t>> asks_;
    std::unordered_map<uint64_t, Location> locations_;
    uint64_t order_id_counter_ = 0;

    template <typename Book>
    void add_order(const Order &order, Book &book)
    {
        PriceLevel &level = book[order.price];
        level.orders.push_back(order);
        level.total_quantity += order.quantity;
        auto node_it = std::prev(level.orders.end());
        locations_[order.id] = Location{order.side, order.price, node_it};
    }

    template <typename Book>
    void erase_from_book(Book &book, const Location &loc)
    {
        auto level_it = book.find(loc.price);
        if (level_it == book.end())
            return;
        PriceLevel &level = level_it->second;
        level.total_quantity -= loc.node_it->quantity;
        level.orders.erase(loc.node_it);
        if (level.orders.empty())
            book.erase(level_it);
    }

    template <typename Book>
    std::vector<DepthLevel> depth_of(const Book &book, std::size_t limit) const
    {
        std::vector<DepthLevel> depth;
        for (const auto &[price, level] : book)
        {
            if (depth.size() >= limit)
                break;
            depth.push_back({price, level.total_quantity, static_cast<uint32_t>(level.orders.size())});
        }
        return depth;
    }

    template <typename Book>
    std::vector<OrderSnapshot> orders_of(const Book &book, std::size_t limit) const
    {
        std::vector<OrderSnapshot> snaps;
        for (const auto &[price, level] : book)
        {
            for (const Order &o : level.orders)
            {
                if (snaps.size() >= limit)
                    return snaps;
                snaps.push_back({o.id, o.user_id, o.timestamp, o.price, o.quantity});
            }
        }
        return snaps;
    }

    uint32_t match_buy(Order &incoming)
    {
        while (incoming.quantity > 0 && !asks_.empty() && asks_.begin()->first <= incoming.price)
        {
            auto level_it = asks_.begin();
            uint32_t price = level_it->first;
            PriceLevel &level = level_it->second;

            while (incoming.quantity > 0 && !level.orders.empty())
            {
                Order &resting = level.orders.front();

                if (incoming.user_id == resting.user_id)
                {
                    uint32_t rejected_qty = incoming.quantity;
                    incoming.quantity = 0;
                    return rejected_qty;
                }

                uint32_t fill_qty = std::min(incoming.quantity, resting.quantity);

                Trade trade;
                trade.company_id = incoming.company_id;
                trade.price = price;
                trade.quantity = fill_qty;
                trade.buy_order_id = incoming.id;
                trade.sell_order_id = resting.id;
                trade.buyer_user_id = incoming.user_id;
                trade.seller_user_id = resting.user_id;
                trade.buyer_limit_price = incoming.price;

                incoming.quantity -= fill_qty;
                resting.quantity -= fill_qty;
                level.total_quantity -= fill_qty;

                if (trade_listener)
                    trade_listener->on_trade(trade);

                if (resting.quantity == 0)
                {
                    locations_.erase(resting.id);
                    level.orders.pop_front();
                }
            }

            if (level.orders.empty())
                asks_.erase(level_it);
        }
        return 0;
    }

    uint32_t match_sell(Order &incoming)
    {
        while (incoming.quantity > 0 && !bids_.empty() && bids_.begin()->first >= incoming.price)
        {
            auto level_it = bids_.begin();
            uint32_t price = level_it->first;
            PriceLevel &level = level_it->second;

            while (incoming.quantity > 0 && !level.orders.empty())
            {
                Order &resting = level.orders.front();

                if (incoming.user_id == resting.user_id)
                {
                    uint32_t rejected_qty = incoming.quantity;
                    incoming.quantity = 0;
                    return rejected_qty;
                }

                uint32_t fill_qty = std::min(incoming.quantity, resting.quantity);

                Trade trade;
                trade.company_id = incoming.company_id;
                trade.price = price;
                trade.quantity = fill_qty;
                trade.buy_order_id = resting.id;
                trade.sell_order_id = incoming.id;
                trade.buyer_user_id = resting.user_id;
                trade.seller_user_id = incoming.user_id;
                trade.buyer_limit_price = resting.price;

                incoming.quantity -= fill_qty;
                resting.quantity -= fill_qty;
                level.total_quantity -= fill_qty;

                if (trade_listener)
                    trade_listener->on_trade(trade);

                if (resting.quantity == 0)
                {
                    locations_.erase(resting.id);
                    level.orders.pop_front();
                }
            }

            if (level.orders.empty())
                bids_.erase(level_it);
        }
        return 0;
    }
};
