#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <iostream>
#include <thread>
#include <mutex>
#include <set>

#include "order_book.hpp"
#include "order.hpp"
#include "trade.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace json = boost::json;
using tcp = boost::asio::ip::tcp;

std::mutex global_mutex;
std::set<std::shared_ptr<websocket::stream<tcp::socket>>> clients;

// Global order book for AAPL
OrderBook order_book("AAPL");

void broadcast(const json::object &msg) {
    std::lock_guard<std::mutex> lock(global_mutex);
    std::string data = json::serialize(msg);

    for (auto &ws : clients) {
        try {
            ws->text(true);
            ws->write(net::buffer(data));
        } catch (...) {
            // ignore broken connections
        }
    }
}

void handle_message(const std::string &message, std::shared_ptr<websocket::stream<tcp::socket>> ws) {
    try {
        json::value parsed = json::parse(message);
        auto obj = parsed.as_object();

        std::string type = obj["type"].as_string().c_str();

        if (type == "new_order") {
            Order order;
            order.order_id = obj["order_id"].as_string().c_str();
            order.symbol = obj["symbol"].as_string().c_str();
            order.side = obj["side"].as_string().c_str();
            order.price = obj["price"].as_int64();
            order.quantity = obj["quantity"].as_int64();
            order.order_type = obj["order_type"].as_string().c_str();
            order.timestamp = obj["timestamp"].as_string().c_str();

            auto trades = order_book.match(order);

            json::object response;
            response["event"] = "order_executed";
            response["order"] = {
                {"order_id", order.order_id},
                {"symbol", order.symbol},
                {"side", order.side},
                {"price", order.price},
                {"quantity", order.quantity},
                {"remaining_quantity", order.quantity}
            };

            json::array trade_array;
            for (auto &t : trades) {
                trade_array.push_back({
                    {"trade_id", t.trade_id},
                    {"symbol", t.symbol},
                    {"price", t.price},
                    {"quantity", t.quantity},
                    {"maker_order_id", t.maker_order_id},
                    {"taker_order_id", t.taker_order_id},
                    {"aggressor_side", t.aggressor_side},
                    {"timestamp", t.timestamp}
                });
            }

            response["trades"] = trade_array;
            broadcast(response);

            // Also broadcast order book snapshot
            json::object snapshot;
            snapshot["event"] = "order_book_update";
            snapshot["symbol"] = order_book.symbol;
            json::array asks, bids;

            for (auto &[p, q] : order_book.get_asks()) {
                asks.push_back({{"price", p}, {"quantity", q}});
            }
            for (auto &[p, q] : order_book.get_bids()) {
                bids.push_back({{"price", p}, {"quantity", q}});
            }

            snapshot["asks"] = asks;
            snapshot["bids"] = bids;
            broadcast(snapshot);
        }

    } catch (const std::exception &e) {
        std::cerr << "Message handling error: " << e.what() << std::endl;
    }
}

void do_session(tcp::socket socket) {
    try {
        auto ws = std::make_shared<websocket::stream<tcp::socket>>(std::move(socket));
        ws->accept();

        {
            std::lock_guard<std::mutex> lock(global_mutex);
            clients.insert(ws);
        }

        for (;;) {
            beast::flat_buffer buffer;
            ws->read(buffer);
            std::string message = beast::buffers_to_string(buffer.data());
            handle_message(message, ws);
        }
    } catch (const std::exception &e) {
        std::cerr << "WebSocket session ended: " << e.what() << std::endl;
    }
}

int main() {
    try {
        net::io_context ioc{1};
        tcp::acceptor acceptor{ioc, {tcp::v4(), 9002}};

        std::cout << "[WebSocket] Server started on ws://localhost:9002\n";

        for (;;) {
            tcp::socket socket{ioc};
            acceptor.accept(socket);
            std::thread(&do_session, std::move(socket)).detach();
        }

    } catch (const std::exception &e) {
        std::cerr << "Server error: " << e.what() << std::endl;
    }
}
