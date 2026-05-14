#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <iostream>
#include <set>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>
#include <condition_variable>
#include "Market.h"
#include "Database.h"

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;
using json          = nlohmann::json;

class Session;

// Atomic Lock-Free Single-Producer Single-Consumer Queue
template<typename T, size_t Size>
class SPSCQueue {
    static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2 for bitwise optimization");
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) T buffer_[Size];
public:
    bool push(const T& item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (tail + 1) & (Size - 1);
        if (next_tail == head_.load(std::memory_order_acquire)) return false;
        buffer_[tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    bool pop(T& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        item = std::move(buffer_[head]);
        head_.store((head + 1) & (Size - 1), std::memory_order_release);
        return true;
    }
};

struct EngineTask {
    enum Type { ORDER, CANCEL, SNAPSHOT } type;
    uint16_t company_id;
    Order order;
    bool side;
    uint32_t price;
    uint64_t order_id;
    int64_t user_id;
    std::shared_ptr<Session> session;
};

using EngineQueue = SPSCQueue<EngineTask, 65536>;

// JSON Serializers
inline json trade_to_json(const Trade& t) {
    return {
        {"type", "trade"},
        {"company_id", t.company_id},
        {"price", t.price},
        {"quantity", t.quantity},
        {"buy_order_id", t.buy_order_id},
        {"sell_order_id", t.sell_order_id}
    };
}

inline json depth_to_json(const std::vector<OrderBook::DepthLevel>& depth) {
    json levels = json::array();
    for (const auto& level : depth) {
        levels.push_back({{"price", level.price}, {"quantity", level.quantity}, {"orders", level.orders}});
    }
    return levels;
}

inline json orders_to_json(const std::vector<OrderBook::OrderSnapshot>& orders) {
    json rows = json::array();
    for (const auto& order : orders) {
        rows.push_back({{"id", order.id}, {"price", order.price}, {"quantity", order.quantity}, {"timestamp", order.timestamp}});
    }
    return rows;
}

inline json companies_to_json(const std::vector<Company>& companies) {
    json rows = json::array();
    for (const auto& company : companies) {
        rows.push_back({{"id", company.id}, {"symbol", company.symbol}, {"name", company.name}, {"total_shares", company.total_shares}});
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

inline json profile_to_json(const Database::UserProfile& profile) {
    json portfolio = json::array();
    for (const auto& item : profile.portfolio) {
        portfolio.push_back({{"company_id", item.first}, {"shares", item.second}});
    }
    return {
        {"type", "user_update"},
        {"cash", profile.cash},
        {"portfolio", portfolio}
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
        if (trades_.size() > max_trades_) trades_.erase(trades_.begin());
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
    void broadcast_user_update(int64_t user_id);
private:
    std::set<std::shared_ptr<Session>> sessions_;
    std::mutex mutex_;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, MarketState& market, SessionRegistry& registry, 
        std::atomic<uint64_t>& next_order_id, Database& db, EngineQueue& engine_queue)
        : ws_(std::move(socket)), market_(market), registry_(registry)
        , next_order_id_(next_order_id), default_company_id_(market.default_company_id())
        , db_(db), engine_queue_(engine_queue) {}

    void start() {
        ws_.async_accept([self = shared_from_this()](beast::error_code ec) {
            if (!ec) {
                self->registry_.add(self);
                self->request_snapshot(self->default_company_id_);
                self->do_read();
            }
        });
    }

    void send(std::string msg) {
        net::post(ws_.get_executor(), [self = shared_from_this(), msg = std::move(msg)]() {
            self->write_queue_.push_back(msg);
            if (self->write_queue_.size() == 1) self->do_write();
        });
    }

    int64_t user_id() const { return current_user_id_; }

    void send_user_update() {
        if (current_user_id_ == -1) return;
        send(profile_to_json(db_.get_user_profile(current_user_id_)).dump());
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
                self->do_read();
            }
        });
    }

    void do_write() {
        ws_.async_write(net::buffer(write_queue_.front()), 
            [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
                boost::ignore_unused(bytes_transferred);
                if (ec) { self->registry_.remove(self); return; }
                self->write_queue_.erase(self->write_queue_.begin());
                if (!self->write_queue_.empty()) self->do_write();
            });
    }

    void request_snapshot(uint16_t company_id) {
        EngineTask task;
        task.type = EngineTask::SNAPSHOT;
        task.company_id = company_id;
        task.session = shared_from_this();
        while (!engine_queue_.push(task)) std::this_thread::yield();

        send_user_update();
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
        current_user_id_ = result.id;
        send(json{ {"type", "registered"}, {"user_id", result.id}, {"username", username}, {"message", "Account created successfully"} }.dump());
        send_user_update();
    }

    void handle_login(const json& msg) {
        std::string username = msg.value("username", "");
        std::string password = msg.value("password", "");

        auto result = db_.login(username, password);
        if (!result.ok) {
            send(json{{"type", "error"}, {"message", result.error}} .dump());
            return;
        }
        current_user_id_ = result.id;
        send(json{ {"type", "logged_in"}, {"token", result.error}, {"user_id", result.id}, {"username", username}, {"message", "Login successful"} }.dump());
        send_user_update();
    }

    void handle_message(const std::string& raw) {
        try {
            auto msg = json::parse(raw);
            if (!msg.is_object()) return;

            std::string type = msg.value("type", "");

            if (type == "register") { handle_register(msg); return; }
            if (type == "login")    { handle_login(msg); return; }
            
            if (type == "snapshot") { 
                std::string token = msg.value("token", "");
                if (!token.empty()) {
                    auto res = db_.validate_session(token);
                    if (res.ok) current_user_id_ = res.id;
                }
                request_snapshot(msg.value("company_id", default_company_id_)); 
                return; 
            }

            if (type == "cancel") {
                std::string token = msg.value("token", "");
                auto session_result = db_.validate_session(token);
                if (!session_result.ok) {
                    send(json{{"type", "error"}, {"message", "Authentication required to cancel."}} .dump());
                    return;
                }

                EngineTask task;
                task.type = EngineTask::CANCEL;
                task.company_id = msg.value("company_id", default_company_id_);
                task.side = (msg["side"] == "BUY");
                task.price = msg["price"].get<uint32_t>();
                task.order_id = msg["order_id"].get<uint64_t>();
                task.user_id = session_result.id;
                while (!engine_queue_.push(task)) std::this_thread::yield();
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
                current_user_id_ = user_id;

                if (side) { 
                    // price is in cents, need to convert to dollars for DB balance check
                    double required_cash = (static_cast<double>(price) / 100.0) * msg["quantity"].get<uint32_t>();
                    auto check = db_.reserve_cash(user_id, required_cash);
                    if (!check.ok) { send(json{{"type", "error"}, {"message", check.error}} .dump()); return; }
                } else {
                    auto check = db_.reserve_shares(user_id, company_id, msg["quantity"].get<uint32_t>());
                    if (!check.ok) { send(json{{"type", "error"}, {"message", check.error}} .dump()); return; }
                }

                EngineTask task;
                task.type = EngineTask::ORDER;
                task.company_id = company_id;
                task.order.id = next_order_id_.fetch_add(1, std::memory_order_relaxed);
                task.order.user_id = user_id;
                task.order.company_id = company_id;
                task.order.side = side;
                task.order.price = price;
                task.order.quantity = msg["quantity"].get<uint32_t>();
                task.order.timestamp = msg["timestamp"].get<uint64_t>();
                
                while (!engine_queue_.push(task)) std::this_thread::yield();
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
    SessionRegistry& registry_;
    EngineQueue& engine_queue_;
    std::atomic<uint64_t>& next_order_id_;
    uint16_t default_company_id_;
    int64_t current_user_id_ = -1;
};

inline void SessionRegistry::broadcast(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : sessions_) s->send(msg);
}

inline void SessionRegistry::broadcast_user_update(int64_t user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : sessions_) {
        if (s->user_id() == user_id) {
            s->send_user_update();
        }
    }
}

class Server : public ITradeListener {
public:
    struct DbTask {
        int64_t buyer_id;
        int64_t seller_id;
        uint16_t company_id;
        uint32_t quantity;
        uint32_t price;
    };

    Server(net::io_context& ioc, unsigned short port, MarketState& market)
        : acceptor_(ioc, tcp::endpoint(tcp::v4(), port)), market_(market), db_("exchange.db") {
        
        for (const auto& company : market_.companies()) {
            InstrumentState* instrument = market_.find_instrument(company.id);
            if (instrument) instrument->book.trade_listener = this;
        }

        engine_thread_ = std::thread([this]() { run_engine_worker(); });
        db_worker_ = std::thread([this]() { run_db_worker(); });
        
        do_accept();
    }

    ~Server() {
        stop_worker_ = true;
        db_q_cv_.notify_all();
        if (engine_thread_.joinable()) engine_thread_.join();
        if (db_worker_.joinable()) db_worker_.join();
    }

    // Engine executing a trade
    void on_trade(const Trade& t) override {
        history_.record(t);
        registry_.broadcast(trade_to_json(t).dump());

        {
            std::lock_guard<std::mutex> q_lock(db_q_mutex_);
            db_queue_front_.push_back({t.buyer_user_id, t.seller_user_id, t.company_id, t.quantity, t.price});
        }
        db_q_cv_.notify_one();
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(
                        std::move(socket), market_, registry_, 
                        next_order_id_, db_, engine_queue_
                    )->start();
                }
                do_accept();
            });
    }

    // Match Engine Dedicated Spin-Loop
    void run_engine_worker() {
        EngineTask task;
        while (!stop_worker_.load(std::memory_order_relaxed)) {
            while (engine_queue_.pop(task)) {
                InstrumentState* instrument = market_.find_instrument(task.company_id);
                if (!instrument) continue;

                if (task.type == EngineTask::ORDER) {
                    instrument->book.add_order(task.order);
                    registry_.broadcast(book_to_json(*instrument).dump());
                } 
                else if (task.type == EngineTask::CANCEL) {
                    if (instrument->book.cancel_order(task.side, task.price, task.order_id, task.user_id)) {
                        registry_.broadcast(book_to_json(*instrument).dump());
                    }
                } 
                else if (task.type == EngineTask::SNAPSHOT) {
                    json history = history_.to_json();
                    std::string msg = snapshot_to_json(*instrument, history, market_.companies()).dump();
                    if (task.session) task.session->send(msg);
                }
            }
        }
    }

    // Double-Buffered DB Vector Swap
    void run_db_worker() {
        db_queue_front_.reserve(1000);
        db_queue_back_.reserve(1000);

        while (true) {
            {
                std::unique_lock<std::mutex> lock(db_q_mutex_);
                db_q_cv_.wait(lock, [this] { return !db_queue_front_.empty() || stop_worker_; });
                if (stop_worker_ && db_queue_front_.empty()) break;
                
                // O(1) swap: Unlocks the engine immediately 
                db_queue_front_.swap(db_queue_back_);
            }

            if (!db_queue_back_.empty()) {
                db_.begin_transaction();
                std::set<int64_t> affected_users;
                for (const auto& task : db_queue_back_) {
                    db_.settle_trade(task.buyer_id, task.seller_id, task.company_id, task.quantity, task.price);
                    affected_users.insert(task.buyer_id);
                    affected_users.insert(task.seller_id);
                }
                db_.commit_transaction();

                // Broadcast updates AFTER commit to ensure users see fresh data
                for (int64_t uid : affected_users) {
                    registry_.broadcast_user_update(uid);
                }
                
                db_queue_back_.clear(); // Preserves memory capacity for next batch
            }
        }
    }

    tcp::acceptor acceptor_;
    MarketState&  market_;
    Database      db_;
    
    SessionRegistry registry_;
    TradeHistory    history_;
    EngineQueue     engine_queue_;
    std::atomic<uint64_t> next_order_id_{1};

    std::vector<DbTask> db_queue_front_;
    std::vector<DbTask> db_queue_back_;
    std::mutex db_q_mutex_;
    std::condition_variable db_q_cv_;
    
    std::atomic<bool> stop_worker_{false};
    std::thread engine_thread_;
    std::thread db_worker_;
};