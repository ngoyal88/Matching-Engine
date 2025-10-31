// ============================================================================
// FILE: src/server.cpp (UPDATED with better error handling)
// ============================================================================
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

static std::unordered_map<std::string, OrderBook> order_books;
static std::unordered_map<std::string, std::string> order_to_symbol;
static std::mutex order_to_symbol_mutex;
static std::atomic<uint64_t> total_orders{0};
static std::atomic<uint64_t> total_trades{0};

void setup_server(int port) {
    httplib::Server svr;

    // Enable CORS for web clients
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"}
    });

    // Health check
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res){
        json health = {
            {"status", "healthy"},
            {"uptime_seconds", std::chrono::steady_clock::now().time_since_epoch().count() / 1000000000},
            {"ws_clients", global_ws_server ? global_ws_server->client_count() : 0}
        };
        res.set_content(health.dump(2), "application/json");
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

    // Create order with validation
    svr.Post("/orders", [](const httplib::Request &req, httplib::Response &res) {
        try {
            auto j = json::parse(req.body);
            
            // Validate required fields
            std::vector<std::string> required = {"symbol", "order_type", "side", "quantity"};
            for (const auto &field : required) {
                if (!j.contains(field)) {
                    res.status = 400;
                    json err = {{"error", "missing field: " + field}};
                    res.set_content(err.dump(), "application/json");
                    return;
                }
            }

            std::string symbol = j["symbol"].get<std::string>();
            std::string order_type = j["order_type"].get<std::string>();
            std::string side = j["side"].get<std::string>();
            
            // Validate order type
            if (order_type != "market" && order_type != "limit" && 
                order_type != "ioc" && order_type != "fok") {
                res.status = 400;
                json err = {{"error", "invalid order_type. Use: market, limit, ioc, fok"}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            
            // Validate side
            if (side != "buy" && side != "sell") {
                res.status = 400;
                json err = {{"error", "invalid side. Use: buy or sell"}};
                res.set_content(err.dump(), "application/json");
                return;
            }

            double quantity_d = j["quantity"].get<double>();
            if (quantity_d <= 0.0) {
                res.status = 400;
                json err = {{"error", "quantity must be positive"}};
                res.set_content(err.dump(), "application/json");
                return;
            }

            long long quantity = static_cast<long long>(quantity_d * 1000000.0);
            long long price = 0;
            
            if (order_type == "limit" || order_type == "ioc" || order_type == "fok") {
                if (!j.contains("price")) {
                    res.status = 400;
                    json err = {{"error", order_type + " order requires price"}};
                    res.set_content(err.dump(), "application/json");
                    return;
                }
                double price_d = j["price"].get<double>();
                if (price_d <= 0.0) {
                    res.status = 400;
                    json err = {{"error", "price must be positive"}};
                    res.set_content(err.dump(), "application/json");
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

            // Track order
            {
                std::lock_guard<std::mutex> lk(order_to_symbol_mutex);
                order_to_symbol[o.order_id] = symbol;
            }
            total_orders.fetch_add(1);

            // Match order
            auto &book = order_books.try_emplace(symbol, symbol).first->second;
            auto trades = book.add_order(o);
            total_trades.fetch_add(trades.size());

            // Log trades
            for (auto &t : trades) {
                json trade_json = {
                    {"trade_id", t.trade_id},
                    {"symbol", t.symbol},
                    {"price", t.price},
                    {"quantity", t.quantity},
                    {"aggressor_side", t.aggressor_side},
                    {"maker_order_id", t.maker_order_id},
                    {"taker_order_id", t.taker_order_id},
                    {"maker_fee", t.maker_fee},
                    {"taker_fee", t.taker_fee},
                    {"timestamp", t.timestamp_iso}
                };
                global_wal.append_trade(trade_json);
                
                if (global_ws_server && global_ws_server->is_running()) {
                    global_ws_server->broadcast_trade(t);
                }
            }
                
            //Broadcast orderbook update
            if (global_ws_server && global_ws_server->is_running()) {
                auto bids = book.top_bids(10);
                auto asks = book.top_asks(10);
                global_ws_server->broadcast_orderbook_update(symbol, bids, asks);
            }
            

            // Build response
            json resp;
            resp["trades"] = json::array();
            long long filled_qty = 0;
            for (auto &t : trades) {
                filled_qty += t.quantity;
                resp["trades"].push_back({
                    {"trade_id", t.trade_id},
                    {"symbol", t.symbol},
                    {"price", t.price},
                    {"quantity", t.quantity},
                    {"aggressor_side", t.aggressor_side},
                    {"maker_order_id", t.maker_order_id},
                    {"taker_order_id", t.taker_order_id},
                    {"maker_fee", t.maker_fee},
                    {"taker_fee", t.taker_fee},
                    {"timestamp", t.timestamp_iso}
                });
            }
            long long remaining_qty = std::max(0LL, quantity - filled_qty);

            // Determine order status
            std::string status;
            if (order_type == "fok") {
                status = (filled_qty == quantity) ? "filled" : "cancelled";
            } else if (order_type == "ioc") {
                status = (filled_qty > 0) ? "partially_filled" : "cancelled";
            } else if (order_type == "market") {
                if (filled_qty == 0) status = "cancelled"; // no liquidity
                else if (filled_qty < quantity) status = "partially_filled";
                else status = "filled";
            } else { // limit
                if (remaining_qty == 0) status = "filled"; // immediate full fill
                else if (filled_qty > 0) status = "partially_filled"; // resting remainder
                else status = "open";
            }

            resp["order"] = {
                {"order_id", o.order_id},
                {"symbol", o.symbol},
                {"order_type", o.order_type},
                {"side", o.side},
                {"quantity", o.quantity},
                {"price", o.price},
                {"timestamp", to_iso8601(o.timestamp)},
                {"status", status}
            };

            resp["filled_quantity"] = filled_qty;
            resp["remaining_quantity"] = remaining_qty;

            res.status = 200;
            res.set_content(resp.dump(2), "application/json");

        } catch (const json::parse_error &e) {
            res.status = 400;
            json err = {{"error", "invalid json: " + std::string(e.what())}};
            res.set_content(err.dump(), "application/json");
        } catch (const std::exception &e) {
            res.status = 500;
            json err = {{"error", "internal error: " + std::string(e.what())}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // Cancel order
    svr.Delete(R"(/orders/(.+))", [](const httplib::Request &req, httplib::Response &res) {
        try {
            std::string order_id = req.matches[1].str();
            
            std::string symbol;
            {
                std::lock_guard<std::mutex> lk(order_to_symbol_mutex);
                auto it = order_to_symbol.find(order_id);
                if (it == order_to_symbol.end()) {
                    res.status = 404;
                    json err = {{"error", "order not found"}};
                    res.set_content(err.dump(), "application/json");
                    return;
                }
                symbol = it->second;
            }
            
            auto book_it = order_books.find(symbol);
            if (book_it == order_books.end()) {
                res.status = 404;
                json err = {{"error", "symbol not found"}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            
            bool cancelled = book_it->second.cancel_order(order_id);
            
            if (cancelled) {
                global_wal.append_cancel(order_id, "user_request");
                
                {
                    std::lock_guard<std::mutex> lk(order_to_symbol_mutex);
                    order_to_symbol.erase(order_id);
                }
                
                if (global_ws_server && global_ws_server->is_running()) {
                    auto bids = book_it->second.top_bids(10);
                    auto asks = book_it->second.top_asks(10);
                    global_ws_server->broadcast_orderbook_update(symbol, bids, asks);
                }
                
                json resp = {
                    {"cancelled", true},
                    {"order_id", order_id},
                    {"symbol", symbol},
                    {"timestamp", to_iso8601(std::chrono::system_clock::now())}
                };
                res.set_content(resp.dump(2), "application/json");
            } else {
                res.status = 404;
                json err = {{"error", "order not found or already filled"}};
                res.set_content(err.dump(), "application/json");
            }
            
        } catch (const std::exception &e) {
            res.status = 500;
            json err = {{"error", "internal error: " + std::string(e.what())}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // View orderbook with depth parameter
    svr.Get(R"(/orderbook/(.+))", [](const httplib::Request &req, httplib::Response &res) {
        std::string symbol = req.matches[1].str();
        
        int depth = 10;
        if (req.has_param("depth")) {
            try {
                depth = std::stoi(req.get_param_value("depth"));
                depth = std::max(1, std::min(depth, 100));
            } catch (...) {
                depth = 10;
            }
        }
        
        if (!order_books.count(symbol)) {
            res.status = 404;
            json err = {{"error", "symbol not found"}};
            res.set_content(err.dump(), "application/json");
            return;
        }

        auto &book = order_books.at(symbol);
        
        auto mk_levels = [](const std::vector<std::pair<long long,long long>> &lvls) {
            json arr = json::array();
            for (auto &p : lvls) {
                arr.push_back({
                    {"price", p.first / 100.0},
                    {"quantity", p.second / 1000000.0}
                });
            }
            return arr;
        };

        auto bids = book.top_bids(depth);
        auto asks = book.top_asks(depth);

        json j;
        j["symbol"] = symbol;
        j["bids"] = mk_levels(bids);
        j["asks"] = mk_levels(asks);
        j["best_bid"] = bids.empty() ? json(nullptr) : json(bids[0].first / 100.0);
        j["best_ask"] = asks.empty() ? json(nullptr) : json(asks[0].first / 100.0);
        if (!bids.empty() && !asks.empty()) {
            j["spread"] = (asks[0].first - bids[0].first) / 100.0;
        } else {
            j["spread"] = nullptr;
        }
        j["timestamp"] = to_iso8601(std::chrono::system_clock::now());
        
        res.set_content(j.dump(2), "application/json");
    });

    // Get recent trades
    svr.Get(R"(/trades/(.+))", [](const httplib::Request &req, httplib::Response &res) {
        std::string symbol = req.matches[1].str();
        
        int limit = 50;
        if (req.has_param("limit")) {
            try {
                limit = std::stoi(req.get_param_value("limit"));
                limit = std::max(1, std::min(limit, 1000));
            } catch (...) {
                limit = 50;
            }
        }
        
        auto entries = global_wal.replay();
        
        json trades_array = json::array();
        int count = 0;
        
        for (auto it = entries.rbegin(); it != entries.rend() && count < limit; ++it) {
            if (it->contains("type") && (*it)["type"] == "trade") {
                auto payload = (*it)["payload"];
                if (payload["symbol"] == symbol) {
                    json trade_display = payload;
                    trade_display["price"] = payload["price"].get<long long>() / 100.0;
                    trade_display["quantity"] = payload["quantity"].get<long long>() / 1000000.0;
                    trade_display["maker_fee"] = payload["maker_fee"].get<long long>() / 1000000.0;
                    trade_display["taker_fee"] = payload["taker_fee"].get<long long>() / 1000000.0;
                    trades_array.push_back(trade_display);
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

    // Server statistics
    svr.Get("/stats", [](const httplib::Request&, httplib::Response& res) {
        json stats = {
            {"symbols_count", order_books.size()},
            {"total_orders", total_orders.load()},
            {"total_trades", total_trades.load()},
            {"wal_total_entries", global_wal.total_entries()},
            {"wal_pending_writes", global_wal.pending_writes()},
            {"ws_active", global_ws_server != nullptr && global_ws_server->is_running()},
            {"ws_clients", global_ws_server ? global_ws_server->client_count() : 0}
        };
        
        json symbols = json::object();
        for (auto &[symbol, book] : order_books) {
            auto bids = book.top_bids(1);
            auto asks = book.top_asks(1);
            
            json entry;
            entry["best_bid"] = bids.empty() ? json(nullptr) : json(bids[0].first / 100.0);
            entry["best_ask"] = asks.empty() ? json(nullptr) : json(asks[0].first / 100.0);
            entry["bid_depth"] = bids.empty() ? 0 : bids[0].second / 1000000.0;
            entry["ask_depth"] = asks.empty() ? 0 : asks[0].second / 1000000.0;
            if (!bids.empty() && !asks.empty()) {
                entry["spread"] = (asks[0].first - bids[0].first) / 100.0;
            } else {
                entry["spread"] = nullptr;
            }
            symbols[symbol] = entry;
        }
        stats["symbols"] = symbols;
        
        res.set_content(stats.dump(2), "application/json");
    });

    // Batch order submission
    svr.Post("/orders/batch", [](const httplib::Request &req, httplib::Response &res) {
        try {
            auto j = json::parse(req.body);
            
            if (!j.contains("orders") || !j["orders"].is_array()) {
                res.status = 400;
                json err = {{"error", "request must contain 'orders' array"}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            
            json results = json::array();
            
            for (auto &order_json : j["orders"]) {
                // Reuse single order logic
                // This is a simplified version - in production, implement proper batch processing
                results.push_back({
                    {"status", "pending"},
                    {"message", "batch processing not fully implemented"}
                });
            }
            
            json response = {
                {"processed", results.size()},
                {"results", results}
            };
            
            res.set_content(response.dump(2), "application/json");
            
        } catch (const std::exception &e) {
            res.status = 500;
            json err = {{"error", std::string(e.what())}};
            res.set_content(err.dump(), "application/json");
        }
    });

    std::cout << "[HTTP] Server listening on port " << port << "\n";
    svr.listen("0.0.0.0", port);
}
