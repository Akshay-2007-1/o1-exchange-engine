#pragma once
#include <string>
#include <unordered_map>
#include <mutex>

struct UserAccount {
    std::string username;
    std::string password; 
    double cash_balance;
    std::unordered_map<int, int> share_balances; // company_id -> quantity
};

class UserManager {
private:
    std::unordered_map<std::string, UserAccount> users;
    std::mutex mtx;

public:
    UserManager() {
        // Pre-seed some test users matching your listed companies (APL=1, BLZ=2, CRN=3)
        users["alice"] = {"alice", "pass123", 100000.0, {{1, 500}, {2, 1000}}};
        users["bob"] = {"bob", "pass123", 50000.0, {{1, 100}, {3, 2000}}};
    }

    bool authenticate(const std::string& username, const std::string& password) {
        std::lock_guard<std::mutex> lock(mtx);
        if (users.find(username) != users.end()) {
            return users[username].password == password;
        }
        return false;
    }

    UserAccount get_account(const std::string& username) {
        std::lock_guard<std::mutex> lock(mtx);
        return users[username];
    }

    bool can_place_order(const std::string& username, const std::string& side, int company_id, double price, int quantity) {
        std::lock_guard<std::mutex> lock(mtx);
        if (users.find(username) == users.end()) return false;

        auto& user = users[username];
        if (side == "BUY") {
            double required_cash = price * quantity;
            return user.cash_balance >= required_cash;
        } else if (side == "SELL") {
            return user.share_balances[company_id] >= quantity;
        }
        return false;
    }

    void process_trade_settlement(const std::string& buyer, const std::string& seller, int company_id, double price, int quantity) {
        std::lock_guard<std::mutex> lock(mtx);
        double total_value = price * quantity;

        if (users.find(buyer) != users.end()) {
            users[buyer].cash_balance -= total_value;
            users[buyer].share_balances[company_id] += quantity;
        }
        if (users.find(seller) != users.end()) {
            users[seller].cash_balance += total_value;
            users[seller].share_balances[company_id] -= quantity;
        }
    }
};