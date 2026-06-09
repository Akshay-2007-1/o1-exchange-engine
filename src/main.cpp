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
    auto res_shrey = test_db.create_user("shrey", "pass123");
    test_db.create_user("akshay", "pass123");
    if (res_shrey.ok) {
        test_db.settle_trade(res_shrey.id, res_shrey.id, 1, 500, 0.0); // give shrey 500 APL shares free
    }
    
    boost::asio::io_context ioc;
    Server server(ioc, 9001, market);
    
    std::cout << "WebSocket server listening on port 9001...\n";
    
    // ioc.run() blocks the main thread and drives all the async_read/write callbacks
    ioc.run();

    return 0;
}