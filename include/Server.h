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
#include "OrderBook.h"

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;
using json          = nlohmann::json;

class Session;

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
    beast::flat_buffer buffer_;

    Session(tcp::socket socket, OrderBook& book, SessionRegistry& registry)
        : ws_(std::move(socket))
        , book_(book)
        , registry_(registry) {}

    void send(const std::string& msg) {
        try {
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

        // wire trade broadcast to all sessions
        book_.on_trade = [this](const Trade& t) {
            json msg;
            msg["type"]          = "trade";
            msg["price"]         = t.price;
            msg["quantity"]      = t.quantity;
            msg["buy_order_id"]  = t.buy_order_id;
            msg["sell_order_id"] = t.sell_order_id;
            registry_.broadcast(msg.dump());
        };

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
    void handle_message(const std::string& raw) {
        try {
            auto msg = json::parse(raw);

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

            book_.add_order(order);

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
        , ioc_(ioc) {}

    void run() {
        std::cout << "WebSocket server listening on port "
                  << acceptor_.local_endpoint().port() << "\n";
        while (true) {
            try {
                tcp::socket socket(ioc_);
                acceptor_.accept(socket);
                auto session = std::make_shared<Session>(
                    std::move(socket), book_, registry_
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
};