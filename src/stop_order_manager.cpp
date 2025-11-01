#include "../include/stop_order_manager.h"
#include <sstream>
#include <algorithm>

StopOrderManager::StopOrderManager(const std::string &symbol) : symbol_(symbol) {}

std::string StopOrderManager::generate_stop_order_id() {
    auto id = stop_order_counter_.fetch_add(1);
    std::ostringstream ss;
    ss << "STOP-" << id;
    return ss.str();
}

std::string StopOrderManager::add_stop_order(const StopOrder &order) {
    std::lock_guard<std::mutex> lock(mu_);
    
    StopOrder stop = order;
    if (stop.order_id.empty()) {
        stop.order_id = generate_stop_order_id();
    }
    
    // Initialize best price for trailing stops
    if (stop.stop_type == StopOrderType::TRAILING_STOP) {
        stop.best_price = stop.trigger_price;
    }
    
    // Add to appropriate map
    if (stop.side == "buy") {
        buy_stops_.insert({stop.trigger_price, stop});
    } else {
        sell_stops_.insert({stop.trigger_price, stop});
    }
    
    order_index_[stop.order_id] = stop.trigger_price;
    
    return stop.order_id;
}

void StopOrderManager::add_stop_order_from_replay(const StopOrder &order) {
    std::lock_guard<std::mutex> lock(mu_);
    
    // Add to appropriate map
    if (order.side == "buy") {
        buy_stops_.insert({order.trigger_price, order});
    } else {
        sell_stops_.insert({order.trigger_price, order});
    }
    
    // Add to index for cancellation
    order_index_[order.order_id] = order.trigger_price;
}

bool StopOrderManager::cancel_stop_order(const std::string &order_id) {
    std::lock_guard<std::mutex> lock(mu_);
    
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;
    
    long long trigger_price = it->second;
    
    // Search in buy stops
    auto range = buy_stops_.equal_range(trigger_price);
    for (auto iter = range.first; iter != range.second; ++iter) {
        if (iter->second.order_id == order_id) {
            buy_stops_.erase(iter);
            order_index_.erase(it);
            return true;
        }
    }
    
    // Search in sell stops
    range = sell_stops_.equal_range(trigger_price);
    for (auto iter = range.first; iter != range.second; ++iter) {
        if (iter->second.order_id == order_id) {
            sell_stops_.erase(iter);
            order_index_.erase(it);
            return true;
        }
    }
    
    return false;
}

std::vector<Order> StopOrderManager::check_triggers(long long last_trade_price) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Order> triggered_orders;
    
    // Check buy stops (trigger when price rises above trigger)
    auto buy_it = buy_stops_.begin();
    while (buy_it != buy_stops_.end()) {
        if (last_trade_price >= buy_it->first) {
            const StopOrder &stop = buy_it->second;
            
            Order order;
            order.order_id = stop.order_id;
            order.symbol = stop.symbol;
            order.side = stop.side;
            order.quantity = stop.quantity;
            order.timestamp = std::chrono::system_clock::now();
            
            if (stop.stop_type == StopOrderType::STOP_LIMIT) {
                order.order_type = "limit";
                order.price = stop.limit_price;
            } else {
                order.order_type = "market";
                order.price = 0;
            }
            
            triggered_orders.push_back(order);
            order_index_.erase(stop.order_id);
            buy_it = buy_stops_.erase(buy_it);
        } else {
            break;  // Stops are sorted, no need to check further
        }
    }
    
    // Check sell stops (trigger when price falls below trigger)
    auto sell_it = sell_stops_.begin();
    while (sell_it != sell_stops_.end()) {
        if (last_trade_price <= sell_it->first) {
            const StopOrder &stop = sell_it->second;
            
            Order order;
            order.order_id = stop.order_id;
            order.symbol = stop.symbol;
            order.side = stop.side;
            order.quantity = stop.quantity;
            order.timestamp = std::chrono::system_clock::now();
            
            if (stop.stop_type == StopOrderType::STOP_LIMIT) {
                order.order_type = "limit";
                order.price = stop.limit_price;
            } else {
                order.order_type = "market";
                order.price = 0;
            }
            
            triggered_orders.push_back(order);
            order_index_.erase(stop.order_id);
            sell_it = sell_stops_.erase(sell_it);
        } else {
            break;
        }
    }
    
    return triggered_orders;
}

void StopOrderManager::update_trailing_stops(long long current_price) {
    std::lock_guard<std::mutex> lock(mu_);
    
    // Update buy trailing stops (track lowest price)
    for (auto &pair : buy_stops_) {
        StopOrder &stop = const_cast<StopOrder&>(pair.second);
        if (stop.stop_type == StopOrderType::TRAILING_STOP) {
            if (current_price < stop.best_price) {
                stop.best_price = current_price;
                // Adjust trigger price
                stop.trigger_price = current_price + stop.trail_amount;
            }
        }
    }
    
    // Update sell trailing stops (track highest price)
    for (auto &pair : sell_stops_) {
        StopOrder &stop = const_cast<StopOrder&>(pair.second);
        if (stop.stop_type == StopOrderType::TRAILING_STOP) {
            if (current_price > stop.best_price) {
                stop.best_price = current_price;
                // Adjust trigger price
                stop.trigger_price = current_price - stop.trail_amount;
            }
        }
    }
}

std::vector<StopOrder> StopOrderManager::get_active_stops() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mu_));
    std::vector<StopOrder> stops;
    
    for (const auto &pair : buy_stops_) {
        stops.push_back(pair.second);
    }
    for (const auto &pair : sell_stops_) {
        stops.push_back(pair.second);
    }
    
    return stops;
}