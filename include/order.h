// ============================================================================
// FILE: include/order.h
// ============================================================================
#pragma once
#include <string>
#include <chrono>
    
struct Order {
    std::string order_id;
    std::string symbol;
    std::string order_type; // market, limit, ioc, fok
    std::string side; // buy or sell
    long long quantity;
    long long price; // 0 for market orders
    std::chrono::system_clock::time_point timestamp;
};