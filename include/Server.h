#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
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
#include <queue>
#include <condition_variable>
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
        return { {"type", "history"}, {"trades", trades} };
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

// Async Session implementation
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, MarketState& market, SessionRegistry& registry, 
        TradeHistory& history, std::atomic<uint64_t>& next_order_id, Database& db, 
        std::map<uint64_t, int64_t>& order_user_map, std::mutex& order_map_mutex)
        : ws_(std::move(socket))
        , market_(market)
        , registry_(registry)
        , history_(history)
        , next_order_id_(next_order_id)
        , default_company_id_(market.default_company_id())
        , db_(db)
        , order_user_map_(order_user_map)
        , order_map_mutex_(order_map_mutex) {}

    void start() {
        // Accept the websocket handshake asynchronously
        ws_.async_accept([self = shared_from_this()](beast::error_code ec) {
            if (!ec) {
                self->registry_.add(self);
                self->send_snapshot(self->default_company_id_);
                self->do_read();
            }
        });
    }

    void send(std::string msg) {
        // Post the write to the I/O strand to avoid concurrent async_write calls
        net::post(ws_.get_executor(), [self = shared_from_this(), msg = std::move(msg)]() {
            self->write_queue_.push_back(msg);
            if (self->write_queue_.size() == 1) {
                self->do_write();
            }
        });
    }

private:
    void do_read() {
        ws_.async_read(buffer_, [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
            boost::ignore_unused(bytes_transferred);
            if (ec == websocket::error::closed || ec == net::error::eof || ec == net::error::connection_reset) {
                self->registry_.remove(self);
                return;
            }
            if (!ec) {
                self->handle_message(beast::buffers_to_string(self->buffer_.data()));
                self->buffer_.consume(self->buffer_.size());
                self->do_read(); // Queue next read
            }
        });
    }

    void do_write() {
        ws_.async_write(net::buffer(write_queue_.front()), 
            [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
                boost::ignore_unused(bytes_transferred);
                if (ec) {
                    self->registry_.remove(self);
                    return;
                }
                self->write_queue_.erase(self->write_queue_.begin());
                if (!self->write_queue_.empty()) {
                    self->do_write(); // Write next message in queue
                }
            });
    }

    void send_snapshot(uint16_t company_id) {
        const InstrumentState* instrument = market_.find_instrument(company_id);
        if (instrument == nullptr) instrument = market_.find_instrument(default_company_id_);
        if (instrument == nullptr) return;

        std::lock_guard<std::mutex> lock(instrument->mutex);
        json history = history_.to_json();
        send(snapshot_to_json(*instrument, history, market_.companies()).dump());
    }

    void handle_register(const json& msg) {
        std::string username = msg.value("username", "");
        std::string password = msg.value("password", "");
        std::string role     = msg.value("role", "trader");

        if (username.empty() || password.empty() || password.size() < 6) {
            send(json{{"type", "error"}, {"message", "Valid username and 6+ char password required"}} .dump());
            return;
        }
        auto result = db_.create_user(username, password, role);
        if (!result.ok) {
            send(json{{"type", "error"}, {"message", result.error}} .dump());
            return;
        }
        send(json{ {"type", "registered"}, {"user_id", result.id}, {"username", username}, {"message", "Account created successfully"} }.dump());
    }

    void handle_login(const json& msg) {
        std::string username = msg.value("username", "");
        std::string password = msg.value("password", "");

        auto result = db_.login(username, password);
        if (!result.ok) {
            send(json{{"type", "error"}, {"message", result.error}} .dump());
            return;
        }
        send(json{ {"type", "logged_in"}, {"token", result.error}, {"user_id", result.id}, {"username", username}, {"message", "Login successful"} }.dump());
    }

    void handle_message(const std::string& raw) {
        try {
            auto msg = json::parse(raw);
            if (!msg.is_object()) return;

            std::string type = msg.value("type", "");

            if (type == "register") { handle_register(msg); return; }
            if (type == "login")    { handle_login(msg); return; }
            if (type == "snapshot") { send_snapshot(msg.value("company_id", default_company_id_)); return; }

            if (type == "cancel") {
                const uint16_t company_id = msg.value("company_id", default_company_id_);
                InstrumentState* instrument = market_.find_instrument(company_id);
                if (!instrument) return;

                const bool side = (msg["side"] == "BUY");
                const uint32_t price = msg["price"].get<uint32_t>();
                const uint64_t order_id = msg["order_id"].get<uint64_t>();

                std::lock_guard<std::mutex> lock(instrument->mutex);
                if (instrument->book.cancel_order(side, price, order_id)) {
                    registry_.broadcast(book_to_json(*instrument).dump());
                }
                return;
            }

            if (type == "order") {
                const uint16_t company_id = msg.value("company_id", default_company_id_);
                const bool side = (msg["side"] == "BUY");
                const uint32_t price = msg["price"].get<uint32_t>();
                
                if (price >= 100000) {
                     send(json{{"type", "error"}, {"message", "Price exceeds maximum allowed ($999.99)."}} .dump());
                     return;
                }
                
                std::string token = msg.value("token", "");
                auto session_result = db_.validate_session(token);
                if (!session_result.ok) {
                    send(json{{"type", "error"}, {"message", "Authentication required/invalid."}} .dump());
                    return;
                }

                int64_t user_id = session_result.id;

                if (side) { // BUY
                    double required_cash = static_cast<double>(price) * msg["quantity"].get<uint32_t>();
                    auto check = db_.reserve_cash(user_id, required_cash);
                    if (!check.ok) { send(json{{"type", "error"}, {"message", check.error}} .dump()); return; }
                } else {
                    auto check = db_.reserve_shares(user_id, company_id, msg["quantity"].get<uint32_t>());
                    if (!check.ok) { send(json{{"type", "error"}, {"message", check.error}} .dump()); return; }
                }

                InstrumentState* instrument = market_.find_instrument(company_id);
                if (!instrument) return;

                Order order;
                order.id        = next_order_id_.fetch_add(1);
                order.company_id = instrument->company.id;
                order.side      = side;
                order.price     = price;
                order.quantity  = msg["quantity"].get<uint32_t>();
                order.timestamp = msg["timestamp"].get<uint64_t>();

                {
                    std::lock_guard<std::mutex> lock(order_map_mutex_);
                    order_user_map_[order.id] = user_id;
                }

                std::lock_guard<std::mutex> lock(instrument->mutex);
                instrument->book.add_order(order);
                registry_.broadcast(book_to_json(*instrument).dump());
            }
        } catch (const std::exception& e) {
            std::cerr << "Message parse error: " << e.what() << "\n";
        }
    }

    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    std::vector<std::string> write_queue_;

    MarketState& market_;
    Database& db_;
    std::map<uint64_t, int64_t>& order_user_map_;
    std::mutex& order_map_mutex_;
    SessionRegistry& registry_;
    TradeHistory& history_;
    std::atomic<uint64_t>& next_order_id_;
    uint16_t default_company_id_;
};

inline void SessionRegistry::broadcast(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : sessions_) {
        s->send(msg);
    }
}

// ─────────────────────────────────────────
//  Server & Background DB Worker
// ─────────────────────────────────────────
class Server {
public:
    struct DbTask {
        int64_t buyer_id;
        int64_t seller_id;
        uint16_t company_id;
        uint32_t quantity;
        uint32_t price;
    };

    Server(net::io_context& ioc, unsigned short port, MarketState& market)
        : acceptor_(ioc, tcp::endpoint(tcp::v4(), port))
        , market_(market)
        , db_("exchange.db") {
        
        // Spawn Background Worker Thread for SQLite Writes
        db_worker_ = std::thread([this]() { run_db_worker(); });

        for (const auto& company : market_.companies()) {
            InstrumentState* instrument = market_.find_instrument(company.id);
            if (!instrument) continue;

            // Matching Engine Callback (Executes instantly in memory)
            instrument->book.on_trade = [this](const Trade& t) {
                history_.record(t);
                registry_.broadcast(trade_to_json(t).dump());

                std::lock_guard<std::mutex> lock(order_map_mutex_);
                auto buyer_it  = order_user_map_.find(t.buy_order_id);
                auto seller_it = order_user_map_.find(t.sell_order_id);

                if (buyer_it != order_user_map_.end() && seller_it != order_user_map_.end()) {
                    // Send to background queue instead of blocking matching engine
                    {
                        std::lock_guard<std::mutex> q_lock(db_q_mutex_);
                        db_queue_.push({buyer_it->second, seller_it->second, t.company_id, t.quantity, t.price});
                    }
                    db_q_cv_.notify_one();
                }
            };
        }
        
        do_accept();
    }

    ~Server() {
        stop_worker_ = true;
        db_q_cv_.notify_all();
        if (db_worker_.joinable()) db_worker_.join();
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(
                        std::move(socket), market_, registry_, history_, 
                        next_order_id_, db_, order_user_map_, order_map_mutex_
                    )->start();
                }
                do_accept(); // Loop
            });
    }

    // Database Background Thread: Batches transactions together
    void run_db_worker() {
        std::vector<DbTask> batch;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(db_q_mutex_);
                db_q_cv_.wait(lock, [this] { return !db_queue_.empty() || stop_worker_; });
                
                if (stop_worker_ && db_queue_.empty()) break;

                // Pull everything currently in the queue
                while (!db_queue_.empty()) {
                    batch.push_back(db_queue_.front());
                    db_queue_.pop();
                }
            }

            if (!batch.empty()) {
                // Massive performance gain: wrap batch in a single transaction
                db_.begin_transaction();
                for (const auto& task : batch) {
                    db_.settle_trade(task.buyer_id, task.seller_id, task.company_id, task.quantity, task.price);
                }
                db_.commit_transaction();
                batch.clear();
            }
        }
    }

    tcp::acceptor    acceptor_;
    MarketState&     market_;
    Database         db_;
    
    std::map<uint64_t, int64_t> order_user_map_; 
    std::mutex order_map_mutex_;
    
    SessionRegistry  registry_;
    TradeHistory     history_;
    std::atomic<uint64_t> next_order_id_{1};

    // DB Worker Queue variables
    std::queue<DbTask> db_queue_;
    std::mutex db_q_mutex_;
    std::condition_variable db_q_cv_;
    bool stop_worker_ = false;
    std::thread db_worker_;
};