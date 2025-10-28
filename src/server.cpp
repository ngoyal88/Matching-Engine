// FILE: src/server.cpp
#include "../include/order.h"
#include "../include/order_store.h"
#include "../include/order_book.h"
#include "../include/wal.h"
#include "../include/ws_server.h"
#include <httplib.h>
#include "../vendor/json.hpp"
#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <unordered_map>
#include <sstream>
#include <ctime>
using json = nlohmann::json;

static std::string to_iso8601(const std::chrono::system_clock::time_point &tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

// Global book store per symbol
static std::unordered_map<std::string, OrderBook> order_books;

// Track which symbol an order belongs to (for cancellation)
static std::unordered_map<std::string, std::string> order_to_symbol;
static std::mutex order_to_symbol_mutex;

void setup_server(int port) {
    httplib::Server svr;

    // Health check
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res){
        res.set_content("OK", "text/plain");
    });

    // Get available symbols
    svr.Get("/symbols", [](const httplib::Request&, httplib::Response& res) {
        json symbols = json::array();
        for (const auto &[symbol, book] : order_books) {
            symbols.push_back(symbol);
        }
        
        json response = {
            {"symbols", symbols},
            {"count", symbols.size()}
        };
        
        res.set_content(response.dump(2), "application/json");
    });

    // Create order
    svr.Post("/orders", [](const httplib::Request &req, httplib::Response &res) {
        try {
            auto j = json::parse(req.body);
            if (!j.contains("symbol") || !j.contains("order_type") ||
                !j.contains("side") || !j.contains("quantity")) {
                res.status = 400;
                res.set_content("{\"error\": \"missing required fields\"}", "application/json");
                return;
            }

            std::string symbol = j["symbol"].get<std::string>();
            std::string order_type = j["order_type"].get<std::string>();
            std::string side = j["side"].get<std::string>();
            double quantity_d = j["quantity"].get<double>();
            
            if (quantity_d <= 0.0) {
                res.status = 400;
                res.set_content("{\"error\": \"quantity must be positive\"}", "application/json");
                return;
            }

            long long quantity = static_cast<long long>(quantity_d * 1000000.0);
            long long price = 0;
            
            if (order_type == "limit") {
                if (!j.contains("price")) {
                    res.status = 400;
                    res.set_content("{\"error\": \"limit order requires price\"}", "application/json");
                    return;
                }
                double price_d = j["price"].get<double>();
                if (price_d <= 0.0) {
                    res.status = 400;
                    res.set_content("{\"error\": \"price must be positive\"}", "application/json");
                    return;
                }
                price = static_cast<long long>(price_d * 100.0);
            }

            Order o;
            o.order_id = global_order_store.next_id();
            o.symbol = symbol;
            o.order_type = order_type;
            o.side = side;
            o.quantity = quantity;
            o.price = price;
            o.timestamp = std::chrono::system_clock::now();

            // Log order to WAL
            json order_json = {
                {"order_id", o.order_id},
                {"symbol", o.symbol},
                {"order_type", o.order_type},
                {"side", o.side},
                {"quantity", o.quantity},
                {"price", o.price},
                {"timestamp", to_iso8601(o.timestamp)}
            };
            global_wal.append_order(order_json);

            // Track order to symbol mapping
            {
                std::lock_guard<std::mutex> lk(order_to_symbol_mutex);
                order_to_symbol[o.order_id] = symbol;
            }

            // Match the order against per-symbol order book
            auto &book = order_books.try_emplace(symbol, symbol).first->second;
            auto trades = book.add_order(o);

            // Log trades to WAL and broadcast via WebSocket
            for (auto &t : trades) {
                json trade_json = {
                    {"trade_id", t.trade_id},
                    {"symbol", t.symbol},
                    {"price", t.price},
                    {"quantity", t.quantity},
                    {"aggressor_side", t.aggressor_side},
                    {"maker_order_id", t.maker_order_id},
                    {"taker_order_id", t.taker_order_id},
                    {"timestamp", t.timestamp_iso}
                };
                global_wal.append_trade(trade_json);
                
                // Broadcast trade to WebSocket clients
                if (global_ws_server && global_ws_server->is_running()) {
                    global_ws_server->broadcast_trade(t);
                }
            }

            // Broadcast order book update via WebSocket
            if (global_ws_server && global_ws_server->is_running()) {
                auto bids = book.top_bids(10);
                auto asks = book.top_asks(10);
                global_ws_server->broadcast_orderbook_update(symbol, bids, asks);
            }

            // Prepare JSON response
            json resp;
            resp["order"] = {
                {"order_id", o.order_id},
                {"symbol", o.symbol},
                {"order_type", o.order_type},
                {"side", o.side},
                {"quantity", o.quantity},
                {"price", o.price},
                {"timestamp", to_iso8601(o.timestamp)}
            };

            resp["trades"] = json::array();
            for (auto &t : trades) {
                resp["trades"].push_back({
                    {"trade_id", t.trade_id},
                    {"symbol", t.symbol},
                    {"price", t.price},
                    {"quantity", t.quantity},
                    {"aggressor_side", t.aggressor_side},
                    {"maker_order_id", t.maker_order_id},
                    {"taker_order_id", t.taker_order_id},
                    {"timestamp", t.timestamp_iso}
                });
            }

            res.status = 200;
            res.set_content(resp.dump(2), "application/json");

        } catch (const std::exception &e) {
            res.status = 400;
            json err = { {"error", std::string("invalid json payload: ") + e.what()} };
            res.set_content(err.dump(), "application/json");
        }
    });

    // Cancel order endpoint
    svr.Delete(R"(/orders/(.+))", [](const httplib::Request &req, httplib::Response &res) {
        try {
            std::string order_id = req.matches[1].str();
            
            // Find which symbol this order belongs to
            std::string symbol;
            {
                std::lock_guard<std::mutex> lk(order_to_symbol_mutex);
                auto it = order_to_symbol.find(order_id);
                if (it == order_to_symbol.end()) {
                    res.status = 404;
                    res.set_content("{\"error\":\"order not found\"}", "application/json");
                    return;
                }
                symbol = it->second;
            }
            
            // Cancel from order book
            auto book_it = order_books.find(symbol);
            if (book_it == order_books.end()) {
                res.status = 404;
                res.set_content("{\"error\":\"symbol not found\"}", "application/json");
                return;
            }
            
            bool cancelled = book_it->second.cancel_order(order_id);
            
            if (cancelled) {
                // Log cancellation to WAL
                global_wal.append_cancel(order_id, "user_request");
                
                // Remove from tracking
                {
                    std::lock_guard<std::mutex> lk(order_to_symbol_mutex);
                    order_to_symbol.erase(order_id);
                }
                
                // Broadcast order book update
                if (global_ws_server && global_ws_server->is_running()) {
                    auto bids = book_it->second.top_bids(10);
                    auto asks = book_it->second.top_asks(10);
                    global_ws_server->broadcast_orderbook_update(symbol, bids, asks);
                }
                
                json resp = {
                    {"cancelled", true},
                    {"order_id", order_id},
                    {"symbol", symbol}
                };
                res.set_content(resp.dump(2), "application/json");
            } else {
                res.status = 404;
                res.set_content("{\"error\":\"order not found or already filled\"}", "application/json");
            }
            
        } catch (const std::exception &e) {
            res.status = 500;
            json err = { {"error", std::string("internal error: ") + e.what()} };
            res.set_content(err.dump(), "application/json");
        }
    });

    // View current order book
    svr.Get(R"(/orderbook/(.+))", [](const httplib::Request &req, httplib::Response &res) {
        std::string symbol = req.matches[1].str();
        
        // Parse depth parameter (default 10)
        int depth = 10;
        if (req.has_param("depth")) {
            try {
                depth = std::stoi(req.get_param_value("depth"));
                if (depth < 1) depth = 1;
                if (depth > 100) depth = 100;
            } catch (...) {
                depth = 10;
            }
        }
        
        if (!order_books.count(symbol)) {
            res.status = 404;
            res.set_content("{\"error\":\"symbol not found\"}", "application/json");
            return;
        }

        auto &book = order_books.at(symbol);
        
        auto mk_levels = [](const std::vector<std::pair<long long,long long>> &lvls) {
            json arr = json::array();
            for (auto &p : lvls) {
                arr.push_back({{"price", p.first}, {"quantity", p.second}});
            }
            return arr;
        };

        json j;
        j["symbol"] = symbol;
        j["bids"] = mk_levels(book.top_bids(depth));
        j["asks"] = mk_levels(book.top_asks(depth));
        j["timestamp"] = to_iso8601(std::chrono::system_clock::now());
        
        res.set_content(j.dump(2), "application/json");
    });

    // Get recent trades for a symbol
    svr.Get(R"(/trades/(.+))", [](const httplib::Request &req, httplib::Response &res) {
        std::string symbol = req.matches[1].str();
        
        // Parse limit parameter (default 50)
        int limit = 50;
        if (req.has_param("limit")) {
            try {
                limit = std::stoi(req.get_param_value("limit"));
                if (limit < 1) limit = 1;
                if (limit > 1000) limit = 1000;
            } catch (...) {
                limit = 50;
            }
        }
        
        // Read recent trades from WAL
        // In production, maintain in-memory cache or separate trade store
        auto entries = global_wal.replay();
        
        json trades_array = json::array();
        int count = 0;
        
        // Iterate in reverse to get most recent first
        for (auto it = entries.rbegin(); it != entries.rend() && count < limit; ++it) {
            if (it->contains("type") && (*it)["type"] == "trade") {
                auto payload = (*it)["payload"];
                if (payload["symbol"] == symbol) {
                    trades_array.push_back(payload);
                    count++;
                }
            }
        }
        
        json response = {
            {"symbol", symbol},
            {"trades", trades_array},
            {"count", count}
        };
        
        res.set_content(response.dump(2), "application/json");
    });

    // Server stats endpoint
    svr.Get("/stats", [](const httplib::Request&, httplib::Response& res) {
        json stats = {
            {"symbols_count", order_books.size()},
            {"wal_total_entries", global_wal.total_entries()},
            {"wal_pending_writes", global_wal.pending_writes()},
            {"ws_active", global_ws_server != nullptr && global_ws_server->is_running()}
        };
        
        // Per-symbol stats
        json symbols = json::object();
        for (auto &[symbol, book] : order_books) {
            auto bids = book.top_bids(1);
            auto asks = book.top_asks(1);
            
            symbols[symbol] = {
                {"best_bid", bids.empty() ? nullptr : json(bids[0].first)},
                {"best_ask", asks.empty() ? nullptr : json(asks[0].first)},
                {"bid_depth", bids.empty() ? 0 : bids[0].second},
                {"ask_depth", asks.empty() ? 0 : asks[0].second}
            };
        }
        stats["symbols"] = symbols;
        
        res.set_content(stats.dump(2), "application/json");
    });

    std::cout << "[HTTP] Server listening on port " << port << "\n";
    svr.listen("0.0.0.0", port);
}