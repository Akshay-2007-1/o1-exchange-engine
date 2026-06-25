#pragma once

#include <cstdint>
#include <iostream>
#include <vector>

constexpr uint32_t MAX_PRICE = 100000;
constexpr uint32_t BITMAP_L1_SIZE = (MAX_PRICE / 64) + 1;
constexpr uint32_t BITMAP_L2_SIZE = (BITMAP_L1_SIZE / 64) + 1;
constexpr uint32_t MAX_ORDERS = 100000;
constexpr uint32_t NULL_IDX = -1;

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
struct OrderNode
{
    Order order;         // 40 bytes
    uint32_t prev_idx;   // 4 bytes
    uint32_t next_idx;   // 4 bytes
    uint8_t padding[16]; // 16 bytes padding -> Total 64 bytes
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

struct OrderList
{
    uint32_t head_idx = NULL_IDX;
    uint32_t tail_idx = NULL_IDX;
    uint32_t total_quantity = 0;
    uint32_t order_count = 0;
};

class OrderBook
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
    uint64_t order_id_counter = 0;

    OrderBook()
    {
        node_pool_.resize(MAX_ORDERS);
        free_indices_.reserve(MAX_ORDERS);

        for (uint32_t i = 0; i < MAX_ORDERS; ++i)
        {
            free_indices_.push_back(MAX_ORDERS - 1 - i);
        }
    }

    // checks if the order can start to be processed
    bool can_process_order(Order &order)
    {
        return order.price < MAX_PRICE && !free_indices_.empty();
    }

    // assumes the order can at least start to be processed
    // returns the quantity of the order which it could NOT process
    /*
        currently the only reason for an order to stop getting
        processed in the middle of its processing is the
        prevention of self-trades
    */
    // assumes the order is a buy order
    uint32_t process_buy_order(Order &order)
    {
        uint32_t pool_idx = free_indices_.back();
        free_indices_.pop_back();

        order.id = ((++order_id_counter) << 32) | pool_idx;

        uint32_t rejected_qty = match_buy(order);

        if (order.quantity > 0)
            add_buy_order(order);
        else
            free_indices_.push_back(pool_idx);

        return rejected_qty;
    }

    // assumes the order can at least start to be processed
    // returns the quantity of the order which it could NOT process
    /*
        currently the only reason for an order to stop getting
        processed in the middle of its processing is the
        prevention of self-trades
    */
    // assumes the order is a sell order
    uint32_t process_sell_order(Order &order)
    {
        uint32_t pool_idx = free_indices_.back();
        free_indices_.pop_back();

        order.id = ((++order_id_counter) << 32) | pool_idx;

        uint32_t rejected_qty = match_sell(order);

        if (order.quantity > 0)
            add_sell_order(order);
        else
            free_indices_.push_back(pool_idx);

        return rejected_qty;
    }  

    bool cancel_order(uint64_t order_id, int64_t user_id, Order &cancelled_order)
    {
        uint32_t target_idx = static_cast<uint32_t>(order_id & 0xFFFFFFFFULL);
        if (target_idx >= MAX_ORDERS) [[unlikely]]
            return false;

        OrderNode &node = node_pool_[target_idx];

        if (node.order.id != order_id || node.order.user_id != user_id) [[unlikely]]
            return false;
        
        cancelled_order = node.order;
        bool side = cancelled_order.side;
        uint32_t price = cancelled_order.price;

        OrderList &list = side ? bids_[price] : asks_[price];

        if (node.prev_idx == NULL_IDX) [[unlikely]]
            list.head_idx = node.next_idx;
        else
            node_pool_[node.prev_idx].next_idx = node.next_idx;

        if (node.next_idx == NULL_IDX) [[unlikely]]
            list.tail_idx = node.prev_idx;
        else
            node_pool_[node.next_idx].prev_idx = node.prev_idx;

        list.total_quantity -= node.order.quantity;
        list.order_count--;

        if (list.order_count == 0) [[unlikely]]
        {
            if (side)
                clear_bids_bit(price);
            else
                clear_asks_bit(price);
        }

        node.order.id = 0;

        free_indices_.push_back(target_idx);
        return true;
    }

    std::vector<DepthLevel> bid_depth(std::size_t limit = 20) const
    {
        std::vector<DepthLevel> depth;
        uint64_t l3_copy = bids_l3_;

        while (l3_copy && depth.size() < limit)
        {
            uint32_t l3_bit = 63 - __builtin_clzll(l3_copy);
            uint64_t l2_copy = bids_l2_[l3_bit];

            while (l2_copy && depth.size() < limit)
            {
                uint32_t l2_bit = 63 - __builtin_clzll(l2_copy);
                uint32_t l1_idx = (l3_bit * 64) + l2_bit;
                uint64_t l1_copy = bids_l1_[l1_idx];

                while (l1_copy && depth.size() < limit)
                {
                    uint32_t l1_bit = 63 - __builtin_clzll(l1_copy);
                    uint32_t price = (l1_idx * 64) + l1_bit;
                    depth.push_back({price, bids_[price].total_quantity, bids_[price].order_count});
                    l1_copy &= ~(1ULL << l1_bit);
                }
                l2_copy &= ~(1ULL << l2_bit);
            }
            l3_copy &= ~(1ULL << l3_bit);
        }
        return depth;
    }

    std::vector<DepthLevel> ask_depth(std::size_t limit = 20) const
    {
        std::vector<DepthLevel> depth;
        uint64_t l3_copy = asks_l3_;

        while (l3_copy && depth.size() < limit)
        {
            uint32_t l3_bit = __builtin_ctzll(l3_copy);
            uint64_t l2_copy = asks_l2_[l3_bit];

            while (l2_copy && depth.size() < limit)
            {
                uint32_t l2_bit = __builtin_ctzll(l2_copy);
                uint32_t l1_idx = (l3_bit * 64) + l2_bit;
                uint64_t l1_copy = asks_l1_[l1_idx];

                while (l1_copy && depth.size() < limit)
                {
                    uint32_t l1_bit = __builtin_ctzll(l1_copy);
                    uint32_t price = (l1_idx * 64) + l1_bit;
                    depth.push_back({price, asks_[price].total_quantity, asks_[price].order_count});
                    l1_copy &= ~(1ULL << l1_bit);
                }
                l2_copy &= ~(1ULL << l2_bit);
            }
            l3_copy &= ~(1ULL << l3_bit);
        }
        return depth;
    }

    std::vector<OrderSnapshot> bid_orders(std::size_t limit = 100) const
    {
        std::vector<OrderSnapshot> snaps;
        auto depth = bid_depth(limit);
        for (const auto &level : depth)
        {
            uint32_t curr_idx = bids_[level.price].head_idx;
            while (curr_idx != NULL_IDX && snaps.size() < limit)
            {
                const Order &o = node_pool_[curr_idx].order;
                snaps.push_back({o.id, o.user_id, o.timestamp, o.price, o.quantity});
                curr_idx = node_pool_[curr_idx].next_idx;
            }
            if (snaps.size() == limit)
                break;
        }
        return snaps;
    }

    uint32_t best_bid() const { return get_best_bid(); }
    uint32_t best_ask() const { return get_best_ask(); }

    std::vector<OrderSnapshot> ask_orders(std::size_t limit = 100) const
    {
        std::vector<OrderSnapshot> snaps;
        auto depth = ask_depth(limit);
        for (const auto &level : depth)
        {
            uint32_t curr_idx = asks_[level.price].head_idx;
            while (curr_idx != NULL_IDX && snaps.size() < limit)
            {
                const Order &o = node_pool_[curr_idx].order;
                snaps.push_back({o.id, o.user_id, o.timestamp, o.price, o.quantity});
                curr_idx = node_pool_[curr_idx].next_idx;
            }
            if (snaps.size() == limit)
                break;
        }
        return snaps;
    }

private:
    std::vector<OrderNode> node_pool_;
    std::vector<uint32_t> free_indices_;

    OrderList bids_[MAX_PRICE];
    OrderList asks_[MAX_PRICE];

    uint64_t bids_l1_[BITMAP_L1_SIZE] = {0};
    uint64_t bids_l2_[BITMAP_L2_SIZE] = {0};
    uint64_t bids_l3_ = 0;

    uint64_t asks_l1_[BITMAP_L1_SIZE] = {0};
    uint64_t asks_l2_[BITMAP_L2_SIZE] = {0};
    uint64_t asks_l3_ = 0;

    void set_bids_bit(uint32_t price)
    {
        uint32_t l1_idx = price / 64;
        uint32_t l1_bit = price % 64;
        uint32_t l2_idx = l1_idx / 64;
        uint32_t l2_bit = l1_idx % 64;
        uint32_t l3_bit = l2_idx;

        bids_l1_[l1_idx] |= (1ULL << l1_bit);
        bids_l2_[l2_idx] |= (1ULL << l2_bit);
        bids_l3_ |= (1ULL << l3_bit);
    }

    void set_asks_bit(uint32_t price)
    {
        uint32_t l1_idx = price / 64;
        uint32_t l1_bit = price % 64;
        uint32_t l2_idx = l1_idx / 64;
        uint32_t l2_bit = l1_idx % 64;
        uint32_t l3_bit = l2_idx;

        asks_l1_[l1_idx] |= (1ULL << l1_bit);
        asks_l2_[l2_idx] |= (1ULL << l2_bit);
        asks_l3_ |= (1ULL << l3_bit);
    }

    void clear_bids_bit(uint32_t price)
    {
        uint32_t l1_idx = price / 64;
        uint32_t l1_bit = price % 64;
        uint32_t l2_idx = l1_idx / 64;
        uint32_t l2_bit = l1_idx % 64;
        uint32_t l3_bit = l2_idx;

        bids_l1_[l1_idx] &= ~(1ULL << l1_bit);
        if (bids_l1_[l1_idx] == 0)
        {
            bids_l2_[l2_idx] &= ~(1ULL << l2_bit);
            if (bids_l2_[l2_idx] == 0)
            {
                bids_l3_ &= ~(1ULL << l3_bit);
            }
        }
    }

    void clear_asks_bit(uint32_t price)
    {
        uint32_t l1_idx = price / 64;
        uint32_t l1_bit = price % 64;
        uint32_t l2_idx = l1_idx / 64;
        uint32_t l2_bit = l1_idx % 64;
        uint32_t l3_bit = l2_idx;

        asks_l1_[l1_idx] &= ~(1ULL << l1_bit);
        if (asks_l1_[l1_idx] == 0)
        {
            asks_l2_[l2_idx] &= ~(1ULL << l2_bit);
            if (asks_l2_[l2_idx] == 0)
            {
                asks_l3_ &= ~(1ULL << l3_bit);
            }
        }
    }

    uint32_t get_best_bid() const
    {
        if (bids_l3_ == 0)
            return NULL_IDX;
        uint32_t l3_bit = 63 - __builtin_clzll(bids_l3_);
        uint32_t l2_bit = 63 - __builtin_clzll(bids_l2_[l3_bit]);
        uint32_t l1_idx = (l3_bit * 64) + l2_bit;
        uint32_t l1_bit = 63 - __builtin_clzll(bids_l1_[l1_idx]);
        return (l1_idx * 64) + l1_bit;
    }

    uint32_t get_best_ask() const
    {
        if (asks_l3_ == 0)
            return NULL_IDX;
        uint32_t l3_bit = __builtin_ctzll(asks_l3_);
        uint32_t l2_bit = __builtin_ctzll(asks_l2_[l3_bit]);
        uint32_t l1_idx = (l3_bit * 64) + l2_bit;
        uint32_t l1_bit = __builtin_ctzll(asks_l1_[l1_idx]);
        return (l1_idx * 64) + l1_bit;
    }

    // assumes the order is a buy order
    void add_buy_order(const Order &order)
    {
        uint32_t pool_idx = static_cast<uint32_t>(order.id & 0xFFFFFFFFULL);
        OrderNode &node = node_pool_[pool_idx];
        node.order = order;
        node.prev_idx = NULL_IDX;
        node.next_idx = NULL_IDX;

        OrderList &list = bids_[order.price];

        if (list.tail_idx == NULL_IDX)
        {
            list.head_idx = list.tail_idx = pool_idx;
            set_bids_bit(order.price);
        }
        else
        {
            node_pool_[list.tail_idx].next_idx = pool_idx;
            node.prev_idx = list.tail_idx;
            list.tail_idx = pool_idx;
        }

        list.total_quantity += order.quantity;
        list.order_count++;
    }

    // assumes the order is a sell order
    void add_sell_order(const Order &order)
    {
        uint32_t pool_idx = static_cast<uint32_t>(order.id & 0xFFFFFFFFULL);
        OrderNode &node = node_pool_[pool_idx];
        node.order = order;
        node.prev_idx = NULL_IDX;
        node.next_idx = NULL_IDX;

        OrderList &list = asks_[order.price];

        if (list.tail_idx == NULL_IDX)
        {
            list.head_idx = list.tail_idx = pool_idx;
            set_asks_bit(order.price);
        }
        else
        {
            node_pool_[list.tail_idx].next_idx = pool_idx;
            node.prev_idx = list.tail_idx;
            list.tail_idx = pool_idx;
        }

        list.total_quantity += order.quantity;
        list.order_count++;
    }

    // assumes the incoming order is a buy order
    uint32_t match_buy(Order &incoming)
    {
        uint32_t best_ask = get_best_ask();

        while (incoming.quantity > 0 && best_ask != NULL_IDX && best_ask <= incoming.price)
        {
            OrderList &list = asks_[best_ask];

            while (incoming.quantity > 0 && list.head_idx != NULL_IDX)
            {
                uint32_t resting_idx = list.head_idx;
                OrderNode &resting_node = node_pool_[resting_idx];
                Order &resting = resting_node.order;

                // Self-trade prevention check:
                if (incoming.user_id == resting.user_id)
                {
                    uint32_t rejected_qty = incoming.quantity;
                    incoming.quantity = 0;
                    return rejected_qty;
                }

                uint32_t fill_qty = std::min(incoming.quantity, resting.quantity);

                Trade trade;
                trade.company_id = incoming.company_id;
                trade.price = best_ask;
                trade.quantity = fill_qty;
                trade.buy_order_id = incoming.id;
                trade.sell_order_id = resting.id;
                trade.buyer_user_id = incoming.user_id;
                trade.seller_user_id = resting.user_id;
                trade.buyer_limit_price = incoming.price;

                incoming.quantity -= fill_qty;
                resting.quantity -= fill_qty;
                list.total_quantity -= fill_qty;

                if (trade_listener)
                    trade_listener->on_trade(trade);

                if (resting.quantity == 0)
                {
                    list.head_idx = resting_node.next_idx;
                    if (list.head_idx != NULL_IDX)
                        node_pool_[list.head_idx].prev_idx = NULL_IDX;
                    else
                        list.tail_idx = NULL_IDX;

                    list.order_count--;
                    resting_node.order.id = 0; // Mark inactive

                    free_indices_.push_back(resting_idx);
                }
            }

            if (asks_[best_ask].order_count == 0)
                clear_asks_bit(best_ask);

            best_ask = get_best_ask();
        }
        return 0;
    }
    
    // assumes the incoming order is a sell order
    uint32_t match_sell(Order &incoming)
    {
        uint32_t best_bid = get_best_bid();
        
        while (incoming.quantity > 0 && best_bid != NULL_IDX && best_bid >= incoming.price)
        {
            OrderList &list = bids_[best_bid];

            while (incoming.quantity > 0 && list.head_idx != NULL_IDX)
            {
                uint32_t resting_idx = list.head_idx;
                OrderNode &resting_node = node_pool_[resting_idx];
                Order &resting = resting_node.order;

                // Self-trade prevention check:
                if (incoming.user_id == resting.user_id)
                {
                    uint32_t rejected_qty = incoming.quantity;
                    incoming.quantity = 0;
                    return rejected_qty;
                }

                uint32_t fill_qty = std::min(incoming.quantity, resting.quantity);

                Trade trade;
                trade.company_id = incoming.company_id;
                trade.price = best_bid;
                trade.quantity = fill_qty;
                trade.buy_order_id = resting.id;
                trade.sell_order_id = incoming.id;
                trade.buyer_user_id = resting.user_id;
                trade.seller_user_id = incoming.user_id;
                trade.buyer_limit_price = resting.price;

                incoming.quantity -= fill_qty;
                resting.quantity -= fill_qty;
                list.total_quantity -= fill_qty;

                if (trade_listener)
                    trade_listener->on_trade(trade);

                if (resting.quantity == 0)
                {
                    list.head_idx = resting_node.next_idx;
                    if (list.head_idx != NULL_IDX)
                        node_pool_[list.head_idx].prev_idx = NULL_IDX;
                    else
                        list.tail_idx = NULL_IDX;

                    list.order_count--;
                    resting_node.order.id = 0; // Mark inactive

                    free_indices_.push_back(resting_idx);
                }
            }

            if (bids_[best_bid].order_count == 0)
                clear_bids_bit(best_bid);

            best_bid = get_best_bid();
        }
        
        return 0;
    }
};