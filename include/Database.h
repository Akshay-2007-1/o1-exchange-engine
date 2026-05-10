#pragma once

#include <sqlite3.h>
#include <string>
#include <stdexcept>
#include <iostream>
#include <sodium.h>
#include <ctime>
#include <random>

class Database {
public:
    // ── Result type — cleaner than exceptions for expected failures ──
    struct Result {
        bool        ok;
        std::string error;
        int64_t     id = -1;   // used when returning a new row id
    };

    // ── Open (or create) the database file ──
    explicit Database(const std::string& path) {
        int rc = sqlite3_open(path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            std::string err = sqlite3_errmsg(db_);
            sqlite3_close(db_);
            throw std::runtime_error("Failed to open database: " + err);
        }
        std::cout << "Database opened: " << path << "\n";
        initialise();
    }

    // ── Always close on destruction ──
    ~Database() {
        if (db_) sqlite3_close(db_);
    }

    // ── No copying — only one owner of the db handle ──
    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    // ── Register a new user ──────────────────────────────────────
    // Returns ok=true and the new user's id on success.
    // Returns ok=false with an error message if username is taken.
    Result create_user(const std::string& username, 
                    const std::string& password,
                    const std::string& role = "trader") {

        // 1. hash the password — never store it plain
        char hash[crypto_pwhash_STRBYTES];
        if (crypto_pwhash_str(
                hash,
                password.c_str(), password.size(),
                crypto_pwhash_OPSLIMIT_INTERACTIVE,
                crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
            return {false, "Password hashing failed (out of memory?)"};
        }

        // 2. get current unix timestamp
        int64_t now = static_cast<int64_t>(std::time(nullptr));

        // 3. insert into users table
        const char* sql =
            "INSERT INTO users (username, password_hash, role, created_at) "
            "VALUES (?, ?, ?, ?);";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, hash,             -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, role.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, now);

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc == SQLITE_CONSTRAINT) {
            return {false, "Username already taken"};
        }
        if (rc != SQLITE_DONE) {
            return {false, std::string("DB error: ") + sqlite3_errmsg(db_)};
        }

        int64_t user_id = sqlite3_last_insert_rowid(db_);

        // 4. create starting balance ($10,000 cash)
        const char* bal_sql =
            "INSERT INTO balances (user_id, cash) VALUES (?, 10000.0);";
        sqlite3_stmt* bal = nullptr;
        sqlite3_prepare_v2(db_, bal_sql, -1, &bal, nullptr);
        sqlite3_bind_int64(bal, 1, user_id);
        sqlite3_step(bal);
        sqlite3_finalize(bal);

        return {true, "", user_id};
    }

    // ── Login — verify password, create session, return token ────
    // Returns ok=true and token in the error field (reusing the struct)
    // Returns ok=false with error message on bad credentials
    Result login(const std::string& username, const std::string& password) {

        // 1. look up user by username
        const char* sql =
            "SELECT id, password_hash FROM users WHERE username = ?;";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt);

        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return {false, "Invalid username or password"};
        }

        int64_t user_id = sqlite3_column_int64(stmt, 0);
        std::string stored_hash = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 1)
        );
        sqlite3_finalize(stmt);

        // 2. verify password against stored hash
        if (crypto_pwhash_str_verify(
                stored_hash.c_str(),
                password.c_str(), password.size()) != 0) {
            return {false, "Invalid username or password"};
        }
        // note: same error message for bad username AND bad password
        // never tell the client which one was wrong — security best practice

        // 3. generate a random session token
        std::string token = generate_token();

        // 4. store session with 24 hour expiry
        int64_t expires_at = static_cast<int64_t>(std::time(nullptr)) + 86400;

        const char* session_sql =
            "INSERT INTO sessions (token, user_id, expires_at) VALUES (?, ?, ?);";

        sqlite3_stmt* sess = nullptr;
        sqlite3_prepare_v2(db_, session_sql, -1, &sess, nullptr);
        sqlite3_bind_text(sess,  1, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(sess, 2, user_id);
        sqlite3_bind_int64(sess, 3, expires_at);
        sqlite3_step(sess);
        sqlite3_finalize(sess);

        // return token in the error field — a bit hacky, we'll clean this up
        return {true, token, user_id};
    }

    // ── Validate a session token ──────────────────────────────────
    // Returns ok=true and user_id in the id field if valid
    // Returns ok=false if token doesn't exist or has expired
    Result validate_session(const std::string& token) {
        int64_t now = static_cast<int64_t>(std::time(nullptr));

        const char* sql =
            "SELECT user_id FROM sessions "
            "WHERE token = ? AND expires_at > ?;";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt,  1, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, now);

        int rc = sqlite3_step(stmt);

        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return {false, "Invalid or expired session"};
        }

        int64_t user_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);

        return {true, "", user_id};
    }

    // ── Get a user's cash balance ─────────────────────────────────
    double get_balance(int64_t user_id) {
        const char* sql =
            "SELECT cash FROM balances WHERE user_id = ?;";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, user_id);

        double cash = 0.0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            cash = sqlite3_column_double(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return cash;
    }

    // ── Get shares owned by a user for a specific company ─────────
    int64_t get_shares(int64_t user_id, int32_t company_id) {
        const char* sql =
            "SELECT shares FROM portfolios "
            "WHERE user_id = ? AND company_id = ?;";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, user_id);
        sqlite3_bind_int(stmt,  2, company_id);

        int64_t shares = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            shares = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return shares;
    }

    // ── Deduct cash (called when a BUY order is placed) ──────────
    Result reserve_cash(int64_t user_id, double amount) {
        double current = get_balance(user_id);
        if (current < amount) {
            return {false, "Insufficient funds. Have $" +
                std::to_string(current) + ", need $" +
                std::to_string(amount)};
        }

        const char* sql =
            "UPDATE balances SET cash = cash - ? WHERE user_id = ?;";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_double(stmt, 1, amount);
        sqlite3_bind_int64(stmt, 2, user_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        return {true, ""};
    }

    // ── Deduct shares (called when a SELL order is placed) ────────
    Result reserve_shares(int64_t user_id, int32_t company_id, int64_t quantity) {
        int64_t current = get_shares(user_id, company_id);
        if (current < quantity) {
            return {false, "Insufficient shares. Have " +
                std::to_string(current) + ", need " +
                std::to_string(quantity)};
        }

        const char* sql =
            "UPDATE portfolios SET shares = shares - ? "
            "WHERE user_id = ? AND company_id = ?;";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, quantity);
        sqlite3_bind_int64(stmt, 2, user_id);
        sqlite3_bind_int(stmt,  3, company_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        return {true, ""};
    }

    // ── Settle a trade — called inside on_trade callback ──────────
    // buyer receives shares, seller receives cash
    void settle_trade(int64_t buyer_id, int64_t seller_id,
                    int32_t company_id, int64_t quantity, double price) {

        double total = price * quantity;

        // credit shares to buyer
        const char* share_sql =
            "INSERT INTO portfolios (user_id, company_id, shares) "
            "VALUES (?, ?, ?) "
            "ON CONFLICT (user_id, company_id) "
            "DO UPDATE SET shares = shares + excluded.shares;";

        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_, share_sql, -1, &s, nullptr);
        sqlite3_bind_int64(s, 1, buyer_id);
        sqlite3_bind_int(s,  2, company_id);
        sqlite3_bind_int64(s, 3, quantity);
        sqlite3_step(s);
        sqlite3_finalize(s);

        // credit cash to seller
        const char* cash_sql =
            "UPDATE balances SET cash = cash + ? WHERE user_id = ?;";

        sqlite3_stmt* c = nullptr;
        sqlite3_prepare_v2(db_, cash_sql, -1, &c, nullptr);
        sqlite3_bind_double(c, 1, total);
        sqlite3_bind_int64(c, 2, seller_id);
        sqlite3_step(c);
        sqlite3_finalize(c);
    }

private:
    sqlite3* db_ = nullptr;

    // ── Create tables if they don't exist yet ──
    void initialise() {
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
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string msg = err;
            sqlite3_free(err);
            throw std::runtime_error("Failed to initialise schema: " + msg);
        }
        std::cout << "Schema ready.\n";
    }

    std::string generate_token() {
        static const char chars[] =
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, sizeof(chars) - 2);
        std::string token(32, ' ');
        for (auto& c : token) c = chars[dis(gen)];
        return token;
    }
};