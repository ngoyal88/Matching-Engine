// ============================================================================
// FILE: tests/test_order_book.cpp (ENHANCED TESTS)
// ============================================================================
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include "../include/order_book.h"
#include "../include/order.h"

void test_basic_matching() {
    std::cout << "[TEST] Basic limit order matching...\n";
    OrderBook ob("BTC-USDT");

    Order sell{"S1", "BTC-USDT", "limit", "sell", 1000000, 1000000, 
               std::chrono::system_clock::now()};
    ob.add_order(sell);

    Order buy{"B1", "BTC-USDT", "limit", "buy", 500000, 1100000, 
              std::chrono::system_clock::now()};
    auto trades = ob.add_order(buy);
    
    assert(trades.size() == 1);
    assert(trades[0].quantity == 500000);
    assert(trades[0].price == 1000000);
    std::cout << "[TEST] PASS - Basic matching passed\n";
}

void test_market_order() {
    std::cout << "[TEST] Market order execution...\n";
    OrderBook ob("BTC-USDT");
    
    Order s1{"S1", "BTC-USDT", "limit", "sell", 300000, 1000000, 
             std::chrono::system_clock::now()};
    Order s2{"S2", "BTC-USDT", "limit", "sell", 300000, 1000000, 
             std::chrono::system_clock::now()};
    ob.add_order(s1);
    ob.add_order(s2);

    Order market_buy{"B1", "BTC-USDT", "market", "buy", 500000, 0, 
                     std::chrono::system_clock::now()};
    auto trades = ob.add_order(market_buy);
    
    long long sum = 0;
    for (auto &t: trades) sum += t.quantity;
    assert(sum == 500000);
    std::cout << "[TEST] PASS - Market order passed\n";
}

void test_ioc_order() {
    std::cout << "[TEST] IOC (Immediate-or-Cancel) order...\n";
    OrderBook ob("BTC-USDT");
    
    Order sell{"S1", "BTC-USDT", "limit", "sell", 300000, 1000000, 
               std::chrono::system_clock::now()};
    ob.add_order(sell);
    
    Order ioc{"B1", "BTC-USDT", "ioc", "buy", 500000, 1100000, 
              std::chrono::system_clock::now()};
    auto trades = ob.add_order(ioc);
    
    // Should fill 300000, cancel remaining 200000
    assert(trades.size() == 1);
    assert(trades[0].quantity == 300000);
    
    // Check no resting order on book
    auto bids = ob.top_bids(10);
    assert(bids.empty());
    std::cout << "[TEST] PASS - IOC order passed\n";
}

void test_fok_order() {
    std::cout << "[TEST] FOK (Fill-or-Kill) order...\n";
    OrderBook ob("BTC-USDT");
    
    Order sell{"S1", "BTC-USDT", "limit", "sell", 300000, 1000000, 
               std::chrono::system_clock::now()};
    ob.add_order(sell);
    
    // FOK that cannot be fully filled - should be cancelled
    Order fok1{"B1", "BTC-USDT", "fok", "buy", 500000, 1100000, 
               std::chrono::system_clock::now()};
    auto trades1 = ob.add_order(fok1);
    assert(trades1.empty()); // Order cancelled
    
    // FOK that can be fully filled
    Order fok2{"B2", "BTC-USDT", "fok", "buy", 300000, 1100000, 
               std::chrono::system_clock::now()};
    auto trades2 = ob.add_order(fok2);
    assert(trades2.size() == 1);
    assert(trades2[0].quantity == 300000);
    
    std::cout << "[TEST] PASS - FOK order passed\n";
}

void test_partial_fill() {
    std::cout << "[TEST] Partial fill with resting order...\n";
    OrderBook ob("BTC-USDT");
    
    Order s1{"S1", "BTC-USDT", "limit", "sell", 300000, 1000000, 
             std::chrono::system_clock::now()};
    ob.add_order(s1);

    Order buy{"B1", "BTC-USDT", "limit", "buy", 500000, 1100000, 
              std::chrono::system_clock::now()};
    auto trades = ob.add_order(buy);
    
    assert(trades.size() == 1);
    auto bids = ob.top_bids(5);
    assert(bids.size() == 1);
    assert(bids[0].second == 200000); // Remaining quantity
    std::cout << "[TEST] PASS - Partial fill passed\n";
}

void test_price_time_priority() {
    std::cout << "[TEST] Price-time priority (FIFO)...\n";
    OrderBook ob("BTC-USDT");
    
    // Add three sell orders at same price
    Order s1{"S1", "BTC-USDT", "limit", "sell", 100000, 1000000, 
             std::chrono::system_clock::now()};
    Order s2{"S2", "BTC-USDT", "limit", "sell", 100000, 1000000, 
             std::chrono::system_clock::now()};
    Order s3{"S3", "BTC-USDT", "limit", "sell", 100000, 1000000, 
             std::chrono::system_clock::now()};
    
    ob.add_order(s1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ob.add_order(s2);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ob.add_order(s3);
    
    // Buy order should match S1 first (FIFO)
    Order buy{"B1", "BTC-USDT", "limit", "buy", 100000, 1100000, 
              std::chrono::system_clock::now()};
    auto trades = ob.add_order(buy);
    
    assert(trades.size() == 1);
    assert(trades[0].maker_order_id == "S1");
    std::cout << "[TEST] PASS - Price-time priority passed\n";
}

void test_fee_calculation() {
    std::cout << "[TEST] Fee calculation...\n";
    OrderBook ob("BTC-USDT");
    
    FeeConfig fees;
    fees.maker_fee_bps = 10;  // 0.10%
    fees.taker_fee_bps = 20;  // 0.20%
    ob.set_fee_config(fees);
    
    Order sell{"S1", "BTC-USDT", "limit", "sell", 1000000, 5000000, 
               std::chrono::system_clock::now()};
    ob.add_order(sell);
    
    Order buy{"B1", "BTC-USDT", "limit", "buy", 1000000, 5000000, 
              std::chrono::system_clock::now()};
    auto trades = ob.add_order(buy);
    
    assert(trades.size() == 1);
    assert(trades[0].maker_fee > 0);
    assert(trades[0].taker_fee > 0);
    assert(trades[0].taker_fee == 2 * trades[0].maker_fee); // 2x ratio
    
    std::cout << "[TEST] PASS - Fee calculation passed\n";
}

void run_order_book_tests() {
    std::cout << "\n========================================\n";
    std::cout << "  Running Order Book Tests\n";
    std::cout << "========================================\n\n";
    
    test_basic_matching();
    test_market_order();
    test_ioc_order();
    test_fok_order();
    test_partial_fill();
    test_price_time_priority();
    test_fee_calculation();
    
    std::cout << "\n========================================\n";
    std::cout << "  All Tests Passed!\n";
    std::cout << "========================================\n\n";
}