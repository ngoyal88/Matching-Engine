// FILE: tests/test_order_book.cpp (assert-based tests)
#include <cassert>
#include <chrono>
#include "../include/order_book.h"
#include "../include/order.h"

void run_order_book_tests() {
    // Case 1: Limit buy matches resting limit sell at best price with FIFO
    {
        OrderBook ob("BTC-USDT");

        Order sell;
        sell.order_id = "S1";
        sell.symbol = "BTC-USDT";
        sell.order_type = "limit";
        sell.side = "sell";
        sell.quantity = 1000000; // 1.0
        sell.price = 1000000; // 10000.00 * 100 -> example scaling
        sell.timestamp = std::chrono::system_clock::now();
        ob.add_order(sell);

        Order buy;
        buy.order_id = "B1";
        buy.symbol = "BTC-USDT";
        buy.order_type = "limit";
        buy.side = "buy";
        buy.quantity = 500000; // 0.5
        buy.price = 1100000; // 11000.00
        buy.timestamp = std::chrono::system_clock::now();

        auto trades = ob.add_order(buy);
        assert(trades.size() == 1);
        assert(trades[0].quantity == 500000);
        assert(trades[0].price == 1000000); // matched at maker price
    }

    // Case 2: Market order consumes best available liquidity
    {
        OrderBook ob("BTC-USDT");
        Order s1{ "S1", "BTC-USDT", "limit", "sell", 300000, 1000000, std::chrono::system_clock::now() };
        Order s2{ "S2", "BTC-USDT", "limit", "sell", 300000, 1000000, std::chrono::system_clock::now() };
        ob.add_order(s1);
        ob.add_order(s2);

        Order market_buy{ "B1", "BTC-USDT", "market", "buy", 500000, 0, std::chrono::system_clock::now() };
        auto trades = ob.add_order(market_buy);
        long long sum = 0;
        for (auto &t: trades) sum += t.quantity;
        assert(sum == 500000);
    }

    // Case 3: Partial fill rests remainder on book for limit orders
    {
        OrderBook ob("BTC-USDT");
        Order s1{ "S1", "BTC-USDT", "limit", "sell", 300000, 1000000, std::chrono::system_clock::now() };
        ob.add_order(s1);

        Order buy{ "B1", "BTC-USDT", "limit", "buy", 500000, 1100000, std::chrono::system_clock::now() };
        auto trades = ob.add_order(buy);
        assert(trades.size() == 1);
        auto bids = ob.top_bids(5);
        assert(bids.size() == 1);
        assert(bids[0].second == 200000);
    }
}
