#include <iostream>
#include <boost/asio/io_context.hpp>
#include "OrderBook.h"
#include "Market.h"
#include "Server.h"
#include "Database.h"

int main() {
    if (sodium_init() < 0) {
        std::cerr << "Failed to initialise libsodium\n";
        return 1;
    }
    
    MarketState market({
        {1, "APL", "Apollo Technologies", 1'000'000},
        {2, "BLZ", "Blaze Manufacturing", 2'500'000},
        {3, "CRN", "Crown Energy", 1'750'000}
    });
    Database test_db("exchange.db");
    test_db.settle_trade(1, 1, 1, 500, 0.0); // give shrey 500 APL shares free
    
    boost::asio::io_context ioc;
    Server server(ioc, 9001, market);
    server.run();

    return 0;
}
