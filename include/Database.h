#pragma once

#include <sqlite3.h>
#include <string>
#include <stdexcept>
#include <iostream>
#include <sodium.h>
#include <ctime>
#include <random>
#include <mutex>

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

    void begin_transaction() {
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_step(stmt_begin_);
        sqlite3_reset(stmt_begin_);
    }

    void commit_transaction() {
        std::lock_guard<std::mutex> lock(db_mutex_);
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
        sqlite3_step(stmt_begin_);
        sqlite3_reset(stmt_begin_);

        for (const auto& t : trades) {
            settle_trade_unlocked(t.buyer_id, t.seller_id, t.company_id, t.quantity, t.price, t.buyer_limit_price);
        }

        sqlite3_step(stmt_commit_);
        sqlite3_reset(stmt_commit_);
    }

    void release_cash(int64_t user_id, double amount) {
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_clear_bindings(stmt_settle_cash_);
        sqlite3_bind_double(stmt_settle_cash_, 1, amount);
        sqlite3_bind_int64(stmt_settle_cash_, 2, user_id);
        sqlite3_step(stmt_settle_cash_);
        sqlite3_reset(stmt_settle_cash_);
    }

    void release_shares(int64_t user_id, uint16_t company_id, uint32_t quantity) {
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_clear_bindings(stmt_settle_shares_);
        sqlite3_bind_int64(stmt_settle_shares_, 1, user_id);
        sqlite3_bind_int(stmt_settle_shares_, 2, company_id);
        sqlite3_bind_int64(stmt_settle_shares_, 3, quantity);
        sqlite3_step(stmt_settle_shares_);
        sqlite3_reset(stmt_settle_shares_);
    }

private:
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