// FILE: src/order_book.cpp
#include "../include/order_book.h"
#include <algorithm>   // std::min
#include <atomic>      // std::atomic
#include <cstdint>     // uint64_t
#include <chrono>
#include <ctime>       // std::gmtime
#include <iomanip>
#include <sstream>
using namespace std;

// Simple counter-based trade id (avoids optional platform UUID dependencies)
static atomic<uint64_t> trade_counter{1};

static string make_trade_id() {
    auto id = trade_counter.fetch_add(1);
    ostringstream ss; ss << "T-" << id;
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

vector<Trade> OrderBook::add_order(const Order &order) {
    vector<Trade> trades;
    lock_guard<mutex> lk(mu_);

    long long remaining = order.quantity;
    bool is_buy = (order.side == "buy");

    // Helper lambdas
    auto match_against_book = [&](auto &book_map, [[maybe_unused]] bool book_is_ask) {
        // book_map is asks_ when incoming is buy, and bids_ when incoming is sell
        while (remaining > 0 && !book_map.empty()) {
            auto it = book_map.begin(); // best price level
            long long price_level = it->first;

            // check price constraints for limit orders
            if (order.order_type == "limit") {
                if (is_buy && price_level > order.price) break; // best ask > buy limit
                if (!is_buy && price_level < order.price) break; // best bid < sell limit
            }

            auto &q = it->second;
            while (remaining > 0 && !q.empty()) {
                Order maker = q.front();
                long long trade_qty = min(remaining, maker.quantity);

                // produce trade
                Trade tr;
                tr.trade_id = make_trade_id();
                tr.symbol = symbol_;
                tr.price = price_level;
                tr.quantity = trade_qty;
                tr.aggressor_side = order.side;
                tr.maker_order_id = maker.order_id;
                tr.taker_order_id = order.order_id;
                tr.timestamp_iso = now_iso();

                trades.push_back(tr);

                // update quantities
                remaining -= trade_qty;
                maker.quantity -= trade_qty;

                // pop or update maker in-place
                q.pop_front();
                if (maker.quantity > 0) {
                    // remaining part of maker goes back to front (preserve FIFO)
                    q.push_front(maker);
                } else {
                    // fully filled -> remove index
                    order_index_.erase(tr.maker_order_id);
                }
            }

            // if price level empty, erase it
            if (q.empty()) {
                book_map.erase(it);
            } else {
                // if incoming is limit and still cannot trade at this price, stop
                // but above we already enforced price constraints
                ;
            }
        }
    };

    if (is_buy) {
        // match against asks (lowest first)
        match_against_book(asks_, true);
    } else {
        // sell -> match against bids (highest first)
        match_against_book(bids_, false);
    }

    // If there is remaining quantity and order is limit => rest on book
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
        auto &book_map = bids_;
        auto lvl_it = book_map.find(price);
        if (lvl_it == book_map.end()) return false;

        auto &dq = lvl_it->second;
        for (auto dq_it = dq.begin(); dq_it != dq.end(); ++dq_it) {
            if (dq_it->order_id == order_id) {
                dq.erase(dq_it);
                order_index_.erase(it);
                if (dq.empty()) book_map.erase(lvl_it);
                return true;
            }
        }
    } else {
        auto &book_map = asks_;
        auto lvl_it = book_map.find(price);
        if (lvl_it == book_map.end()) return false;

        auto &dq = lvl_it->second;
        for (auto dq_it = dq.begin(); dq_it != dq.end(); ++dq_it) {
            if (dq_it->order_id == order_id) {
                dq.erase(dq_it);
                order_index_.erase(it);
                if (dq.empty()) book_map.erase(lvl_it);
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
