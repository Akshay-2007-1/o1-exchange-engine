#pragma once

#include <sqlite3.h>
#include <string>
#include <stdexcept>
#include <iostream>
#include <sodium.h>
#include <ctime>
#include <random>
#include <mutex>
#include <vector>
#include <optional>

class Database {
public:
    struct Result {
        bool        ok;
        std::string error;
        int64_t     id = -1;
    };

    explicit Database(const std::string& path) {
        int rc = sqlite3_open(path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            std::string err = sqlite3_errmsg(db_);
            sqlite3_close(db_);
            throw std::runtime_error("Failed to open database: " + err);
        }
        std::cout << "Database opened: " << path << "\n";
        
        initialise();
        prepare_statements();
    }

    ~Database() {
        if (db_) {
            finalize_statements();
            sqlite3_close(db_);
        }
    }

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    // Exposed so callers (e.g. Server.h's db_worker_) can hold the lock across an
    // entire multi-statement batch, preventing concurrent reads (get_user_profile,
    // reserve_cash, ...) from observing partially-applied state mid-batch.
    std::mutex& mutex() { return db_mutex_; }

    void begin_transaction_unlocked() {
        sqlite3_step(stmt_begin_);
        sqlite3_reset(stmt_begin_);
    }

    void commit_transaction_unlocked() {
        sqlite3_step(stmt_commit_);
        sqlite3_reset(stmt_commit_);
    }

    Result create_user(const std::string& username, const std::string& password, const std::string& role = "trader") {
        char hash[crypto_pwhash_STRBYTES];
        if (crypto_pwhash_str(hash, password.c_str(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
            return {false, "Password hashing failed"};
        }

        int64_t now = static_cast<int64_t>(std::time(nullptr));

        std::lock_guard<std::mutex> lock(db_mutex_);
        
        sqlite3_clear_bindings(stmt_create_user_);
        sqlite3_bind_text(stmt_create_user_, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_create_user_, 2, hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_create_user_, 3, role.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt_create_user_, 4, now);

        int rc = sqlite3_step(stmt_create_user_);
        sqlite3_reset(stmt_create_user_);

        if (rc == SQLITE_CONSTRAINT) return {false, "Username already taken"};
        if (rc != SQLITE_DONE) return {false, std::string("DB error: ") + sqlite3_errmsg(db_)};

        int64_t user_id = sqlite3_last_insert_rowid(db_);

        sqlite3_clear_bindings(stmt_init_bal_);
        sqlite3_bind_int64(stmt_init_bal_, 1, user_id);
        sqlite3_step(stmt_init_bal_);
        sqlite3_reset(stmt_init_bal_);

        return {true, "", user_id};
    }

    Result login(const std::string& username, const std::string& password) {
        std::lock_guard<std::mutex> lock(db_mutex_);

        sqlite3_clear_bindings(stmt_login_);
        sqlite3_bind_text(stmt_login_, 1, username.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt_login_);
        if (rc != SQLITE_ROW) {
            sqlite3_reset(stmt_login_);
            return {false, "Invalid username or password"};
        }

        int64_t user_id = sqlite3_column_int64(stmt_login_, 0);
        std::string stored_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt_login_, 1));
        sqlite3_reset(stmt_login_);

        if (crypto_pwhash_str_verify(stored_hash.c_str(), password.c_str(), password.size()) != 0) {
            return {false, "Invalid username or password"};
        }

        std::string token = generate_token();
        int64_t expires_at = static_cast<int64_t>(std::time(nullptr)) + 86400;

        sqlite3_clear_bindings(stmt_create_sess_);
        sqlite3_bind_text(stmt_create_sess_,  1, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt_create_sess_, 2, user_id);
        sqlite3_bind_int64(stmt_create_sess_, 3, expires_at);
        sqlite3_step(stmt_create_sess_);
        sqlite3_reset(stmt_create_sess_);

        return {true, token, user_id};
    }

    Result validate_session(const std::string& token) {
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        std::lock_guard<std::mutex> lock(db_mutex_);

        sqlite3_clear_bindings(stmt_val_sess_);
        sqlite3_bind_text(stmt_val_sess_,  1, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt_val_sess_, 2, now);

        int rc = sqlite3_step(stmt_val_sess_);
        if (rc != SQLITE_ROW) {
            sqlite3_reset(stmt_val_sess_);
            return {false, "Invalid or expired session"};
        }

        int64_t user_id = sqlite3_column_int64(stmt_val_sess_, 0);
        sqlite3_reset(stmt_val_sess_);
        return {true, "", user_id};
    }

    Result reserve_cash(int64_t user_id, double amount) {
        std::lock_guard<std::mutex> lock(db_mutex_);

        sqlite3_clear_bindings(stmt_get_bal_);
        sqlite3_bind_int64(stmt_get_bal_, 1, user_id);
        
        double current = 0.0;
        if (sqlite3_step(stmt_get_bal_) == SQLITE_ROW) {
            current = sqlite3_column_double(stmt_get_bal_, 0);
        }
        sqlite3_reset(stmt_get_bal_);

        if (current < amount) {
            return {false, "Insufficient funds."};
        }

        sqlite3_clear_bindings(stmt_res_cash_);
        sqlite3_bind_double(stmt_res_cash_, 1, amount);
        sqlite3_bind_int64(stmt_res_cash_, 2, user_id);
        sqlite3_step(stmt_res_cash_);
        sqlite3_reset(stmt_res_cash_);

        return {true, ""};
    }

    Result reserve_shares(int64_t user_id, uint16_t company_id, uint32_t quantity) {
        std::lock_guard<std::mutex> lock(db_mutex_);

        sqlite3_clear_bindings(stmt_get_shares_);
        sqlite3_bind_int64(stmt_get_shares_, 1, user_id);
        sqlite3_bind_int(stmt_get_shares_, 2, company_id);

        int64_t current = 0;
        if (sqlite3_step(stmt_get_shares_) == SQLITE_ROW) {
            current = sqlite3_column_int64(stmt_get_shares_, 0);
        }
        sqlite3_reset(stmt_get_shares_);

        if (current < quantity) {
            return {false, "Insufficient shares."};
        }

        sqlite3_clear_bindings(stmt_res_shares_);
        sqlite3_bind_int64(stmt_res_shares_, 1, quantity);
        sqlite3_bind_int64(stmt_res_shares_, 2, user_id);
        sqlite3_bind_int(stmt_res_shares_, 3, company_id);
        sqlite3_step(stmt_res_shares_);
        sqlite3_reset(stmt_res_shares_);

        return {true, ""};
    }

    struct UserProfile {
        double cash;
        std::vector<std::pair<uint16_t, uint32_t>> portfolio;
    };

    UserProfile get_user_profile(int64_t user_id) {
        UserProfile profile = {0.0, {}};
        std::lock_guard<std::mutex> lock(db_mutex_);

        // Get Cash
        sqlite3_clear_bindings(stmt_get_bal_);
        sqlite3_bind_int64(stmt_get_bal_, 1, user_id);
        if (sqlite3_step(stmt_get_bal_) == SQLITE_ROW) {
            profile.cash = sqlite3_column_double(stmt_get_bal_, 0);
        }
        sqlite3_reset(stmt_get_bal_);

        // Get Portfolio
        sqlite3_clear_bindings(stmt_get_portfolio_);
        sqlite3_bind_int64(stmt_get_portfolio_, 1, user_id);
        while (sqlite3_step(stmt_get_portfolio_) == SQLITE_ROW) {
            uint16_t cid = static_cast<uint16_t>(sqlite3_column_int(stmt_get_portfolio_, 0));
            uint32_t shares = static_cast<uint32_t>(sqlite3_column_int(stmt_get_portfolio_, 1));
            if (shares > 0) profile.portfolio.push_back({cid, shares});
        }
        sqlite3_reset(stmt_get_portfolio_);

        return profile;
    }

    struct TradeRecord {
        int64_t buyer_id;
        int64_t seller_id;
        uint16_t company_id;
        uint32_t quantity;
        uint32_t price;
        uint32_t buyer_limit_price;
    };

    void settle_trade_unlocked(int64_t buyer_id, int64_t seller_id, uint16_t company_id, uint32_t quantity, uint32_t price, uint32_t buyer_limit_price) {
        double total = (static_cast<double>(price) / 100.0) * quantity;

        // Credit shares to buyer
        sqlite3_clear_bindings(stmt_settle_shares_);
        sqlite3_bind_int64(stmt_settle_shares_, 1, buyer_id);
        sqlite3_bind_int(stmt_settle_shares_,  2, company_id);
        sqlite3_bind_int64(stmt_settle_shares_, 3, quantity);
        sqlite3_step(stmt_settle_shares_);
        sqlite3_reset(stmt_settle_shares_);

        // Credit cash to seller
        sqlite3_clear_bindings(stmt_settle_cash_);
        sqlite3_bind_double(stmt_settle_cash_, 1, total);
        sqlite3_bind_int64(stmt_settle_cash_, 2, seller_id);
        sqlite3_step(stmt_settle_cash_);
        sqlite3_reset(stmt_settle_cash_);

        // Price improvement cash refund for buyer
        if (buyer_limit_price > price) {
            double refund = (static_cast<double>(buyer_limit_price - price) / 100.0) * quantity;
            sqlite3_clear_bindings(stmt_settle_cash_);
            sqlite3_bind_double(stmt_settle_cash_, 1, refund);
            sqlite3_bind_int64(stmt_settle_cash_, 2, buyer_id);
            sqlite3_step(stmt_settle_cash_);
            sqlite3_reset(stmt_settle_cash_);
        }
    }

    void settle_trade(int64_t buyer_id, int64_t seller_id, uint16_t company_id, uint32_t quantity, uint32_t price, uint32_t buyer_limit_price = 0) {
        std::lock_guard<std::mutex> lock(db_mutex_);
        settle_trade_unlocked(buyer_id, seller_id, company_id, quantity, price, buyer_limit_price == 0 ? price : buyer_limit_price);
    }

    void settle_batch(const std::vector<TradeRecord>& trades) {
        std::lock_guard<std::mutex> lock(db_mutex_);
        begin_transaction_unlocked();

        for (const auto& t : trades) {
            settle_trade_unlocked(t.buyer_id, t.seller_id, t.company_id, t.quantity, t.price, t.buyer_limit_price);
        }

        commit_transaction_unlocked();
    }

    struct LeaderboardEntry {
        int64_t     user_id;
        std::string username;
        double      cash;
        int64_t     total_shares;
    };

    std::vector<LeaderboardEntry> get_leaderboard(int limit = 20) {
        std::vector<LeaderboardEntry> rows;
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_clear_bindings(stmt_leaderboard_);
        sqlite3_bind_int(stmt_leaderboard_, 1, limit);
        while (sqlite3_step(stmt_leaderboard_) == SQLITE_ROW) {
            LeaderboardEntry e;
            e.user_id     = sqlite3_column_int64(stmt_leaderboard_, 0);
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt_leaderboard_, 1));
            e.username    = name ? name : "";
            e.cash        = sqlite3_column_double(stmt_leaderboard_, 2);
            e.total_shares = sqlite3_column_int64(stmt_leaderboard_, 3);
            rows.push_back(e);
        }
        sqlite3_reset(stmt_leaderboard_);
        return rows;
    }

    struct TradeRow {
        uint16_t company_id;
        int64_t  buyer_id;
        int64_t  seller_id;
        uint32_t price;
        uint32_t quantity;
        int64_t  ts;
    };

    void log_trade_unlocked(uint16_t company_id, int64_t buyer_id, int64_t seller_id, uint32_t price, uint32_t quantity) {
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        sqlite3_clear_bindings(stmt_log_trade_);
        sqlite3_bind_int(stmt_log_trade_,  1, company_id);
        sqlite3_bind_int64(stmt_log_trade_, 2, buyer_id);
        sqlite3_bind_int64(stmt_log_trade_, 3, seller_id);
        sqlite3_bind_int(stmt_log_trade_,  4, price);
        sqlite3_bind_int(stmt_log_trade_,  5, quantity);
        sqlite3_bind_int64(stmt_log_trade_, 6, now);
        sqlite3_step(stmt_log_trade_);
        sqlite3_reset(stmt_log_trade_);
    }

    std::vector<TradeRow> get_user_trades(int64_t user_id, int limit = 50) {
        std::vector<TradeRow> rows;
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_clear_bindings(stmt_get_user_trades_);
        sqlite3_bind_int64(stmt_get_user_trades_, 1, user_id);
        sqlite3_bind_int64(stmt_get_user_trades_, 2, user_id);
        sqlite3_bind_int(stmt_get_user_trades_,  3, limit);
        while (sqlite3_step(stmt_get_user_trades_) == SQLITE_ROW) {
            TradeRow r;
            r.company_id = static_cast<uint16_t>(sqlite3_column_int(stmt_get_user_trades_, 0));
            r.buyer_id   = sqlite3_column_int64(stmt_get_user_trades_, 1);
            r.seller_id  = sqlite3_column_int64(stmt_get_user_trades_, 2);
            r.price      = static_cast<uint32_t>(sqlite3_column_int(stmt_get_user_trades_, 3));
            r.quantity   = static_cast<uint32_t>(sqlite3_column_int(stmt_get_user_trades_, 4));
            r.ts         = sqlite3_column_int64(stmt_get_user_trades_, 5);
            rows.push_back(r);
        }
        sqlite3_reset(stmt_get_user_trades_);
        return rows;
    }

    // Newest-first, capped at `limit` - callers reverse for a chronological
    // chart. Backs the price chart's seed-on-connect so it survives restarts
    // instead of resetting to empty every time (trades table already logs
    // every fill).
    std::vector<uint32_t> get_recent_prices(uint16_t company_id, int limit = 60) {
        std::vector<uint32_t> prices;
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_clear_bindings(stmt_get_recent_prices_);
        sqlite3_bind_int(stmt_get_recent_prices_, 1, company_id);
        sqlite3_bind_int(stmt_get_recent_prices_, 2, limit);
        while (sqlite3_step(stmt_get_recent_prices_) == SQLITE_ROW) {
            prices.push_back(static_cast<uint32_t>(sqlite3_column_int(stmt_get_recent_prices_, 0)));
        }
        sqlite3_reset(stmt_get_recent_prices_);
        return prices;
    }

    void release_cash_unlocked(int64_t user_id, double amount) {
        sqlite3_clear_bindings(stmt_settle_cash_);
        sqlite3_bind_double(stmt_settle_cash_, 1, amount);
        sqlite3_bind_int64(stmt_settle_cash_, 2, user_id);
        sqlite3_step(stmt_settle_cash_);
        sqlite3_reset(stmt_settle_cash_);
    }

    void release_cash(int64_t user_id, double amount) {
        std::lock_guard<std::mutex> lock(db_mutex_);
        release_cash_unlocked(user_id, amount);
    }

    void release_shares_unlocked(int64_t user_id, uint16_t company_id, uint32_t quantity) {
        sqlite3_clear_bindings(stmt_settle_shares_);
        sqlite3_bind_int64(stmt_settle_shares_, 1, user_id);
        sqlite3_bind_int(stmt_settle_shares_, 2, company_id);
        sqlite3_bind_int64(stmt_settle_shares_, 3, quantity);
        sqlite3_step(stmt_settle_shares_);
        sqlite3_reset(stmt_settle_shares_);
    }

    void release_shares(int64_t user_id, uint16_t company_id, uint32_t quantity) {
        std::lock_guard<std::mutex> lock(db_mutex_);
        release_shares_unlocked(user_id, company_id, quantity);
    }

    // ---- Game Mode (Market Making Practice) ----
    // These methods only ever touch game_sessions/game_rounds - never
    // balances, portfolios, or the trades table - so the game's synthetic
    // P&L is structurally isolated from the real wallet/leaderboard, not
    // just isolated by convention.

    struct GameSessionRow {
        int64_t id;
        int64_t user_id;
        int scenario_id;
        int max_rounds;
        int current_round;
        double position;
        double cash;
        std::string hints_revealed_csv;
        std::string status;
    };

    struct GameRoundRow {
        int round_number;
        double bid;
        double ask;
        std::string verdict;
        std::optional<double> fill_price;
        int64_t ts;
    };

    Result create_game_session(int64_t user_id, int scenario_id, int max_rounds) {
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        std::lock_guard<std::mutex> lock(db_mutex_);

        sqlite3_clear_bindings(stmt_game_create_);
        sqlite3_bind_int64(stmt_game_create_, 1, user_id);
        sqlite3_bind_int(stmt_game_create_, 2, scenario_id);
        sqlite3_bind_int(stmt_game_create_, 3, max_rounds);
        sqlite3_bind_int64(stmt_game_create_, 4, now);
        sqlite3_bind_int64(stmt_game_create_, 5, now);

        int rc = sqlite3_step(stmt_game_create_);
        sqlite3_reset(stmt_game_create_);

        if (rc != SQLITE_DONE) return {false, std::string("DB error: ") + sqlite3_errmsg(db_)};
        return {true, "", sqlite3_last_insert_rowid(db_)};
    }

    std::optional<GameSessionRow> get_active_game_session(int64_t user_id) {
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_clear_bindings(stmt_game_get_active_);
        sqlite3_bind_int64(stmt_game_get_active_, 1, user_id);

        std::optional<GameSessionRow> result;
        if (sqlite3_step(stmt_game_get_active_) == SQLITE_ROW) {
            result = read_game_session_row(stmt_game_get_active_);
        }
        sqlite3_reset(stmt_game_get_active_);
        return result;
    }

    std::optional<GameSessionRow> get_game_session(int64_t session_id, int64_t user_id) {
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_clear_bindings(stmt_game_get_by_id_);
        sqlite3_bind_int64(stmt_game_get_by_id_, 1, session_id);
        sqlite3_bind_int64(stmt_game_get_by_id_, 2, user_id);

        std::optional<GameSessionRow> result;
        if (sqlite3_step(stmt_game_get_by_id_) == SQLITE_ROW) {
            result = read_game_session_row(stmt_game_get_by_id_);
        }
        sqlite3_reset(stmt_game_get_by_id_);
        return result;
    }

    void update_game_session_state(int64_t session_id, int current_round, double position,
                                    double cash, const std::string& hints_revealed_csv) {
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_clear_bindings(stmt_game_update_state_);
        sqlite3_bind_int(stmt_game_update_state_, 1, current_round);
        sqlite3_bind_double(stmt_game_update_state_, 2, position);
        sqlite3_bind_double(stmt_game_update_state_, 3, cash);
        sqlite3_bind_text(stmt_game_update_state_, 4, hints_revealed_csv.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt_game_update_state_, 5, now);
        sqlite3_bind_int64(stmt_game_update_state_, 6, session_id);
        sqlite3_step(stmt_game_update_state_);
        sqlite3_reset(stmt_game_update_state_);
    }

    void end_game_session(int64_t session_id) {
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_clear_bindings(stmt_game_end_);
        sqlite3_bind_int64(stmt_game_end_, 1, now);
        sqlite3_bind_int64(stmt_game_end_, 2, session_id);
        sqlite3_step(stmt_game_end_);
        sqlite3_reset(stmt_game_end_);
    }

    void append_game_round(int64_t session_id, int round_number, double bid, double ask,
                            const std::string& verdict, std::optional<double> fill_price) {
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_clear_bindings(stmt_game_round_insert_);
        sqlite3_bind_int64(stmt_game_round_insert_, 1, session_id);
        sqlite3_bind_int(stmt_game_round_insert_, 2, round_number);
        sqlite3_bind_double(stmt_game_round_insert_, 3, bid);
        sqlite3_bind_double(stmt_game_round_insert_, 4, ask);
        sqlite3_bind_text(stmt_game_round_insert_, 5, verdict.c_str(), -1, SQLITE_TRANSIENT);
        if (fill_price.has_value())
            sqlite3_bind_double(stmt_game_round_insert_, 6, *fill_price);
        else
            sqlite3_bind_null(stmt_game_round_insert_, 6);
        sqlite3_bind_int64(stmt_game_round_insert_, 7, now);
        sqlite3_step(stmt_game_round_insert_);
        sqlite3_reset(stmt_game_round_insert_);
    }

    std::vector<GameRoundRow> get_game_rounds(int64_t session_id) {
        std::vector<GameRoundRow> rows;
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_clear_bindings(stmt_game_rounds_get_);
        sqlite3_bind_int64(stmt_game_rounds_get_, 1, session_id);
        while (sqlite3_step(stmt_game_rounds_get_) == SQLITE_ROW) {
            GameRoundRow r;
            r.round_number = sqlite3_column_int(stmt_game_rounds_get_, 0);
            r.bid = sqlite3_column_double(stmt_game_rounds_get_, 1);
            r.ask = sqlite3_column_double(stmt_game_rounds_get_, 2);
            const char* verdict = reinterpret_cast<const char*>(sqlite3_column_text(stmt_game_rounds_get_, 3));
            r.verdict = verdict ? verdict : "PASS";
            if (sqlite3_column_type(stmt_game_rounds_get_, 4) == SQLITE_NULL)
                r.fill_price = std::nullopt;
            else
                r.fill_price = sqlite3_column_double(stmt_game_rounds_get_, 4);
            r.ts = sqlite3_column_int64(stmt_game_rounds_get_, 5);
            rows.push_back(r);
        }
        sqlite3_reset(stmt_game_rounds_get_);
        return rows;
    }

private:
    GameSessionRow read_game_session_row(sqlite3_stmt* stmt) {
        GameSessionRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.user_id = sqlite3_column_int64(stmt, 1);
        row.scenario_id = sqlite3_column_int(stmt, 2);
        row.max_rounds = sqlite3_column_int(stmt, 3);
        row.current_round = sqlite3_column_int(stmt, 4);
        row.position = sqlite3_column_double(stmt, 5);
        row.cash = sqlite3_column_double(stmt, 6);
        const char* hints = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        row.hints_revealed_csv = hints ? hints : "";
        const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        row.status = status ? status : "active";
        return row;
    }

    sqlite3* db_ = nullptr;
    std::mutex db_mutex_;

    // Prepared Statement Pointers
    sqlite3_stmt* stmt_begin_ = nullptr;
    sqlite3_stmt* stmt_commit_ = nullptr;
    sqlite3_stmt* stmt_create_user_ = nullptr;
    sqlite3_stmt* stmt_init_bal_ = nullptr;
    sqlite3_stmt* stmt_login_ = nullptr;
    sqlite3_stmt* stmt_create_sess_ = nullptr;
    sqlite3_stmt* stmt_val_sess_ = nullptr;
    sqlite3_stmt* stmt_get_bal_ = nullptr;
    sqlite3_stmt* stmt_get_shares_ = nullptr;
    sqlite3_stmt* stmt_get_portfolio_ = nullptr;
    sqlite3_stmt* stmt_res_cash_ = nullptr;
    sqlite3_stmt* stmt_res_shares_ = nullptr;
    sqlite3_stmt* stmt_settle_shares_ = nullptr;
    sqlite3_stmt* stmt_settle_cash_ = nullptr;
    sqlite3_stmt* stmt_leaderboard_ = nullptr;
    sqlite3_stmt* stmt_log_trade_ = nullptr;
    sqlite3_stmt* stmt_get_user_trades_ = nullptr;
    sqlite3_stmt* stmt_get_recent_prices_ = nullptr;
    sqlite3_stmt* stmt_game_create_ = nullptr;
    sqlite3_stmt* stmt_game_get_active_ = nullptr;
    sqlite3_stmt* stmt_game_get_by_id_ = nullptr;
    sqlite3_stmt* stmt_game_update_state_ = nullptr;
    sqlite3_stmt* stmt_game_end_ = nullptr;
    sqlite3_stmt* stmt_game_round_insert_ = nullptr;
    sqlite3_stmt* stmt_game_rounds_get_ = nullptr;

    void initialise() {
        // High-performance SQLite pragmas
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

        const char* sql = R"(
            CREATE TABLE IF NOT EXISTS users (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                username      TEXT UNIQUE NOT NULL,
                password_hash TEXT NOT NULL,
                role          TEXT NOT NULL DEFAULT 'trader',
                created_at    INTEGER NOT NULL
            );

            CREATE TABLE IF NOT EXISTS balances (
                user_id   INTEGER PRIMARY KEY,
                cash      REAL NOT NULL DEFAULT 10000.0,
                FOREIGN KEY (user_id) REFERENCES users(id)
            );

            CREATE TABLE IF NOT EXISTS portfolios (
                user_id    INTEGER NOT NULL,
                company_id INTEGER NOT NULL,
                shares     INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (user_id, company_id),
                FOREIGN KEY (user_id) REFERENCES users(id)
            );

            CREATE TABLE IF NOT EXISTS sessions (
                token      TEXT PRIMARY KEY,
                user_id    INTEGER NOT NULL,
                expires_at INTEGER NOT NULL,
                FOREIGN KEY (user_id) REFERENCES users(id)
            );

            CREATE TABLE IF NOT EXISTS trades (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                company_id INTEGER NOT NULL,
                buyer_id   INTEGER NOT NULL,
                seller_id  INTEGER NOT NULL,
                price      INTEGER NOT NULL,
                quantity   INTEGER NOT NULL,
                ts         INTEGER NOT NULL
            );

            -- ---- Game Mode (Market Making Practice) ----
            -- Fully separate from balances/portfolios: game P&L is synthetic
            -- and never touches the real wallet or leaderboard.
            CREATE TABLE IF NOT EXISTS game_sessions (
                id             INTEGER PRIMARY KEY AUTOINCREMENT,
                user_id        INTEGER NOT NULL,
                scenario_id    INTEGER NOT NULL,
                max_rounds     INTEGER NOT NULL,
                current_round  INTEGER NOT NULL DEFAULT 0,
                position       REAL NOT NULL DEFAULT 0,
                cash           REAL NOT NULL DEFAULT 0,
                hints_revealed TEXT NOT NULL DEFAULT '',
                status         TEXT NOT NULL DEFAULT 'active',
                created_at     INTEGER NOT NULL,
                updated_at     INTEGER NOT NULL,
                FOREIGN KEY (user_id) REFERENCES users(id)
            );

            CREATE TABLE IF NOT EXISTS game_rounds (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                session_id    INTEGER NOT NULL,
                round_number  INTEGER NOT NULL,
                bid           REAL NOT NULL,
                ask           REAL NOT NULL,
                verdict       TEXT NOT NULL,
                fill_price    REAL,
                ts            INTEGER NOT NULL,
                FOREIGN KEY (session_id) REFERENCES game_sessions(id)
            );
        )";

        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err;
            sqlite3_free(err);
            throw std::runtime_error("Failed to initialise schema: " + msg);
        }
    }

    void prepare_statements() {
        sqlite3_prepare_v2(db_, "BEGIN TRANSACTION;", -1, &stmt_begin_, nullptr);
        sqlite3_prepare_v2(db_, "COMMIT;", -1, &stmt_commit_, nullptr);
        sqlite3_prepare_v2(db_, "INSERT INTO users (username, password_hash, role, created_at) VALUES (?, ?, ?, ?);", -1, &stmt_create_user_, nullptr);
        sqlite3_prepare_v2(db_, "INSERT INTO balances (user_id, cash) VALUES (?, 10000.0);", -1, &stmt_init_bal_, nullptr);
        sqlite3_prepare_v2(db_, "SELECT id, password_hash FROM users WHERE username = ?;", -1, &stmt_login_, nullptr);
        sqlite3_prepare_v2(db_, "INSERT INTO sessions (token, user_id, expires_at) VALUES (?, ?, ?);", -1, &stmt_create_sess_, nullptr);
        sqlite3_prepare_v2(db_, "SELECT user_id FROM sessions WHERE token = ? AND expires_at > ?;", -1, &stmt_val_sess_, nullptr);
        sqlite3_prepare_v2(db_, "SELECT cash FROM balances WHERE user_id = ?;", -1, &stmt_get_bal_, nullptr);
        sqlite3_prepare_v2(db_, "SELECT shares FROM portfolios WHERE user_id = ? AND company_id = ?;", -1, &stmt_get_shares_, nullptr);
        sqlite3_prepare_v2(db_, "SELECT company_id, shares FROM portfolios WHERE user_id = ?;", -1, &stmt_get_portfolio_, nullptr);
        sqlite3_prepare_v2(db_, "UPDATE balances SET cash = cash - ? WHERE user_id = ?;", -1, &stmt_res_cash_, nullptr);
        sqlite3_prepare_v2(db_, "UPDATE portfolios SET shares = shares - ? WHERE user_id = ? AND company_id = ?;", -1, &stmt_res_shares_, nullptr);
        
        sqlite3_prepare_v2(db_, "INSERT INTO portfolios (user_id, company_id, shares) VALUES (?, ?, ?) ON CONFLICT (user_id, company_id) DO UPDATE SET shares = shares + excluded.shares;", -1, &stmt_settle_shares_, nullptr);
        sqlite3_prepare_v2(db_, "UPDATE balances SET cash = cash + ? WHERE user_id = ?;", -1, &stmt_settle_cash_, nullptr);
        sqlite3_prepare_v2(db_,
            "SELECT u.id, u.username, b.cash, COALESCE(SUM(p.shares), 0) as total_shares "
            "FROM users u "
            "JOIN balances b ON b.user_id = u.id "
            "LEFT JOIN portfolios p ON p.user_id = u.id "
            "GROUP BY u.id "
            "ORDER BY b.cash DESC "
            "LIMIT ?;",
            -1, &stmt_leaderboard_, nullptr);
        sqlite3_prepare_v2(db_, "INSERT INTO trades (company_id, buyer_id, seller_id, price, quantity, ts) VALUES (?,?,?,?,?,?);", -1, &stmt_log_trade_, nullptr);
        sqlite3_prepare_v2(db_, "SELECT company_id, buyer_id, seller_id, price, quantity, ts FROM trades WHERE buyer_id = ? OR seller_id = ? ORDER BY ts DESC, id DESC LIMIT ?;", -1, &stmt_get_user_trades_, nullptr);
        sqlite3_prepare_v2(db_, "SELECT price FROM trades WHERE company_id = ? ORDER BY ts DESC, id DESC LIMIT ?;", -1, &stmt_get_recent_prices_, nullptr);

        sqlite3_prepare_v2(db_,
            "INSERT INTO game_sessions (user_id, scenario_id, max_rounds, created_at, updated_at) VALUES (?, ?, ?, ?, ?);",
            -1, &stmt_game_create_, nullptr);
        sqlite3_prepare_v2(db_,
            "SELECT id, user_id, scenario_id, max_rounds, current_round, position, cash, hints_revealed, status "
            "FROM game_sessions WHERE user_id = ? AND status = 'active' ORDER BY id DESC LIMIT 1;",
            -1, &stmt_game_get_active_, nullptr);
        sqlite3_prepare_v2(db_,
            "SELECT id, user_id, scenario_id, max_rounds, current_round, position, cash, hints_revealed, status "
            "FROM game_sessions WHERE id = ? AND user_id = ?;",
            -1, &stmt_game_get_by_id_, nullptr);
        sqlite3_prepare_v2(db_,
            "UPDATE game_sessions SET current_round = ?, position = ?, cash = ?, hints_revealed = ?, updated_at = ? WHERE id = ?;",
            -1, &stmt_game_update_state_, nullptr);
        sqlite3_prepare_v2(db_,
            "UPDATE game_sessions SET status = 'ended', updated_at = ? WHERE id = ?;",
            -1, &stmt_game_end_, nullptr);
        sqlite3_prepare_v2(db_,
            "INSERT INTO game_rounds (session_id, round_number, bid, ask, verdict, fill_price, ts) VALUES (?, ?, ?, ?, ?, ?, ?);",
            -1, &stmt_game_round_insert_, nullptr);
        sqlite3_prepare_v2(db_,
            "SELECT round_number, bid, ask, verdict, fill_price, ts FROM game_rounds WHERE session_id = ? ORDER BY round_number;",
            -1, &stmt_game_rounds_get_, nullptr);
    }

    void finalize_statements() {
        sqlite3_finalize(stmt_begin_);
        sqlite3_finalize(stmt_commit_);
        sqlite3_finalize(stmt_create_user_);
        sqlite3_finalize(stmt_init_bal_);
        sqlite3_finalize(stmt_login_);
        sqlite3_finalize(stmt_create_sess_);
        sqlite3_finalize(stmt_val_sess_);
        sqlite3_finalize(stmt_get_bal_);
        sqlite3_finalize(stmt_get_shares_);
        sqlite3_finalize(stmt_get_portfolio_);
        sqlite3_finalize(stmt_res_cash_);
        sqlite3_finalize(stmt_res_shares_);
        sqlite3_finalize(stmt_settle_shares_);
        sqlite3_finalize(stmt_settle_cash_);
        sqlite3_finalize(stmt_leaderboard_);
        sqlite3_finalize(stmt_log_trade_);
        sqlite3_finalize(stmt_get_user_trades_);
        sqlite3_finalize(stmt_get_recent_prices_);
        sqlite3_finalize(stmt_game_create_);
        sqlite3_finalize(stmt_game_get_active_);
        sqlite3_finalize(stmt_game_get_by_id_);
        sqlite3_finalize(stmt_game_update_state_);
        sqlite3_finalize(stmt_game_end_);
        sqlite3_finalize(stmt_game_round_insert_);
        sqlite3_finalize(stmt_game_rounds_get_);
    }

    std::string generate_token() {
        static const char chars[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        uint8_t raw[32];
        randombytes_buf(raw, sizeof(raw));
        std::string token(32, ' ');
        for (int i = 0; i < 32; i++)
            token[i] = chars[raw[i] % 62];
        return token;
    }
};