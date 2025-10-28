// FILE: include/order_book.h
#pragma once

#include "order.h"
#include <map>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

struct Trade {
    std::string trade_id;
    std::string symbol;
    long long price;       // scaled integer price (0 for market-fill with no price)
    long long quantity;    // scaled integer quantity
    std::string aggressor_side; // "buy" or "sell"
    std::string maker_order_id;
    std::string taker_order_id;
    std::string timestamp_iso;
};

class OrderBook {
public:
    explicit OrderBook(const std::string &symbol);

    // Add an order to the book and return any resulting trades (0..n trades)
    std::vector<Trade> add_order(const Order &order);

    // Cancel a resting order. Returns true if removed.
    bool cancel_order(const std::string &order_id);

    // Snapshot top N levels for bids/asks
    std::vector<std::pair<long long,long long>> top_bids(size_t n);
    std::vector<std::pair<long long,long long>> top_asks(size_t n);

private:
    std::string symbol_;

    // price -> deque of orders. Use descending order for bids (highest first)
    std::map<long long, std::deque<Order>, std::greater<long long>> bids_;
    // ascending order for asks (lowest first)
    std::map<long long, std::deque<Order>> asks_;

    // index to find a resting order: order_id -> (price, is_buy)
    std::unordered_map<std::string, std::pair<long long,bool>> order_index_;

    // protect whole book with single mutex (single-thread per symbol recommended)
    std::mutex mu_;

    // helper to get ISO timestamp
    static std::string now_iso();
};
