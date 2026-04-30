#include <iostream>
#include "OrderBook.h"

int main() {
    OrderBook book;

    // print every trade that fires
    book.on_trade = [](const Trade& t) {
        std::cout << "TRADE: " << t.quantity
                  << " shares @ $" << t.price
                  << "  (buy#" << t.buy_order_id
                  << " vs sell#" << t.sell_order_id << ")\n";
    };

    // Alice posts a resting sell: 150 shares @ $102.00
    book.add_order({1, Side::SELL, 102.00, 150, 1000});

    // Bob posts a resting sell: 400 shares @ $102.50
    book.add_order({2, Side::SELL, 102.50, 400, 2000});

    // Carol buys 300 @ $102.50 — should trigger TWO trades:
    //   trade 1: 150 shares @ $102.00 (partial fill from Alice)
    //   trade 2: 150 shares @ $102.50 (partial fill from Bob)
    book.add_order({3, Side::BUY, 102.50, 300, 3000});

    return 0;
}