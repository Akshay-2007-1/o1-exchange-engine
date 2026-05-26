#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono> // For high-resolution clock
#include "IOrderBook.h"
#include "Metrics.h" // Include the new metrics header

constexpr uint32_t MAX_PRICE = 100000;                     
constexpr uint32_t BITMAP_L1_SIZE = (MAX_PRICE / 64) + 1;  
constexpr uint32_t BITMAP_L2_SIZE = (BITMAP_L1_SIZE / 64) + 1; 
constexpr uint32_t MAX_ORDERS = 1000000;                   
constexpr uint32_t NULL_IDX = -1;                          

#pragma pack(push, 1)
struct Order {
    uint64_t id;         // 8 bytes
    int64_t  user_id;    // 8 bytes - Embedded to remove external hash map
    uint64_t timestamp;  // 8 bytes
    uint32_t price;      // 4 bytes
    uint32_t quantity;   // 4 bytes
    uint16_t company_id; // 2 bytes
    bool     side;       // 1 byte (true for BUY, false for SELL)
    uint8_t  padding[5]; // 5 bytes padding -> Total 40 bytes
};

struct OrderNode {
    Order order;         // 40 bytes
    uint32_t prev_idx;   // 4 bytes
    uint32_t next_idx;   // 4 bytes
    uint8_t  padding[16];// 16 bytes padding -> Total 64 bytes
};
#pragma pack(pop)

static_assert(sizeof(OrderNode) == 64, "OrderNode must be exactly 64 bytes to prevent false sharing and cache straddling");

struct Trade {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    int64_t  buyer_user_id;
    int64_t  seller_user_id;
    uint32_t price;
    uint32_t quantity;
    uint16_t company_id;
};

class ITradeListener {
public:
    virtual ~ITradeListener() = default;
    virtual void on_trade(const Trade& trade) = 0;
};

struct OrderList {
    uint32_t head_idx = NULL_IDX;
    uint32_t tail_idx = NULL_IDX;
    uint32_t total_quantity = 0;
    uint32_t order_count = 0;
};

class OrderBook : public IOrderBook {
public:
    ITradeListener* trade_listener = nullptr;
    EngineMetrics* metrics_ = nullptr; // Initialize in constructor

    OrderBook() {
        node_pool_.resize(MAX_ORDERS);
        free_indices_.reserve(MAX_ORDERS);

        for (uint32_t i = 0; i < MAX_ORDERS; ++i) {
            free_indices_.push_back(MAX_ORDERS - 1 - i); 
        }

        order_map_array_.resize(MAX_ORDERS, NULL_IDX);
    }

    void set_metrics(EngineMetrics* metrics) override {
        metrics_ = metrics;
    }

    EngineMetrics* get_metrics() const override {
        return metrics_;
    }

    void add_order(const Order& order_ref) override {
        if (order_ref.price >= MAX_PRICE) [[unlikely]] return; 

        if (metrics_) {
            metrics_->orders_submitted++;
        }

        Order order = order_ref;

        if (order.side) { 
            match_buy(order);
            if (order.quantity > 0) insert_to_book(true, order);
        } else {          
            match_sell(order);
            if (order.quantity > 0) insert_to_book(false, order);
        }
    }
    bool cancel_order(bool side, uint32_t price, uint64_t order_id, int64_t user_id) override {
        uint32_t target_idx = order_map_array_[order_id % MAX_ORDERS];
        if (target_idx == NULL_IDX) return false;

        OrderNode& node = node_pool_[target_idx];
        
        if (node.order.id != order_id || node.order.user_id != user_id || node.order.side != side) [[unlikely]] return false;

        OrderList& list = side ? bids_[price] : asks_[price];

        if (node.prev_idx != NULL_IDX) node_pool_[node.prev_idx].next_idx = node.next_idx;
        else list.head_idx = node.next_idx;

        if (node.next_idx != NULL_IDX) node_pool_[node.next_idx].prev_idx = node.prev_idx;
        else list.tail_idx = node.prev_idx;

        list.total_quantity -= node.order.quantity;
        list.order_count--;

        if (list.order_count == 0) {
            clear_bit(side, price);
        }

        free_indices_.push_back(target_idx);
        order_map_array_[order_id % MAX_ORDERS] = NULL_IDX; 
        
        return true;
    }

    std::vector<DepthLevel> bid_depth(std::size_t limit = 20) const override {
        std::vector<DepthLevel> depth;
        uint64_t l3_copy = bids_l3_;
        
        while (l3_copy && depth.size() < limit) {
            uint32_t l3_bit = 63 - __builtin_clzll(l3_copy);
            uint64_t l2_copy = bids_l2_[l3_bit];
            
            while (l2_copy && depth.size() < limit) {
                uint32_t l2_bit = 63 - __builtin_clzll(l2_copy);
                uint32_t l1_idx = (l3_bit * 64) + l2_bit;
                uint64_t l1_copy = bids_l1_[l1_idx];
                
                while (l1_copy && depth.size() < limit) {
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

    std::vector<DepthLevel> ask_depth(std::size_t limit = 20) const override {
        std::vector<DepthLevel> depth;
        uint64_t l3_copy = asks_l3_;
        
        while (l3_copy && depth.size() < limit) {
            uint32_t l3_bit = __builtin_ctzll(l3_copy);
            uint64_t l2_copy = asks_l2_[l3_bit];
            
            while (l2_copy && depth.size() < limit) {
                uint32_t l2_bit = __builtin_ctzll(l2_copy);
                uint32_t l1_idx = (l3_bit * 64) + l2_bit;
                uint64_t l1_copy = asks_l1_[l1_idx];
                
                while (l1_copy && depth.size() < limit) {
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

    std::vector<OrderSnapshot> bid_orders(std::size_t limit = 100) const override {
        std::vector<OrderSnapshot> snaps;
        auto depth = bid_depth(limit); 
        for (const auto& level : depth) {
            uint32_t curr_idx = bids_[level.price].head_idx;
            while (curr_idx != NULL_IDX && snaps.size() < limit) {
                const Order& o = node_pool_[curr_idx].order;
                snaps.push_back({o.id, o.user_id, o.timestamp, o.price, o.quantity});
                curr_idx = node_pool_[curr_idx].next_idx;
            }
            if (snaps.size() == limit) break;
        }
        return snaps;
    }

    std::vector<OrderSnapshot> ask_orders(std::size_t limit = 100) const override {
        std::vector<OrderSnapshot> snaps;
        auto depth = ask_depth(limit);
        for (const auto& level : depth) {
            uint32_t curr_idx = asks_[level.price].head_idx;
            while (curr_idx != NULL_IDX && snaps.size() < limit) {
                const Order& o = node_pool_[curr_idx].order;
                snaps.push_back({o.id, o.user_id, o.timestamp, o.price, o.quantity});
                curr_idx = node_pool_[curr_idx].next_idx;
            }
            if (snaps.size() == limit) break;
        }
        return snaps;
    }

    ITradeListener* get_trade_listener() override {
        return trade_listener;
    }

    void set_trade_listener(ITradeListener* listener) override {
        trade_listener = listener;
    }

private:
    std::vector<OrderNode> node_pool_;
    std::vector<uint32_t> free_indices_;
    std::vector<uint32_t> order_map_array_;

    OrderList bids_[MAX_PRICE];
    OrderList asks_[MAX_PRICE];

    uint64_t bids_l1_[BITMAP_L1_SIZE] = {0};
    uint64_t bids_l2_[BITMAP_L2_SIZE] = {0};
    uint64_t bids_l3_ = 0;

    uint64_t asks_l1_[BITMAP_L1_SIZE] = {0};
    uint64_t asks_l2_[BITMAP_L2_SIZE] = {0};
    uint64_t asks_l3_ = 0;

    void set_bit(bool side, uint32_t price) {
        uint32_t l1_idx = price / 64;
        uint32_t l1_bit = price % 64;
        uint32_t l2_idx = l1_idx / 64;
        uint32_t l2_bit = l1_idx % 64;
        uint32_t l3_bit = l2_idx;

        if (side) { 
            bids_l1_[l1_idx] |= (1ULL << l1_bit);
            bids_l2_[l2_idx] |= (1ULL << l2_bit);
            bids_l3_         |= (1ULL << l3_bit);
        } else {    
            asks_l1_[l1_idx] |= (1ULL << l1_bit);
            asks_l2_[l2_idx] |= (1ULL << l2_bit);
            asks_l3_         |= (1ULL << l3_bit);
        }
    }

    void clear_bit(bool side, uint32_t price) {
        uint32_t l1_idx = price / 64;
        uint32_t l1_bit = price % 64;
        uint32_t l2_idx = l1_idx / 64;
        uint32_t l2_bit = l1_idx % 64;
        uint32_t l3_bit = l2_idx;

        if (side) { 
            bids_l1_[l1_idx] &= ~(1ULL << l1_bit);
            if (bids_l1_[l1_idx] == 0) {
                bids_l2_[l2_idx] &= ~(1ULL << l2_bit);
                if (bids_l2_[l2_idx] == 0) {
                    bids_l3_ &= ~(1ULL << l3_bit);
                }
            }
        } else {    
            asks_l1_[l1_idx] &= ~(1ULL << l1_bit);
            if (asks_l1_[l1_idx] == 0) {
                asks_l2_[l2_idx] &= ~(1ULL << l2_bit);
                if (asks_l2_[l2_idx] == 0) {
                    asks_l3_ &= ~(1ULL << l3_bit);
                }
            }
        }
    }

    int32_t get_best_bid() const {
        if (bids_l3_ == 0) return -1;
        uint32_t l3_bit = 63 - __builtin_clzll(bids_l3_);
        uint32_t l2_bit = 63 - __builtin_clzll(bids_l2_[l3_bit]);
        uint32_t l1_idx = (l3_bit * 64) + l2_bit;
        uint32_t l1_bit = 63 - __builtin_clzll(bids_l1_[l1_idx]);
        return (l1_idx * 64) + l1_bit;
    }

    int32_t get_best_ask() const {
        if (asks_l3_ == 0) return -1;
        uint32_t l3_bit = __builtin_ctzll(asks_l3_);
        uint32_t l2_bit = __builtin_ctzll(asks_l2_[l3_bit]);
        uint32_t l1_idx = (l3_bit * 64) + l2_bit;
        uint32_t l1_bit = __builtin_ctzll(asks_l1_[l1_idx]);
        return (l1_idx * 64) + l1_bit;
    }

    void insert_to_book(bool side, const Order& order) {
        if (free_indices_.empty()) [[unlikely]] {
            std::cerr << "CRITICAL ERROR: Order pool exhausted.\n";
            return; 
        }

        uint32_t new_idx = free_indices_.back();
        free_indices_.pop_back();

        OrderNode& node = node_pool_[new_idx];
        node.order = order;
        node.prev_idx = NULL_IDX;
        node.next_idx = NULL_IDX;

        order_map_array_[order.id % MAX_ORDERS] = new_idx;

        OrderList& list = side ? bids_[order.price] : asks_[order.price];

        if (list.tail_idx == NULL_IDX) {
            list.head_idx = list.tail_idx = new_idx;
            set_bit(side, order.price);
        } else {
            node_pool_[list.tail_idx].next_idx = new_idx;
            node.prev_idx = list.tail_idx;
            list.tail_idx = new_idx;
        }

        list.total_quantity += order.quantity;
        list.order_count++;
    }

    void match_buy(Order& incoming) {
        int32_t best_ask = get_best_ask();
        while (incoming.quantity > 0 && best_ask != -1 && best_ask <= incoming.price) {
            execute_match(incoming, asks_[best_ask], best_ask, false);
            if (asks_[best_ask].order_count == 0) clear_bit(false, best_ask);
            best_ask = get_best_ask(); 
        }
    }

    void match_sell(Order& incoming) {
        int32_t best_bid = get_best_bid();
        while (incoming.quantity > 0 && best_bid != -1 && best_bid >= incoming.price) {
            execute_match(incoming, bids_[best_bid], best_bid, true);
            if (bids_[best_bid].order_count == 0) clear_bit(true, best_bid);
            best_bid = get_best_bid(); 
        }
    }

    void execute_match(Order& incoming, OrderList& list, uint32_t exec_price, bool matching_against_bids) {
        while (incoming.quantity > 0 && list.head_idx != NULL_IDX) {
            uint32_t resting_idx = list.head_idx;
            OrderNode& resting_node = node_pool_[resting_idx];
            Order& resting = resting_node.order;

            uint32_t fill_qty = std::min(incoming.quantity, resting.quantity);

            Trade trade;
            trade.company_id     = incoming.company_id;
            trade.price          = exec_price;
            trade.quantity       = fill_qty;
            trade.buy_order_id   = incoming.side ? incoming.id : resting.id;
            trade.sell_order_id  = !incoming.side ? incoming.id : resting.id;
            trade.buyer_user_id  = incoming.side ? incoming.user_id : resting.user_id;
            trade.seller_user_id = !incoming.side ? incoming.user_id : resting.user_id;

            // Update metrics if available
            if (metrics_) {
                metrics_->orders_matched++;
                // Client-to-engine latency for the incoming order
                auto client_timestamp = std::chrono::time_point<std::chrono::steady_clock>(std::chrono::milliseconds(incoming.timestamp));
                auto current_time = std::chrono::steady_clock::now();
                auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(current_time - client_timestamp).count();
                metrics_->add_latency(latency_us);
            }

            incoming.quantity    -= fill_qty;
            resting.quantity     -= fill_qty;
            list.total_quantity  -= fill_qty;

            if (trade_listener) trade_listener->on_trade(trade);

            if (resting.quantity == 0) {
                list.head_idx = resting_node.next_idx;
                if (list.head_idx != NULL_IDX) node_pool_[list.head_idx].prev_idx = NULL_IDX;
                else list.tail_idx = NULL_IDX;
                
                list.order_count--;
                order_map_array_[resting.id % MAX_ORDERS] = NULL_IDX; 
                
                free_indices_.push_back(resting_idx);
            }
        }
    }
};