#include <iostream>
#include <boost/asio/io_context.hpp>
#include "OrderBook.h"
#include "Server.h"

int main() {
    OrderBook book;

    boost::asio::io_context ioc;
    Server server(ioc, 9001, book);
    server.run();

    return 0;
}