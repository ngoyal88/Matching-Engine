// FILE: src/server.cpp
#include "../include/order.h"
#include "../include/order_store.h"
#include "../include/order_book.h"
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

void setup_server(int port) {
    httplib::Server svr;

    // Health check
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res){
        res.set_content("OK", "text/plain");
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

    // Store order in WAL
        global_order_store.add_order(o);

    // Match the order against per-symbol order book (create if missing)
    auto &book = order_books.try_emplace(symbol, symbol).first->second;
    auto trades = book.add_order(o);

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

    // View current order book
    svr.Get(R"(/orderbook/(.+))", [](const httplib::Request &req, httplib::Response &res) {
        std::string symbol = req.matches[1].str();
        if (!order_books.count(symbol)) {
            res.status = 404;
            res.set_content("{\"error\":\"symbol not found\"}", "application/json");
            return;
        }

    auto &book = order_books.at(symbol);
        // Return top levels (default 10)
        auto mk_levels = [](const std::vector<std::pair<long long,long long>> &lvls) {
            json arr = json::array();
            for (auto &p : lvls) {
                // Represent each level as {"price": p.first, "quantity": p.second}
                arr.push_back({{"price", p.first}, {"quantity", p.second}});
            }
            return arr;
        };

        json j;
        j["symbol"] = symbol;
        j["bids"] = mk_levels(book.top_bids(10));
        j["asks"] = mk_levels(book.top_asks(10));
        res.set_content(j.dump(2), "application/json");
    });

    std::cout << "HTTP server listening on port " << port << "\n";
    svr.listen("0.0.0.0", port);
}
