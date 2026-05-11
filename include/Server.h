#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <iostream>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>
#include "Market.h"
#include "Database.h"

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;
using json          = nlohmann::json;

class Session;

inline json trade_to_json(const Trade& t) {
    json msg;
    msg["type"]          = "trade";
    msg["company_id"]    = t.company_id;
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

inline json companies_to_json(const std::vector<Company>& companies) {
    json rows = json::array();
    for (const auto& company : companies) {
        rows.push_back({
            {"id", company.id},
            {"symbol", company.symbol},
            {"name", company.name},
            {"total_shares", company.total_shares}
        });
    }
    return rows;
}

inline json book_to_json(const InstrumentState& instrument) {
    return {
        {"type", "book"},
        {"company_id", instrument.company.id},
        {"company_name", instrument.company.name},
        {"company_symbol", instrument.company.symbol},
        {"total_shares", instrument.company.total_shares},
        {"bids", depth_to_json(instrument.book.bid_depth())},
        {"asks", depth_to_json(instrument.book.ask_depth())},
        {"buy_orders", orders_to_json(instrument.book.bid_orders())},
        {"sell_orders", orders_to_json(instrument.book.ask_orders())}
    };
}

inline json snapshot_to_json(const InstrumentState& instrument, const json& history, const std::vector<Company>& companies) {
    return {
        {"type", "snapshot"},
        {"company_id", instrument.company.id},
        {"company_name", instrument.company.name},
        {"company_symbol", instrument.company.symbol},
        {"total_shares", instrument.company.total_shares},
        {"companies", companies_to_json(companies)},
        {"trades", history.value("trades", json::array())},
        {"bids", depth_to_json(instrument.book.bid_depth())},
        {"asks", depth_to_json(instrument.book.ask_depth())},
        {"buy_orders", orders_to_json(instrument.book.bid_orders())},
        {"sell_orders", orders_to_json(instrument.book.ask_orders())}
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

class Session : public std::enable_shared_from_this<Session> {
public:
    websocket::stream<tcp::socket> ws_;
    MarketState& market_;
    Database& db_;
    std::map<uint64_t, int64_t>& order_user_map_;
    std::mutex& order_map_mutex_;
    SessionRegistry& registry_;
    TradeHistory& history_;
    std::atomic<uint64_t>& next_order_id_;
    uint16_t default_company_id_;
    beast::flat_buffer buffer_;
    std::mutex write_mutex_;

    Session(tcp::socket socket, MarketState& market, SessionRegistry& registry, 
        TradeHistory& history, std::atomic<uint64_t>& next_order_id, Database& db, std::map<uint64_t, int64_t>& order_user_map,
        std::mutex& order_map_mutex)
        : ws_(std::move(socket))
        , market_(market)
        , registry_(registry)
        , history_(history)
        , next_order_id_(next_order_id)
        , default_company_id_(market.default_company_id())
        , db_(db)
        , order_user_map_(order_user_map)
        , order_map_mutex_(order_map_mutex) {}

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
        try {
            ws_.accept();
        } catch (const std::exception& e) {
            std::cerr << "Accept error: " << e.what() << "\n";
            return;
        }

        registry_.add(shared_from_this());
        send_snapshot(default_company_id_);

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
                break;
            } catch (const std::exception& e) {
                std::cerr << "Unexpected session error: " << e.what() << "\n";
                break;
            }
        }

        registry_.remove(shared_from_this());
    }

private:
    void send_snapshot(uint16_t company_id) {
        std::string payload;
        const InstrumentState* instrument = market_.find_instrument(company_id);
        if (instrument == nullptr) instrument = market_.find_instrument(default_company_id_);
        if (instrument == nullptr) return;

        std::lock_guard<std::mutex> lock(instrument->mutex);
        json history = history_.to_json();
        payload = snapshot_to_json(*instrument, history, market_.companies()).dump();
        send(payload);
    }

    void handle_register(const json& msg) {
        std::string username = msg.value("username", "");
        std::string password = msg.value("password", "");
        std::string role     = msg.value("role", "trader");

        if (username.empty() || password.empty()) {
            send(json{{"type", "error"}, {"message", "Username and password are required"}} .dump());
            return;
        }

        if (password.size() < 6) {
            send(json{{"type", "error"}, {"message", "Password must be at least 6 characters"}} .dump());
            return;
        }

        auto result = db_.create_user(username, password, role);

        if (!result.ok) {
            send(json{{"type", "error"}, {"message", result.error}} .dump());
            return;
        }

        send(json{
            {"type", "registered"},
            {"user_id", result.id},
            {"username", username},
            {"message", "Account created successfully"}
        }.dump());
    }

    void handle_login(const json& msg) {
        std::string username = msg.value("username", "");
        std::string password = msg.value("password", "");

        if (username.empty() || password.empty()) {
            send(json{{"type", "error"}, {"message", "Username and password are required"}} .dump());
            return;
        }

        auto result = db_.login(username, password);

        if (!result.ok) {
            send(json{{"type", "error"}, {"message", result.error}} .dump());
            return;
        }

        std::string token = result.error;

        send(json{
            {"type", "logged_in"},
            {"token", token},
            {"user_id", result.id},
            {"username", username},
            {"message", "Login successful"}
        }.dump());
    }

    void handle_message(const std::string& raw) {
        try {
            auto msg = json::parse(raw);

            if (!msg.is_object()) return;

            if (msg.value("type", "") == "register") {
                handle_register(msg);
                return;
            }

            if (msg.value("type", "") == "login") {
                handle_login(msg);
                return;
            }

            if (msg.value("type", "") == "snapshot") {
                send_snapshot(msg.value("company_id", default_company_id_));
                return;
            }

            if (msg.value("type", "") == "cancel") {
                const uint16_t company_id = msg.value("company_id", default_company_id_);
                InstrumentState* instrument = market_.find_instrument(company_id);
                if (instrument == nullptr) return;

                const bool side = (msg["side"] == "BUY");
                const uint32_t price = msg["price"].get<uint32_t>();
                const uint64_t order_id = msg["order_id"].get<uint64_t>();

                std::cout << "Cancel order #" << order_id
                          << " [" << instrument->company.symbol << "]"
                          << " " << (side ? "BUY" : "SELL")
                          << " @ $" << price << "\n";

                std::lock_guard<std::mutex> lock(instrument->mutex);
                if (instrument->book.cancel_order(side, price, order_id)) {
                    registry_.broadcast(book_to_json(*instrument).dump());
                }
                return;
            }

            if (msg.value("type", "") == "order") {
                const uint16_t company_id = msg.value("company_id", default_company_id_);
                const bool side = (msg["side"] == "BUY");
                const uint32_t price = msg["price"].get<uint32_t>();
                
                // Security Check for Max Price Array bounds
                if (price >= 100000) {
                     send(json{{"type", "error"}, {"message", "Price exceeds maximum allowed ($999.99)."}} .dump());
                     return;
                }
                
                std::string token = msg.value("token", "");
                if (token.empty()) {
                    send(json{{"type", "error"}, {"message", "Authentication required. Please log in."}} .dump());
                    return;
                }

                auto session_result = db_.validate_session(token);
                if (!session_result.ok) {
                    send(json{{"type", "error"}, {"message", session_result.error}} .dump());
                    return;
                }

                int64_t user_id = session_result.id;

                if (side) { // BUY
                    double required_cash = static_cast<double>(price) * msg["quantity"].get<uint32_t>();
                    auto check = db_.reserve_cash(user_id, required_cash);
                    if (!check.ok) {
                        send(json{{"type", "error"}, {"message", check.error}} .dump());
                        return;
                    }
                } else {
                    auto check = db_.reserve_shares(user_id, company_id, msg["quantity"].get<uint32_t>());
                    if (!check.ok) {
                        send(json{{"type", "error"}, {"message", check.error}} .dump());
                        return;
                    }
                }

                InstrumentState* instrument = market_.find_instrument(company_id);
                if (instrument == nullptr) return;

                Order order;
                order.id        = next_order_id_.fetch_add(1);

                {
                    std::lock_guard<std::mutex> lock(order_map_mutex_);
                    order_user_map_[order.id] = user_id;
                }

                order.company_id = instrument->company.id;
                order.side      = side;
                order.price     = price;
                order.quantity  = msg["quantity"].get<uint32_t>();
                order.timestamp = msg["timestamp"].get<uint64_t>();

                std::cout << "Order #" << order.id
                          << " [" << instrument->company.symbol << "]"
                          << " " << (order.side ? "BUY" : "SELL")
                          << " " << order.quantity
                          << " @ $" << order.price << "\n";

                std::lock_guard<std::mutex> lock(instrument->mutex);
                instrument->book.add_order(order);
                registry_.broadcast(book_to_json(*instrument).dump());
            }

        } catch (const std::exception& e) {
            std::cerr << "Bad message: " << e.what() << "\n";
        }
    }
};

inline void SessionRegistry::broadcast(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : sessions_) {
        s->send(msg);
    }
}

class Server {
public:
    Server(net::io_context& ioc, unsigned short port, MarketState& market)
        : acceptor_(ioc, tcp::endpoint(tcp::v4(), port))
        , market_(market)
        , ioc_(ioc)
        , db_("exchange.db") {
        for (const auto& company : market_.companies()) {
            InstrumentState* instrument = market_.find_instrument(company.id);
            if (instrument == nullptr) continue;

            instrument->book.on_trade = [this](const Trade& t) {
                history_.record(t);
                registry_.broadcast(trade_to_json(t).dump());

                std::lock_guard<std::mutex> lock(order_map_mutex_);
                auto buyer_it  = order_user_map_.find(t.buy_order_id);
                auto seller_it = order_user_map_.find(t.sell_order_id);

                if (buyer_it != order_user_map_.end() &&
                    seller_it != order_user_map_.end()) {
                    db_.settle_trade(
                        buyer_it->second,
                        seller_it->second,
                        t.company_id,
                        t.quantity,
                        t.price
                    );
                }
            };
        }
    }

    void run() {
        std::cout << "WebSocket server listening on port "
                  << acceptor_.local_endpoint().port() << "\n";
        while (true) {
            try {
                tcp::socket socket(ioc_);
                acceptor_.accept(socket);
                auto session = std::make_shared<Session>(
                    std::move(socket), market_, registry_, history_, next_order_id_, db_, order_user_map_, order_map_mutex_
                );
                std::thread([session]() { session->run(); }).detach();
            } catch (const std::exception& e) {
                std::cerr << "Accept loop error: " << e.what() << "\n";
            }
        }
    }

private:
    tcp::acceptor    acceptor_;
    MarketState&     market_;
    Database db_;
    std::map<uint64_t, int64_t> order_user_map_; 
    std::mutex order_map_mutex_;
    net::io_context& ioc_;
    SessionRegistry  registry_;
    TradeHistory     history_;
    std::atomic<uint64_t> next_order_id_{1};
};