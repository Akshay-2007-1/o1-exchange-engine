#include <iostream>
#include <boost/asio/io_context.hpp>
#include "OrderBook.h"
#include "Market.h"
#include "Server.h"
#include "Database.h"

int main() {
    if (sodium_init() < 0) return 1;

    // fresh start
    Database db("exchange.db");

    // create two users
    db.create_user("buyer",  "pass1");
    db.create_user("seller", "pass2");

    // give seller some shares of company 1 manually
    // (in production this happens via the company role)
    db.settle_trade(2, 2, 1, 500, 0.0); // give seller 500 shares at no cost

    // check starting state
    std::cout << "Buyer cash:    $" << db.get_balance(1) << "\n";
    std::cout << "Seller cash:   $" << db.get_balance(2) << "\n";
    std::cout << "Seller shares: "  << db.get_shares(2, 1) << "\n\n";

    // seller tries to reserve 100 shares for a sell order
    auto r1 = db.reserve_shares(2, 1, 100);
    std::cout << (r1.ok ? "Shares reserved" : "Error: " + r1.error) << "\n";

    // buyer tries to buy 100 shares @ $102.50
    auto r2 = db.reserve_cash(1, 100 * 102.50);
    std::cout << (r2.ok ? "Cash reserved" : "Error: " + r2.error) << "\n";

    // simulate trade executing
    db.settle_trade(1, 2, 1, 100, 102.50);

    // check final state
    std::cout << "\nAfter trade:\n";
    std::cout << "Buyer cash:    $" << db.get_balance(1) << "\n";
    std::cout << "Buyer shares:  "  << db.get_shares(1, 1) << "\n";
    std::cout << "Seller cash:   $" << db.get_balance(2) << "\n";
    std::cout << "Seller shares: "  << db.get_shares(2, 1) << "\n";

    // try to sell more shares than available
    auto r3 = db.reserve_shares(2, 1, 10000);
    std::cout << "\n" << (r3.ok ? "Reserved" : "Error: " + r3.error) << "\n";
    
    MarketState market({
        {1, "APL", "Apollo Technologies", 1'000'000},
        {2, "BLZ", "Blaze Manufacturing", 2'500'000},
        {3, "CRN", "Crown Energy", 1'750'000}
    });

    boost::asio::io_context ioc;
    Server server(ioc, 9001, market);
    server.run();

    return 0;
}
