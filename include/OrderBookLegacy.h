#pragma once

#include "IOrderBook.h"
#include "OrderBook.h"  // For Order, Trade, ITradeListener struct definitions
#include "Metrics.h"    // Include the new metrics header
#include <map>
#include <queue>
#include <memory>
#include <algorithm>
#include <iostream>
#include <chrono>       // For high-resolution clock

/**
 * OrderBookLegacy: Pre-5bb424a std::map-based implementation
 * This is the baseline for performance comparison.
 * Uses:
 * - std::map<double, ...> for price levels (O(log n) insertion)
 * - std::queue<Order> at each price level (FIFO)
 * - Floating-point prices (no integer cents)
 * - Dynamic std::unique_ptr<Order> allocation per order
 */

using PriceLevelLegacy = std::queue<std::shared_ptr<Order>>;

class OrderBookLegacy : public IOrderBook {
public:
    EngineMetrics* metrics_ = nullptr; // Initialize in constructor
    
    OrderBookLegacy() : trade_listener_(nullptr) {}

    void set_metrics(EngineMetrics* metrics) override {
        metrics_ = metrics;
    }

    EngineMetrics* get_metrics() const override {
        return metrics_;
    }

    void add_order(const Order& order_ref) override {
        if (metrics_) {
            metrics_->orders_submitted++;
        }

        Order order = order_ref;
        
        if (order.side) {  // BUY
            match_buy(order);
            if (order.quantity > 0) {
                insert_to_book(true, order);
            }
        } else {  // SELL
            match_sell(order);
            if (order.quantity > 0) {
                insert_to_book(false, order);
            }
        }
    }

    bool cancel_order(bool side, uint32_t price, uint64_t order_id, int64_t user_id) override {
        double double_price = price / 100.0;
        bool found = false;

        if (side) { // BUY -> cancel from bids_
            auto it = bids_.find(double_price);
            if (it == bids_.end()) return false;

            auto& queue = it->second;
            std::queue<std::shared_ptr<Order>> temp;
            
            while (!queue.empty()) {
                auto order_ptr = queue.front();
                queue.pop();

                if (order_ptr->id == order_id && order_ptr->user_id == user_id && order_ptr->side == side) {
                    found = true;
                } else {
                    temp.push(order_ptr);
                }
            }
            it->second = temp;
            if (it->second.empty()) {
                bids_.erase(it);
            }
        } else {    // SELL -> cancel from asks_
            auto it = asks_.find(double_price);
            if (it == asks_.end()) return false;

            auto& queue = it->second;
            std::queue<std::shared_ptr<Order>> temp;
            
            while (!queue.empty()) {
                auto order_ptr = queue.front();
                queue.pop();

                if (order_ptr->id == order_id && order_ptr->user_id == user_id && order_ptr->side == side) {
                    found = true;
                } else {
                    temp.push(order_ptr);
                }
            }
            it->second = temp;
            if (it->second.empty()) {
                asks_.erase(it);
            }
        }
        return found;
    }

    std::vector<DepthLevel> bid_depth(std::size_t limit = 20) const override {
        std::vector<DepthLevel> depth;
        
        for (auto it = bids_.rbegin(); it != bids_.rend() && depth.size() < limit; ++it) {
            uint32_t total_qty = 0;
            uint32_t order_count = 0;
            
            auto queue_copy = it->second;
            while (!queue_copy.empty()) {
                total_qty += queue_copy.front()->quantity;
                order_count++;
                queue_copy.pop();
            }
            
            depth.push_back({static_cast<uint32_t>(it->first * 100), total_qty, order_count});
        }
        
        return depth;
    }

    std::vector<DepthLevel> ask_depth(std::size_t limit = 20) const override {
        std::vector<DepthLevel> depth;
        
        for (auto it = asks_.begin(); it != asks_.end() && depth.size() < limit; ++it) {
            uint32_t total_qty = 0;
            uint32_t order_count = 0;
            
            auto queue_copy = it->second;
            while (!queue_copy.empty()) {
                total_qty += queue_copy.front()->quantity;
                order_count++;
                queue_copy.pop();
            }
            
            depth.push_back({static_cast<uint32_t>(it->first * 100), total_qty, order_count});
        }
        
        return depth;
    }

    std::vector<OrderSnapshot> bid_orders(std::size_t limit = 100) const override {
        std::vector<OrderSnapshot> snaps;
        
        for (auto it = bids_.rbegin(); it != bids_.rend() && snaps.size() < limit; ++it) {
            auto queue_copy = it->second;
            while (!queue_copy.empty() && snaps.size() < limit) {
                auto order_ptr = queue_copy.front();
                queue_copy.pop();
                snaps.push_back({order_ptr->id, order_ptr->user_id, order_ptr->timestamp, 
                                 order_ptr->price, order_ptr->quantity});
            }
        }
        
        return snaps;
    }

    std::vector<OrderSnapshot> ask_orders(std::size_t limit = 100) const override {
        std::vector<OrderSnapshot> snaps;
        
        for (auto it = asks_.begin(); it != asks_.end() && snaps.size() < limit; ++it) {
            auto queue_copy = it->second;
            while (!queue_copy.empty() && snaps.size() < limit) {
                auto order_ptr = queue_copy.front();
                queue_copy.pop();
                snaps.push_back({order_ptr->id, order_ptr->user_id, order_ptr->timestamp, 
                                 order_ptr->price, order_ptr->quantity});
            }
        }
        
        return snaps;
    }

    ITradeListener* get_trade_listener() override {
        return trade_listener_;
    }

    void set_trade_listener(ITradeListener* listener) override {
        trade_listener_ = listener;
    }

private:
    // Maps with std::greater for bids (descending), default ascending for asks
    std::map<double, PriceLevelLegacy, std::greater<double>> bids_;
    std::map<double, PriceLevelLegacy> asks_;
    
    ITradeListener* trade_listener_;

    void insert_to_book(bool side, const Order& order) {
        double double_price = order.price / 100.0;
        auto order_ptr = std::make_shared<Order>(order);

        if (side) { // BUY -> insert into bids_
            bids_[double_price].push(order_ptr);
        } else {    // SELL -> insert into asks_
            asks_[double_price].push(order_ptr);
        }
    }

    void match_buy(Order& incoming) {
        double double_price = incoming.price / 100.0;
        
        while (incoming.quantity > 0 && !asks_.empty()) {
            auto best_ask_it = asks_.begin();
            double best_ask_price = best_ask_it->first;
            
            if (best_ask_price > double_price) {
                break;  // Prices don't cross
            }
            
            auto& queue = best_ask_it->second;
            
            while (incoming.quantity > 0 && !queue.empty()) {
                auto resting_ptr = queue.front();
                queue.pop();
                
                uint32_t fill_qty = std::min(incoming.quantity, resting_ptr->quantity);
                
                Trade trade;
                trade.company_id = incoming.company_id;
                trade.price = static_cast<uint32_t>(best_ask_price * 100);
                trade.quantity = fill_qty;
                trade.buy_order_id = incoming.id;
                trade.sell_order_id = resting_ptr->id;
                trade.buyer_user_id = incoming.user_id;
                trade.seller_user_id = resting_ptr->user_id;

                // Update metrics if available
                if (metrics_) {
                    metrics_->orders_matched++;
                    // Client-to-engine latency for the incoming order
                    auto client_timestamp = std::chrono::time_point<std::chrono::steady_clock>(std::chrono::milliseconds(incoming.timestamp));
                    auto current_time = std::chrono::steady_clock::now();
                    auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(current_time - client_timestamp).count();
                    metrics_->add_latency(latency_us);
                }
                
                incoming.quantity -= fill_qty;
                resting_ptr->quantity -= fill_qty;
                
                if (trade_listener_) {
                    trade_listener_->on_trade(trade);
                }
                
                if (resting_ptr->quantity > 0) {
                    queue.push(resting_ptr);  // Re-queue partial fill
                }
            }
            
            if (queue.empty()) {
                asks_.erase(best_ask_it);
            }
        }
    }

    void match_sell(Order& incoming) {
        double double_price = incoming.price / 100.0;
        
        while (incoming.quantity > 0 && !bids_.empty()) {
            auto best_bid_it = bids_.begin();
            double best_bid_price = best_bid_it->first;
            
            if (best_bid_price < double_price) {
                break;  // Prices don't cross
            }
            
            auto& queue = best_bid_it->second;
            
            while (incoming.quantity > 0 && !queue.empty()) {
                auto resting_ptr = queue.front();
                queue.pop();
                
                uint32_t fill_qty = std::min(incoming.quantity, resting_ptr->quantity);
                
                Trade trade;
                trade.company_id = incoming.company_id;
                trade.price = static_cast<uint32_t>(best_bid_price * 100);
                trade.quantity = fill_qty;
                trade.buy_order_id = resting_ptr->id;
                trade.sell_order_id = incoming.id;
                trade.buyer_user_id = resting_ptr->user_id;
                trade.seller_user_id = incoming.user_id;

                // Update metrics if available
                if (metrics_) {
                    metrics_->orders_matched++;
                    // Client-to-engine latency for the incoming order
                    auto client_timestamp = std::chrono::time_point<std::chrono::steady_clock>(std::chrono::milliseconds(incoming.timestamp));
                    auto current_time = std::chrono::steady_clock::now();
                    auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(current_time - client_timestamp).count();
                    metrics_->add_latency(latency_us);
                }
                
                incoming.quantity -= fill_qty;
                resting_ptr->quantity -= fill_qty;
                
                if (trade_listener_) {
                    trade_listener_->on_trade(trade);
                }
                
                if (resting_ptr->quantity > 0) {
                    queue.push(resting_ptr);  // Re-queue partial fill
                }
            }
            
            if (queue.empty()) {
                bids_.erase(best_bid_it);
            }
        }
    }
};
