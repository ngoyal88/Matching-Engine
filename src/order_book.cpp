// ============================================================================
// FILE: src/order_book.cpp
// ============================================================================
#include "../include/order_book.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
using namespace std;

static atomic<uint64_t> trade_counter{1};

static string make_trade_id() {
    auto id = trade_counter.fetch_add(1);
    ostringstream ss; 
    ss << "T-" << id;
    return ss.str();
}

string OrderBook::now_iso() {
    auto tp = chrono::system_clock::now();
    auto t = chrono::system_clock::to_time_t(tp);
    ostringstream ss;
    ss << put_time(gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

OrderBook::OrderBook(const string &symbol) : symbol_(symbol) {}

void OrderBook::calculate_fees(Trade &trade) {
    long long notional = (trade.price * trade.quantity) / 100000000LL;
    trade.maker_fee = (notional * fee_config_.maker_fee_bps) / 10000LL;
    trade.taker_fee = (notional * fee_config_.taker_fee_bps) / 10000LL;
}

vector<Trade> OrderBook::add_order(const Order &order) {
    vector<Trade> trades;
    lock_guard<mutex> lk(mu_);

    long long remaining = order.quantity;
    long long original_qty = order.quantity;
    bool is_buy = (order.side == "buy");

    // Pre-check for FOK: ensure full fillability without mutating the book
    if (order.order_type == "fok") {
        long long fillable = 0;
        if (is_buy) {
            for (const auto &level : asks_) {
                long long price_level = level.first;
                if (order.price > 0 && price_level > order.price) break; // price constraint
                for (const auto &o : level.second) {
                    fillable += o.quantity;
                    if (fillable >= order.quantity) break;
                }
                if (fillable >= order.quantity) break;
            }
        } else { // sell
            for (const auto &level : bids_) {
                long long price_level = level.first;
                if (order.price > 0 && price_level < order.price) break; // price constraint
                for (const auto &o : level.second) {
                    fillable += o.quantity;
                    if (fillable >= order.quantity) break;
                }
                if (fillable >= order.quantity) break;
            }
        }
        if (fillable < order.quantity) {
            // Not fully fillable: cancel without side-effects
            return trades; // empty
        }
    }

    auto match_against_book = [&](auto &book_map) {
        while (remaining > 0 && !book_map.empty()) {
            auto it = book_map.begin();
            long long price_level = it->first;

            // Price constraint for limit orders
            if (order.order_type == "limit") {
                if (is_buy && price_level > order.price) break;
                if (!is_buy && price_level < order.price) break;
            }

            auto &q = it->second;
            while (remaining > 0 && !q.empty()) {
                Order maker = q.front();
                long long trade_qty = min(remaining, maker.quantity);

                Trade tr;
                tr.trade_id = make_trade_id();
                tr.symbol = symbol_;
                tr.price = price_level;
                tr.quantity = trade_qty;
                tr.aggressor_side = order.side;
                tr.maker_order_id = maker.order_id;
                tr.taker_order_id = order.order_id;
                tr.timestamp_iso = now_iso();
                calculate_fees(tr);

                trades.push_back(tr);

                remaining -= trade_qty;
                maker.quantity -= trade_qty;

                q.pop_front();
                if (maker.quantity > 0) {
                    q.push_front(maker);
                } else {
                    order_index_.erase(maker.order_id);
                }
            }

            if (q.empty()) {
                book_map.erase(it);
            }
        }
    };

    if (is_buy) {
        match_against_book(asks_);
    } else {
        match_against_book(bids_);
    }

    // Handle IOC - cancel unfilled portion
    if (order.order_type == "ioc" && remaining > 0) {
        return trades;
    }

    // Handle FOK - at this point we've ensured full fillability; if anything left, treat as cancel
    if (order.order_type == "fok") {
        // If some remaining (shouldn't happen due to pre-check), cancel without resting
        if (remaining > 0) {
            trades.clear();
        }
        return trades;
    }

    // Rest limit orders on book
    if (remaining > 0 && order.order_type == "limit") {
        Order resting = order;
        resting.quantity = remaining;
        if (is_buy) {
            bids_[order.price].push_back(resting);
        } else {
            asks_[order.price].push_back(resting);
        }
        order_index_[order.order_id] = {order.price, is_buy};
    }

    return trades;
}

bool OrderBook::cancel_order(const string &order_id) {
    lock_guard<mutex> lk(mu_);
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;
    
    long long price = it->second.first;
    bool is_buy = it->second.second;

    if (is_buy) {
        auto lvl_it = bids_.find(price);
        if (lvl_it == bids_.end()) return false;
        auto &dq = lvl_it->second;
        for (auto dq_it = dq.begin(); dq_it != dq.end(); ++dq_it) {
            if (dq_it->order_id == order_id) {
                dq.erase(dq_it);
                order_index_.erase(it);
                if (dq.empty()) bids_.erase(lvl_it);
                return true;
            }
        }
    } else {
        auto lvl_it = asks_.find(price);
        if (lvl_it == asks_.end()) return false;
        auto &dq = lvl_it->second;
        for (auto dq_it = dq.begin(); dq_it != dq.end(); ++dq_it) {
            if (dq_it->order_id == order_id) {
                dq.erase(dq_it);
                order_index_.erase(it);
                if (dq.empty()) asks_.erase(lvl_it);
                return true;
            }
        }
    }

    return false;
}

vector<pair<long long,long long>> OrderBook::top_bids(size_t n) {
    lock_guard<mutex> lk(mu_);
    vector<pair<long long,long long>> out;
    for (auto it = bids_.begin(); it != bids_.end() && out.size() < n; ++it) {
        long long total = 0;
        for (auto &o : it->second) total += o.quantity;
        out.emplace_back(it->first, total);
    }
    return out;
}

vector<pair<long long,long long>> OrderBook::top_asks(size_t n) {
    lock_guard<mutex> lk(mu_);
    vector<pair<long long,long long>> out;
    for (auto it = asks_.begin(); it != asks_.end() && out.size() < n; ++it) {
        long long total = 0;
        for (auto &o : it->second) total += o.quantity;
        out.emplace_back(it->first, total);
    }
    return out;
}