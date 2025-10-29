// ============================================================================
// FILE: include/order_book.h
// ============================================================================
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
     long long price;
     long long quantity;
     std::string aggressor_side;
     std::string maker_order_id;
     std::string taker_order_id;
     std::string timestamp_iso;
     long long maker_fee;
     long long taker_fee;
 };

 struct FeeConfig {
     long long maker_fee_bps = 10;  // 0.10%
     long long taker_fee_bps = 20;  // 0.20%
 };

 class OrderBook {
public:
     explicit OrderBook(const std::string &symbol);
    
     std::vector<Trade> add_order(const Order &order);
     bool cancel_order(const std::string &order_id);

     std::vector<std::pair<long long,long long>> top_bids(size_t n);
     std::vector<std::pair<long long,long long>> top_asks(size_t n);

     void set_fee_config(const FeeConfig &config) { fee_config_ = config; }

private:
     std::string symbol_;
     std::map<long long, std::deque<Order>, std::greater<long long>> bids_;
     std::map<long long, std::deque<Order>> asks_;
     std::unordered_map<std::string, std::pair<long long,bool>> order_index_;
     std::mutex mu_;
     FeeConfig fee_config_;
    
     static std::string now_iso();
     void calculate_fees(Trade &trade);
 };