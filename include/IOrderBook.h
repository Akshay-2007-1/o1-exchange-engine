#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

// Shared price-domain constant: both engines size their price-indexed
// structures (bitmap levels, std::map keys) off this.
constexpr uint32_t MAX_PRICE = 100000;

#pragma pack(push, 1)
struct Order
{
    uint64_t id;         // 8 bytes
    int64_t user_id;     // 8 bytes
    uint64_t timestamp;  // 8 bytes
    uint32_t price;      // 4 bytes
    uint32_t quantity;   // 4 bytes
    uint16_t company_id; // 2 bytes
    bool side;           // 1 byte (true for BUY, false for SELL)
    uint8_t padding[5];  // 5 bytes padding -> Total 40 bytes
};
#pragma pack(pop)

#pragma pack(push, 1)
struct Trade
{
    uint64_t buy_order_id;      // 8 bytes
    uint64_t sell_order_id;     // 8 bytes
    int64_t buyer_user_id;      // 8 bytes
    int64_t seller_user_id;     // 8 bytes
    uint32_t price;             // 4 bytes
    uint32_t quantity;          // 4 bytes
    uint32_t buyer_limit_price; // 4 bytes
    uint16_t company_id;        // 2 bytes
    uint8_t padding[2];         // 2 bytes padding -> Total 48 bytes
};
#pragma pack(pop)

class ITradeListener
{
public:
    virtual ~ITradeListener() = default;
    virtual void on_trade(const Trade &trade) = 0;
};

// Abstract matching-engine interface. OrderBook (the O(1) hierarchical-bitmap
// engine) and OrderBookLegacy (a std::map-based baseline) both implement this,
// so MarketState can swap engines for an instrument at runtime for the
// CURRENT-vs-LEGACY stress test comparison.
class IOrderBook
{
public:
#pragma pack(push, 1)
    struct DepthLevel
    {
        uint32_t price;
        uint32_t quantity;
        uint32_t orders;
        uint8_t padding[4];
    };
#pragma pack(pop)

#pragma pack(push, 1)
    struct OrderSnapshot
    {
        uint64_t id;
        int64_t user_id; // Added to filter for "My Open Orders"
        uint64_t timestamp;
        uint32_t price;
        uint32_t quantity;
    };
#pragma pack(pop)

    ITradeListener *trade_listener = nullptr;

    virtual ~IOrderBook() = default;

    // checks if the order can start to be processed
    virtual bool can_process_order(Order &order) = 0;

    // assumes the order can at least start to be processed
    // returns the quantity of the order which it could NOT process
    // (currently only the self-trade-prevention path rejects quantity)
    virtual uint32_t process_buy_order(Order &order) = 0;
    virtual uint32_t process_sell_order(Order &order) = 0;

    virtual bool cancel_order(uint64_t order_id, int64_t user_id, Order &cancelled_order) = 0;

    virtual std::vector<DepthLevel> bid_depth(std::size_t limit = 20) const = 0;
    virtual std::vector<DepthLevel> ask_depth(std::size_t limit = 20) const = 0;
    virtual std::vector<OrderSnapshot> bid_orders(std::size_t limit = 100) const = 0;
    virtual std::vector<OrderSnapshot> ask_orders(std::size_t limit = 100) const = 0;

    virtual uint32_t best_bid() const = 0;
    virtual uint32_t best_ask() const = 0;

    // "CURRENT" (O(1) bitmap) or "LEGACY" (std::map baseline) - surfaced to
    // the stress-test UI/metrics so clients know which engine produced a result.
    virtual const char *engine_name() const = 0;
};
