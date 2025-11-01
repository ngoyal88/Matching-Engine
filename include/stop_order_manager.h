// ============================================================================
// FILE: include/stop_order_manager.h (NEW - Advanced Order Types)
// ============================================================================
#pragma once
#include "order.h"
#include "order_book.h"
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <atomic>
#include <cstdint>
#include <chrono>
#include "../vendor/json.hpp" // <-- ADDED
#include <iomanip>           // <-- ADDED
#include <sstream>           // <-- ADDED

using json = nlohmann::json; // <-- ADDED

enum class StopOrderType {
    STOP_LOSS,      // Trigger market order when price hits trigger
    STOP_LIMIT,     // Trigger limit order when price hits trigger
    TAKE_PROFIT,    // Similar to stop-loss but opposite direction
    TRAILING_STOP   // Dynamic stop that trails the market price
};

struct StopOrder {
    std::string order_id;
    std::string symbol;
    StopOrderType stop_type;
    std::string side;  // buy or sell
    long long trigger_price;
    long long limit_price;  // For stop-limit orders
    long long quantity;
    long long trail_amount;  // For trailing stops
    std::chrono::system_clock::time_point created_at;
    std::string user_id;
    
    // Trailing stop tracking
    long long best_price;  // Track best price seen

    // --- ADDED HELPER FUNCTION ---
    static StopOrder from_json(const json& j) {
        StopOrder so;
        so.order_id = j.at("order_id").get<std::string>();
        so.symbol = j.at("symbol").get<std::string>();
        so.side = j.at("side").get<std::string>();
        so.quantity = j.at("quantity").get<long long>();
        so.trigger_price = j.at("trigger_price").get<long long>();
        
        std::string stop_type_str = j.at("stop_type").get<std::string>();
        if (stop_type_str == "stop_limit") {
            so.stop_type = StopOrderType::STOP_LIMIT;
            so.limit_price = j.at("limit_price").get<long long>();
        } else {
            so.stop_type = StopOrderType::STOP_LOSS;
            so.limit_price = 0;
        }
        
        // Note: We don't need to parse the timestamp for replay logic,
        // but you could add it here if needed.
        so.best_price = so.trigger_price; // Default best price
        so.trail_amount = 0; // Default
        
        return so;
    }
};

class StopOrderManager {
// ... (rest of the file is unchanged)
public:
    explicit StopOrderManager(const std::string &symbol);
    
    // Add stop order
    std::string add_stop_order(const StopOrder &order);
    
    // Cancel stop order
    bool cancel_stop_order(const std::string &order_id);
    
    // Check if any stop orders should trigger at current price
    std::vector<Order> check_triggers(long long last_trade_price);
    
    // --- ADDED FOR WAL REPLAY ---
    void add_stop_order_from_replay(const StopOrder &order);

    // Update trailing stops
    void update_trailing_stops(long long current_price);
    
    // Get all active stop orders
    std::vector<StopOrder> get_active_stops() const;
    
private:
    std::string symbol_;
    
    // Buy stop orders trigger when price rises
    std::multimap<long long, StopOrder> buy_stops_;
    
    // Sell stop orders trigger when price falls
    std::multimap<long long, StopOrder> sell_stops_;
    
    std::unordered_map<std::string, long long> order_index_;  // order_id -> trigger_price
    
    std::mutex mu_;
    std::atomic<std::uint64_t> stop_order_counter_{1};
    
    std::string generate_stop_order_id();
};

