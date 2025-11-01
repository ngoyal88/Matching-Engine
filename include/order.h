// ============================================================================
// FILE: include/order.h
// ============================================================================
#pragma once
#include <string>
#include <chrono>
#include "../vendor/json.hpp" // <-- ADDED
#include <iomanip>           // <-- ADDED
#include <sstream>           // <-- ADDED

using json = nlohmann::json; // <-- ADDED
    
struct Order {
    std::string order_id;
    std::string symbol;
    std::string order_type; // market, limit, ioc, fok
    std::string side; // buy or sell
    long long quantity;
    long long price; // 0 for market orders
    std::chrono::system_clock::time_point timestamp;

    // --- ADDED HELPER FUNCTION ---
    static Order from_json(const json& j) {
        Order o;
        o.order_id = j.at("order_id").get<std::string>();
        o.symbol = j.at("symbol").get<std::string>();
        o.order_type = j.at("order_type").get<std::string>();
        o.side = j.at("side").get<std::string>();
        o.quantity = j.at("quantity").get<long long>();
        o.price = j.at("price").get<long long>();
        
        // Deserialize ISO 8601 timestamp
        std::string ts_str = j.at("timestamp").get<std::string>();
        std::tm tm = {};
        std::stringstream ss(ts_str);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        o.timestamp = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        
        return o;
    }
};

