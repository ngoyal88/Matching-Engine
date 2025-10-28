// FILE: src/main.cpp
#include <iostream>
#include <thread>
#include "../include/order.h"

// forward decl from server.cpp
void setup_server(int port);

// global order store (defined in order_store.cpp)
// forward declare OrderStore to avoid requiring its full definition here
class OrderStore;
extern OrderStore global_order_store;

int main(int argc, char** argv) {
    int port = 8080;
    if (argc > 1) port = std::stoi(argv[1]);

    std::cout << "Starting matching engine (Milestone 1) on port " << port << "\n";

    // Start server (blocking)
    setup_server(port);

    return 0;
}