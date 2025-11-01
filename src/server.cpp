// ============================================================================
// FILE: src/server.cpp (FINAL FIX: Async Broadcast Queue)
// ============================================================================
#include <httplib.h>
#include "../vendor/json.hpp"
#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread> // Keep this include for std::this_thread
#include "../include/order.h"
#include "../include/global_state.h"
#include "../include/wal.h"
#include "../include/broadcast_queue.h" // <-- ADD THIS INCLUDE

using json = nlohmann::json;

// (to_iso8601 helper function is unchanged)
static std::string to_iso8601(const std::chrono::system_clock::time_point &tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()) % 1000000000;
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(9) << ns.count() << 'Z';
    return ss.str();
}

void setup_server(int port) {
    httplib::Server svr;

    // CORS preflight handler
    svr.Options("/(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, GET, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    // Helper to add CORS headers
    auto add_cors = [](httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
    };

    // Health check
    svr.Get("/health", [&](const httplib::Request&, httplib::Response& res){
        add_cors(res);
        json health = {
            {"status", "healthy"},
            {"uptime_seconds", std::chrono::steady_clock::now().time_since_epoch().count() / 1000000000},
            {"ws_clients", g_ws_server ? g_ws_server->client_count() : 0}
        };
        res.set_content(health.dump(), "application/json");
    });

    // Get symbols
    svr.Get("/symbols", [&](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        json symbols = json::array();
        std::lock_guard<std::mutex> lk(g_global_mutex);
        for (const auto &[symbol, book] : g_order_books) {
            symbols.push_back(symbol);
        }
        json response = { {"symbols", symbols}, {"count", symbols.size()} };
        res.set_content(response.dump(), "application/json");
    });

    // Create Order (Main)
    svr.Post("/orders", [&](const httplib::Request &req, httplib::Response &res) {
        add_cors(res);
        try {
            auto j = json::parse(req.body);
            
            // --- 1. Validation (Fast) ---
            // (Validation code is unchanged)
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
            if (order_type != "market" && order_type != "limit" && 
                order_type != "ioc" && order_type != "fok") {
                res.status = 400;
                json err = {{"error", "invalid order_type. Use: market, limit, ioc, fok"}};
                res.set_content(err.dump(), "application/json");
                return;
            }
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
            // --- End Validation ---

            // --- 2. Order Creation & WAL (Fast) ---
            Order o;
            o.order_id = "ORD-" + std::to_string(g_total_orders.fetch_add(1) + 1);
            o.symbol = symbol;
            o.order_type = order_type;
            o.side = side;
            o.quantity = quantity;
            o.price = price;
            o.timestamp = std::chrono::system_clock::now();
            json order_json = {
                {"order_id", o.order_id}, {"symbol", o.symbol}, {"order_type", o.order_type},
                {"side", o.side}, {"quantity", o.quantity}, {"price", o.price},
                {"timestamp", to_iso8601(o.timestamp)}
            };
            global_wal.append_order(order_json); // Async push

            
            // --- 3. Fine-Grained Lock to get Book (Fast) ---
            OrderBook* book_ptr;
            {
                std::lock_guard<std::mutex> lk(g_global_mutex);
                book_ptr = &g_order_books.try_emplace(symbol, symbol).first->second;
                g_order_id_to_symbol[o.order_id] = symbol;
            }

            // --- 4. Matching (Fast, uses internal lock) ---
            auto trades = book_ptr->add_order(o);
            g_total_trades.fetch_add(trades.size());

            // --- 5. Post-Trade Logic & Response Prep (Fast) ---
            long long filled_qty = 0;
            json trades_array = json::array();
            for (auto &t : trades) {
                filled_qty += t.quantity;
                json trade_json = {
                    {"trade_id", t.trade_id}, {"symbol", t.symbol}, {"price", t.price},
                    {"quantity", t.quantity}, {"aggressor_side", t.aggressor_side},
                    {"maker_order_id", t.maker_order_id}, {"taker_order_id", t.taker_order_id},
                    {"maker_fee", t.maker_fee}, {"taker_fee", t.taker_fee},
                    {"timestamp", t.timestamp_iso}
                };
                global_wal.append_trade(trade_json); // Async push
                trades_array.push_back(trade_json);
            }
            
            // --- 6. ASYNCHRONOUS BROADCAST (THE REAL FIX) ---
            if (g_ws_server && g_ws_server->is_running() && (trades.size() > 0)) {
                // Get data (fast, uses book's internal lock)
                auto bids_copy = book_ptr->top_bids(10);
                auto asks_copy = book_ptr->top_asks(10);
                
                // --- REMOVED: std::thread().detach() ---
                
                // --- ADDED: Fast, non-blocking queue push ---
                for (const auto& tj : trades_array) {
                    g_broadcast_queue.push_trade(tj);
                }
                g_broadcast_queue.push_book_update(symbol, bids_copy, asks_copy);
                // --- HTTP thread is now free ---
            }

            // --- 7. Build Response (Fast) ---
            // (This code is unchanged)
            long long remaining_qty = std::max(0LL, quantity - filled_qty);
            std::string status;
            if (order_type == "fok") {
                status = (filled_qty == quantity) ? "filled" : "cancelled";
            } else if (order_type == "ioc") {
                status = (filled_qty == 0 && remaining_qty > 0) ? "cancelled" : (remaining_qty == 0 ? "filled" : "partially_filled");
            } else if (order_type == "market") {
                if (filled_qty == 0) status = "cancelled";
                else if (remaining_qty > 0) status = "partially_filled";
                else status = "filled";
            } else { // limit
                if (remaining_qty == 0) status = "filled";
                else if (filled_qty > 0) status = "partially_filled";
                else status = "open";
            }
            json resp;
            resp["order"] = {
                {"order_id", o.order_id}, {"symbol", o.symbol}, {"order_type", o.order_type},
                {"side", o.side}, {"quantity", o.quantity}, {"price", o.price},
                {"timestamp", to_iso8601(o.timestamp)}, {"status", status}
            };
            resp["trades"] = trades_array;
            resp["filled_quantity"] = filled_qty;
            resp["remaining_quantity"] = remaining_qty;

            res.status = 200;
            res.set_content(resp.dump(), "application/json");

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
    
    // Create Stop Order
    svr.Post("/orders/stop", [&](const httplib::Request &req, httplib::Response &res) {
        add_cors(res);
        try {
            auto j = json::parse(req.body);
            std::vector<std::string> required = {"symbol", "stop_type", "side", "quantity", "trigger_price"};
            for (const auto &field : required) {
                if (!j.contains(field)) {
                    res.status = 400;
                    json err = {{"error", "missing field: " + field}};
                    res.set_content(err.dump(), "application/json");
                    return;
                }
            }
            std::string symbol = j["symbol"].get<std::string>();
            std::string stop_type_str = j["stop_type"].get<std::string>();
            std::string side = j["side"].get<std::string>();
            StopOrder so;
            so.symbol = symbol;
            so.side = side;
            so.quantity = static_cast<long long>(j["quantity"].get<double>() * 1000000.0);
            so.trigger_price = static_cast<long long>(j["trigger_price"].get<double>() * 100.0);
            so.limit_price = 0;
            if (stop_type_str == "stop_limit") {
                if (!j.contains("limit_price")) {
                    res.status = 400;
                    json err = {{"error", "stop_limit requires limit_price"}};
                    res.set_content(err.dump(), "application/json");
                    return;
                }
                so.limit_price = static_cast<long long>(j["limit_price"].get<double>() * 100.0);
                so.stop_type = StopOrderType::STOP_LIMIT;
            } else {
                so.stop_type = StopOrderType::STOP_LOSS;
            }
            so.order_id = "STO-" + std::to_string(g_total_orders.fetch_add(1) + 1);
            so.created_at = std::chrono::system_clock::now();
            so.best_price = (side == "buy") ? 999999999999LL : 0;
            json order_json = {
                {"order_id", so.order_id}, {"symbol", so.symbol}, {"order_type", "stop"},
                {"stop_type", stop_type_str}, {"side", so.side}, {"quantity", so.quantity},
                {"trigger_price", so.trigger_price}, {"limit_price", so.limit_price},
                {"timestamp", to_iso8601(so.created_at)}
            };
            global_wal.append_order(order_json);
            StopOrderManager* manager_ptr;
            {
                std::lock_guard<std::mutex> lk(g_global_mutex);
                manager_ptr = &g_stop_order_managers.try_emplace(symbol, symbol).first->second;
                g_order_id_to_symbol[so.order_id] = symbol;
            }
            manager_ptr->add_stop_order(so);
            json resp = {
                {"status", "accepted"},
                {"stop_order_id", so.order_id},
                {"order", order_json}
            };
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception &e) {
            res.status = 500;
            json err = {{"error", "internal error: " + std::string(e.what())}};
            res.set_content(err.dump(), "application/json");
        }
    });


    // --- Cancel order ---
    svr.Delete(R"(/orders/(.+))", [&](const httplib::Request &req, httplib::Response &res) {
        add_cors(res);
        try {
            std::string order_id = req.matches[1].str();
            std::string symbol;
            {
                std::lock_guard<std::mutex> lk(g_global_mutex);
                auto it = g_order_id_to_symbol.find(order_id);
                if (it == g_order_id_to_symbol.end()) {
                    res.status = 404;
                    json err = {{"error", "order not found or already executed"}};
                    res.set_content(err.dump(), "application/json");
                    return;
                }
                symbol = it->second;
            }

            bool cancelled_book = false;
            bool cancelled_stop = false;
            OrderBook* book_ptr = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_global_mutex);
                if (g_order_books.count(symbol)) {
                    book_ptr = &g_order_books.at(symbol);
                    cancelled_book = book_ptr->cancel_order(order_id);
                }
                if (g_stop_order_managers.count(symbol)) {
                    cancelled_stop = g_stop_order_managers.at(symbol).cancel_stop_order(order_id);
                }
            }

            bool cancelled = cancelled_book || cancelled_stop;

            if (cancelled) {
                global_wal.append_cancel(order_id, "user_request");
                {
                    std::lock_guard<std::mutex> lk(g_global_mutex);
                    g_order_id_to_symbol.erase(order_id);
                }
                
                // --- 3. ASYNCHRONOUS BROADCAST (THE REAL FIX) ---
                if (g_ws_server && g_ws_server->is_running() && book_ptr) {
                    auto bids_copy = book_ptr->top_bids(10);
                    auto asks_copy = book_ptr->top_asks(10);
                    
                    // --- REMOVED: std::thread().detach() ---

                    // --- ADDED: Fast, non-blocking queue push ---
                    g_broadcast_queue.push_book_update(symbol, bids_copy, asks_copy);
                }
                
                json resp = {
                    {"cancelled", true}, {"order_id", order_id}, {"symbol", symbol},
                    {"timestamp", to_iso8601(std::chrono::system_clock::now())}
                };
                res.set_content(resp.dump(), "application/json");
            } else {
                res.status = 404;
                json err = {{"error", "order not found or already filled/cancelled"}};
                res.set_content(err.dump(), "application/json");
            }
            
        } catch (const std::exception &e) {
            res.status = 500;
            json err = {{"error", "internal error: " + std::string(e.what())}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // --- View orderbook ---
    svr.Get(R"(/orderbook/(.+))", [&](const httplib::Request &req, httplib::Response &res) {
        add_cors(res);
        std::string symbol = req.matches[1].str();
        int depth = 10;
        OrderBook* book_ptr;
        {
            std::lock_guard<std::mutex> lk(g_global_mutex);
            if (!g_order_books.count(symbol)) {
                res.status = 404;
                json err = {{"error", "symbol not found"}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            book_ptr = &g_order_books.at(symbol);
        }
        auto bids = book_ptr->top_bids(depth);
        auto asks = book_ptr->top_asks(depth);
        auto mk_levels = [](const std::vector<std::pair<long long,long long>> &lvls) {
            json arr = json::array();
            for (auto &p : lvls) {
                arr.push_back({
                    {"price", p.first / 100.0},
                    {"quantity", p.second / 1000000.0},
                    {"total", (p.first / 100.0) * (p.second / 1000000.0)}
                });
            }
            return arr;
        };
        json j;
        j["symbol"] = symbol;
        j["bids"] = mk_levels(bids);
        j["asks"] = mk_levels(asks);
        j["timestamp"] = to_iso8601(std::chrono::system_clock::now());
        res.set_content(j.dump(), "application/json");
    });

    // --- Get recent trades ---
    svr.Get(R"(/trades/(.+))", [&](const httplib::Request &req, httplib::Response &res) {
        add_cors(res);
        std::string symbol = req.matches[1].str();
        int limit = 50;
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
        res.set_content(response.dump(), "application/json");
    });

    // --- Server statistics ---
    svr.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        json stats;
        stats["total_orders"] = g_total_orders.load();
        stats["total_trades"] = g_total_trades.load();
        stats["ws_clients"] = g_ws_server ? g_ws_server->client_count() : 0;
        json symbols = json::object();
        {
            std::lock_guard<std::mutex> lk(g_global_mutex);
            stats["symbols_count"] = g_order_books.size();
            for (auto &[symbol, book] : g_order_books) {
                auto bids = book.top_bids(1);
                auto asks = book.top_asks(1);
                json entry;
                entry["best_bid"] = bids.empty() ? json(nullptr) : json(bids[0].first / 100.0);
                entry["best_ask"] = asks.empty() ? json(nullptr) : json(asks[0].first / 100.0);
                symbols[symbol] = entry;
            }
        }
        stats["symbols"] = symbols;
        res.set_content(stats.dump(), "application/json");
    });

    std::cout << "[HTTP] Server listening on port " << port << "\n";
    svr.listen("0.0.0.0", port);
}

