#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <iostream>
#include <set>
#include <mutex>
#include <thread>
#include <vector>
#include "OrderBook.h"

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;
using json          = nlohmann::json;

class Session;

inline json trade_to_json(const Trade& t) {
    json msg;
    msg["type"]          = "trade";
    msg["price"]         = t.price;
    msg["quantity"]      = t.quantity;
    msg["buy_order_id"]  = t.buy_order_id;
    msg["sell_order_id"] = t.sell_order_id;
    return msg;
}

inline json depth_to_json(const std::vector<OrderBook::DepthLevel>& depth) {
    json levels = json::array();
    for (const auto& level : depth) {
        levels.push_back({
            {"price", level.price},
            {"quantity", level.quantity},
            {"orders", level.orders}
        });
    }
    return levels;
}

inline json orders_to_json(const std::vector<OrderBook::OrderSnapshot>& orders) {
    json rows = json::array();
    for (const auto& order : orders) {
        rows.push_back({
            {"id", order.id},
            {"price", order.price},
            {"quantity", order.quantity},
            {"timestamp", order.timestamp}
        });
    }
    return rows;
}

inline json book_to_json(const OrderBook& book) {
    return {
        {"type", "book"},
        {"bids", depth_to_json(book.bid_depth())},
        {"asks", depth_to_json(book.ask_depth())},
        {"buy_orders", orders_to_json(book.bid_orders())},
        {"sell_orders", orders_to_json(book.ask_orders())}
    };
}

inline json snapshot_to_json(const OrderBook& book, const json& history) {
    return {
        {"type", "snapshot"},
        {"trades", history.value("trades", json::array())},
        {"bids", depth_to_json(book.bid_depth())},
        {"asks", depth_to_json(book.ask_depth())},
        {"buy_orders", orders_to_json(book.bid_orders())},
        {"sell_orders", orders_to_json(book.ask_orders())}
    };
}

class TradeHistory {
public:
    void record(const Trade& trade) {
        std::lock_guard<std::mutex> lock(mutex_);
        trades_.push_back(trade);
        if (trades_.size() > max_trades_) {
            trades_.erase(trades_.begin());
        }
    }

    json to_json() const {
        std::lock_guard<std::mutex> lock(mutex_);
        json trades = json::array();

        for (auto it = trades_.rbegin(); it != trades_.rend(); ++it) {
            trades.push_back(trade_to_json(*it));
        }

        return {
            {"type", "history"},
            {"trades", trades}
        };
    }

private:
    static constexpr std::size_t max_trades_ = 100;
    mutable std::mutex mutex_;
    std::vector<Trade> trades_;
};

// ─────────────────────────────────────────
//  Registry: tracks all live sessions
// ─────────────────────────────────────────
class SessionRegistry {
public:
    void add(std::shared_ptr<Session> s) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.insert(s);
    }

    void remove(std::shared_ptr<Session> s) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(s);
    }

    void broadcast(const std::string& msg);

private:
    std::set<std::shared_ptr<Session>> sessions_;
    std::mutex mutex_;
};

// ─────────────────────────────────────────
//  One session per connected client
// ─────────────────────────────────────────
class Session : public std::enable_shared_from_this<Session> {
public:
    websocket::stream<tcp::socket> ws_;
    OrderBook& book_;
    SessionRegistry& registry_;
    TradeHistory& history_;
    std::mutex& book_mutex_;
    beast::flat_buffer buffer_;
    std::mutex write_mutex_;

    Session(tcp::socket socket, OrderBook& book, SessionRegistry& registry, TradeHistory& history, std::mutex& book_mutex)
        : ws_(std::move(socket))
        , book_(book)
        , registry_(registry)
        , history_(history)
        , book_mutex_(book_mutex) {}

    void send(const std::string& msg) {
        try {
            std::lock_guard<std::mutex> lock(write_mutex_);
            if (ws_.is_open()) {
                ws_.write(net::buffer(msg));
            }
        } catch (const std::exception& e) {
            std::cerr << "Send error: " << e.what() << "\n";
        }
    }

    void run() {
        // accept handshake
        try {
            ws_.accept();
        } catch (const std::exception& e) {
            std::cerr << "Accept error: " << e.what() << "\n";
            return;
        }

        registry_.add(shared_from_this());
        send_snapshot();

        // read loop
        while (true) {
            try {
                buffer_.clear();
                ws_.read(buffer_);
                handle_message(beast::buffers_to_string(buffer_.data()));
            } catch (const beast::system_error& e) {
                if (e.code() != websocket::error::closed &&
                    e.code() != boost::asio::error::eof &&
                    e.code() != boost::asio::error::connection_reset) {
                    std::cerr << "Session error: " << e.what() << "\n";
                }
                break;  // client disconnected cleanly — exit loop
            } catch (const std::exception& e) {
                std::cerr << "Unexpected session error: " << e.what() << "\n";
                break;
            }
        }

        registry_.remove(shared_from_this());
    }

private:
    void send_snapshot() {
        json history = history_.to_json();
        std::lock_guard<std::mutex> lock(book_mutex_);
        send(snapshot_to_json(book_, history).dump());
    }

    void handle_message(const std::string& raw) {
        try {
            auto msg = json::parse(raw);

            if (msg.value("type", "") == "snapshot") {
                send_snapshot();
                return;
            }

            Order order;
            order.id        = msg["id"].get<uint64_t>();
            order.side      = (msg["side"] == "BUY") ? Side::BUY : Side::SELL;
            order.price     = msg["price"].get<double>();
            order.quantity  = msg["quantity"].get<uint32_t>();
            order.timestamp = msg["timestamp"].get<uint64_t>();

            std::cout << "Order #" << order.id
                      << " " << (order.side == Side::BUY ? "BUY" : "SELL")
                      << " " << order.quantity
                      << " @ $" << order.price << "\n";

            {
                std::lock_guard<std::mutex> lock(book_mutex_);
                book_.add_order(order);
                registry_.broadcast(book_to_json(book_).dump());
            }

        } catch (const std::exception& e) {
            std::cerr << "Bad message: " << e.what() << "\n";
        }
    }
};

// broadcast defined after Session
inline void SessionRegistry::broadcast(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : sessions_) {
        s->send(msg);
    }
}

// ─────────────────────────────────────────
//  Server: accepts connections in a loop
//  each client gets its own thread
// ─────────────────────────────────────────
class Server {
public:
    Server(net::io_context& ioc, unsigned short port, OrderBook& book)
        : acceptor_(ioc, tcp::endpoint(tcp::v4(), port))
        , book_(book)
        , ioc_(ioc) {
        book_.on_trade = [this](const Trade& t) {
            history_.record(t);
            registry_.broadcast(trade_to_json(t).dump());
        };
    }

    void run() {
        std::cout << "WebSocket server listening on port "
                  << acceptor_.local_endpoint().port() << "\n";
        while (true) {
            try {
                tcp::socket socket(ioc_);
                acceptor_.accept(socket);
                auto session = std::make_shared<Session>(
                    std::move(socket), book_, registry_, history_, book_mutex_
                );
                std::thread([session]() { session->run(); }).detach();
            } catch (const std::exception& e) {
                std::cerr << "Accept loop error: " << e.what() << "\n";
            }
        }
    }

private:
    tcp::acceptor    acceptor_;
    OrderBook&       book_;
    net::io_context& ioc_;
    SessionRegistry  registry_;
    TradeHistory     history_;
    std::mutex       book_mutex_;
};
