// ============================================================================
// FILE: include/order.h
// ============================================================================
#pragma once
#include <string>
#include <chrono>
#include "../vendor/json.hpp" // <-- ADDED
    
struct Order {
    std::string order_id;
    std::string symbol;
    std::string order_type; // market, limit, ioc, fok
    std::string side; // buy or sell
    long long quantity;
    long long price; // 0 for market orders
    std::chrono::system_clock::time_point timestamp;

    /**
     * @brief Helper to reconstruct an Order object from JSON (for WAL replay).
     * * Assumes timestamp is stored as "timestamp_ns" (nanoseconds).
     */
    static Order from_json(const nlohmann::json& j) {
        Order o;
        o.order_id = j.at("order_id").get<std::string>();
        o.symbol = j.at("symbol").get<std::string>();
        o.order_type = j.at("order_type").get<std::string>();
        o.side = j.at("side").get<std::string>();
        o.quantity = j.at("quantity").get<long long>();
        o.price = j.at("price").get<long long>();
        
        // Reconstruct timestamp from nanoseconds
        long long ts_ns = j.value("timestamp_ns", 0LL);
        if (ts_ns > 0) {
            o.timestamp = std::chrono::system_clock::time_point(std::chrono::nanoseconds(ts_ns));
        } else {
            o.timestamp = std::chrono::system_clock::now(); // Fallback
        }
        
        return o;
    }
};
