// Minimal test executable using assert
#include <cassert>
#include <chrono>
#include "../include/order_store.h"

// forward declaration implemented in test_order_book.cpp
void run_order_book_tests();

int main() {
    OrderStore store("./data/test_wal.jsonl");
    Order o1;
    o1.order_id = store.next_id();
    o1.symbol = "BTC-USDT";
    o1.order_type = "limit";
    o1.side = "buy";
    o1.quantity = 1000000;
    o1.price = 5000000;
    o1.timestamp = std::chrono::system_clock::now();

    store.add_order(o1);
    assert(store.has_order(o1.order_id));

    Order o2;
    o2.order_id = store.next_id();
    assert(o2.order_id != o1.order_id);

    // run order_book tests
    run_order_book_tests();

    return 0;
}