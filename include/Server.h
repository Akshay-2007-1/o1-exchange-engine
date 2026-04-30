#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <iostream>
#include "OrderBook.h"

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;
using json          = nlohmann::json;

// ─────────────────────────────────────────
//  One WebSocket session per connected client
// ─────────────────────────────────────────
class Session : public std::enable_shared_from_this<Session> {
public:
    websocket::stream<tcp::socket> ws_;
    OrderBook& book_;
    beast::flat_buffer buffer_;

    Session(tcp::socket socket, OrderBook& book)
        : ws_(std::move(socket)), book_(book) {}

    void run() {
        ws_.accept();

        book_.on_trade = [this](const Trade& t) {
            json msg;
            msg["type"]          = "trade";
            msg["price"]         = t.price;
            msg["quantity"]      = t.quantity;
            msg["buy_order_id"]  = t.buy_order_id;
            msg["sell_order_id"] = t.sell_order_id;
            ws_.write(net::buffer(msg.dump()));
        };

        while (true) {
            try {
                buffer_.clear();
                ws_.read(buffer_);
                handle_message(beast::buffers_to_string(buffer_.data()));
            } catch (const beast::system_error& e) {
                if (e.code() != websocket::error::closed)
                    std::cerr << "Session error: " << e.what() << "\n";
                break;
            }
        }
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

            std::cout << "Received order #" << order.id
                      << " " << (order.side == Side::BUY ? "BUY" : "SELL")
                      << " " << order.quantity
                      << " @ $" << order.price << "\n";

            book_.add_order(order);

        } catch (const std::exception& e) {
            std::cerr << "Bad message: " << e.what() << "\n";
        }
    }
};

// ─────────────────────────────────────────
//  The server — accepts connections in a loop
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
            tcp::socket socket(ioc_);
            acceptor_.accept(socket);
            std::make_shared<Session>(std::move(socket), book_)->run();
        }
    }

private:
    tcp::acceptor    acceptor_;
    OrderBook&       book_;
    net::io_context& ioc_;
};