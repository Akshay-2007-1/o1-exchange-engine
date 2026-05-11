#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <algorithm>

// ─────────────────────────────────────────
//  Constants & Bounds
// ─────────────────────────────────────────
constexpr uint32_t MAX_PRICE = 100000;                     // Cap at $999.99 (99,999 cents)
constexpr uint32_t BITMAP_L1_SIZE = (MAX_PRICE / 64) + 1;  // ~1563 elements
constexpr uint32_t BITMAP_L2_SIZE = (BITMAP_L1_SIZE / 64) + 1; // ~25 elements
// L3 is just a single uint64_t

struct Order {
    uint64_t id;
    uint64_t timestamp;
    uint32_t price;
    uint32_t quantity;
    uint16_t company_id;
    bool side; // true for BUY, false for SELL
};

struct Trade {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    uint32_t price;
    uint32_t quantity;
    uint16_t company_id;
};

// ─────────────────────────────────────────
//  OrderNode: Doubly-Linked List Node
// ─────────────────────────────────────────
struct OrderNode {
    Order order;
    OrderNode* prev = nullptr;
    OrderNode* next = nullptr;
};

// ─────────────────────────────────────────
//  OrderList: The Queue for a Price Level
// ─────────────────────────────────────────
struct OrderList {
    OrderNode* head = nullptr;
    OrderNode* tail = nullptr;
    uint32_t total_quantity = 0;
    uint32_t order_count = 0;
};

// ─────────────────────────────────────────
//  OrderBook: The O(1) Matching Engine
// ─────────────────────────────────────────
class OrderBook {
public:
    struct DepthLevel {
        uint32_t price;
        uint32_t quantity;
        uint32_t orders;
    };

    struct OrderSnapshot {
        uint64_t id;
        uint64_t timestamp;
        uint32_t price;
        uint32_t quantity;
    };

    std::function<void(const Trade&)> on_trade;

    void add_order(Order order) {
        if (order.price >= MAX_PRICE) return; // Bounds protection

        if (order.side) { // BUY
            match_buy(order);
            if (order.quantity > 0) insert_to_book(true, order);
        } else {          // SELL
            match_sell(order);
            if (order.quantity > 0) insert_to_book(false, order);
        }
    }

    bool cancel_order(bool side, uint32_t price, uint64_t order_id) {
        auto it = order_map_.find(order_id);
        if (it == order_map_.end()) return false;

        OrderNode* node = it->second.get();
        OrderList& list = side ? bids_[price] : asks_[price];

        // Stitch the doubly linked list back together in O(1)
        if (node->prev) node->prev->next = node->next;
        else list.head = node->next;

        if (node->next) node->next->prev = node->prev;
        else list.tail = node->prev;

        list.total_quantity -= node->order.quantity;
        list.order_count--;

        // If level is now completely empty, update the hierarchical bitmap
        if (list.order_count == 0) {
            clear_bit(side, price);
        }

        // Deallocate node
        order_map_.erase(it);
        return true;
    }

    std::vector<DepthLevel> bid_depth(std::size_t limit = 20) const {
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

    std::vector<DepthLevel> ask_depth(std::size_t limit = 20) const {
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

    std::vector<OrderSnapshot> bid_orders(std::size_t limit = 100) const {
        std::vector<OrderSnapshot> snaps;
        auto depth = bid_depth(limit); // Get active price levels
        for (const auto& level : depth) {
            OrderNode* curr = bids_[level.price].head;
            while (curr && snaps.size() < limit) {
                snaps.push_back({curr->order.id, curr->order.timestamp, curr->order.price, curr->order.quantity});
                curr = curr->next;
            }
            if (snaps.size() == limit) break;
        }
        return snaps;
    }

    std::vector<OrderSnapshot> ask_orders(std::size_t limit = 100) const {
        std::vector<OrderSnapshot> snaps;
        auto depth = ask_depth(limit);
        for (const auto& level : depth) {
            OrderNode* curr = asks_[level.price].head;
            while (curr && snaps.size() < limit) {
                snaps.push_back({curr->order.id, curr->order.timestamp, curr->order.price, curr->order.quantity});
                curr = curr->next;
            }
            if (snaps.size() == limit) break;
        }
        return snaps;
    }

private:
    // O(1) Arrays mapped strictly to price in cents
    OrderList bids_[MAX_PRICE];
    OrderList asks_[MAX_PRICE];

    // O(1) Lookup table for mid-queue deletions
    std::unordered_map<uint64_t, std::unique_ptr<OrderNode>> order_map_;

    // Hierarchical Bitmaps
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

        if (side) { // BIDS
            bids_l1_[l1_idx] |= (1ULL << l1_bit);
            bids_l2_[l2_idx] |= (1ULL << l2_bit);
            bids_l3_         |= (1ULL << l3_bit);
        } else {    // ASKS
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

        if (side) { // BIDS
            bids_l1_[l1_idx] &= ~(1ULL << l1_bit);
            if (bids_l1_[l1_idx] == 0) {
                bids_l2_[l2_idx] &= ~(1ULL << l2_bit);
                if (bids_l2_[l2_idx] == 0) {
                    bids_l3_ &= ~(1ULL << l3_bit);
                }
            }
        } else {    // ASKS
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
        auto node = std::make_unique<OrderNode>();
        node->order = order;
        OrderNode* raw_ptr = node.get();
        order_map_[order.id] = std::move(node);

        OrderList& list = side ? bids_[order.price] : asks_[order.price];

        if (list.tail == nullptr) {
            list.head = list.tail = raw_ptr;
            set_bit(side, order.price); // First order at this price activates the bit
        } else {
            list.tail->next = raw_ptr;
            raw_ptr->prev = list.tail;
            list.tail = raw_ptr;
        }

        list.total_quantity += order.quantity;
        list.order_count++;
    }

    void match_buy(Order& incoming) {
        int32_t best_ask = get_best_ask();
        while (incoming.quantity > 0 && best_ask != -1 && best_ask <= incoming.price) {
            execute_match(incoming, asks_[best_ask], best_ask, false);
            if (asks_[best_ask].order_count == 0) clear_bit(false, best_ask);
            best_ask = get_best_ask(); // re-fetch in O(1)
        }
    }

    void match_sell(Order& incoming) {
        int32_t best_bid = get_best_bid();
        while (incoming.quantity > 0 && best_bid != -1 && best_bid >= incoming.price) {
            execute_match(incoming, bids_[best_bid], best_bid, true);
            if (bids_[best_bid].order_count == 0) clear_bit(true, best_bid);
            best_bid = get_best_bid(); // re-fetch in O(1)
        }
    }

    void execute_match(Order& incoming, OrderList& list, uint32_t exec_price, bool matching_against_bids) {
        while (incoming.quantity > 0 && list.head != nullptr) {
            OrderNode* resting_node = list.head;
            Order& resting = resting_node->order;

            uint32_t fill_qty = std::min(incoming.quantity, resting.quantity);

            Trade trade;
            trade.company_id    = incoming.company_id;
            trade.price         = exec_price;
            trade.quantity      = fill_qty;
            trade.buy_order_id  = incoming.side ? incoming.id : resting.id;
            trade.sell_order_id = !incoming.side ? incoming.id : resting.id;

            incoming.quantity    -= fill_qty;
            resting.quantity     -= fill_qty;
            list.total_quantity  -= fill_qty;

            if (on_trade) on_trade(trade);

            if (resting.quantity == 0) {
                // Unlink head
                list.head = resting_node->next;
                if (list.head) list.head->prev = nullptr;
                else list.tail = nullptr;
                
                list.order_count--;
                order_map_.erase(resting.id); // Triggers unique_ptr cleanup
            }
        }
    }
};