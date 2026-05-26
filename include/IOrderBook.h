#pragma once

#include <cstdint>
#include <vector>
#include "Metrics.h" // Add this include

// Forward declarations - these are defined in OrderBook.h
struct Order;
struct Trade;
class ITradeListener;

// Shared structs for OrderBook interface
struct DepthLevel {
    uint32_t price;
    uint32_t quantity;
    uint32_t orders;
};

struct OrderSnapshot {
    uint64_t id;
    int64_t  user_id;
    uint64_t timestamp;
    uint32_t price;
    uint32_t quantity;
};

class IOrderBook {
public:
    virtual ~IOrderBook() = default;

    virtual void add_order(const Order& order) = 0;
    virtual bool cancel_order(bool side, uint32_t price, uint64_t order_id, int64_t user_id) = 0;

    virtual std::vector<DepthLevel> bid_depth(std::size_t limit = 20) const = 0;
    virtual std::vector<DepthLevel> ask_depth(std::size_t limit = 20) const = 0;
    virtual std::vector<OrderSnapshot> bid_orders(std::size_t limit = 100) const = 0;
    virtual std::vector<OrderSnapshot> ask_orders(std::size_t limit = 100) const = 0;

    virtual ITradeListener* get_trade_listener() = 0;
    virtual void set_trade_listener(ITradeListener* listener) = 0;

    virtual void set_metrics(EngineMetrics* metrics) = 0;
    virtual EngineMetrics* get_metrics() const = 0;
};
