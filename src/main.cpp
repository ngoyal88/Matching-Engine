// FILE: src/main.cpp
#include <iostream>
#include <thread>
#include <csignal>
#include <atomic>
#include <vector>
#include <map>
#include "../vendor/json.hpp"
#include "../include/order.h"
#include "../include/wal.h"
#include "../include/ws_server.h"
#include "../include/global_state.h"
#include "../include/order_book.h"
#include "../include/stop_order_manager.h"
//#include "../include/broadcast_queue.h" // <-- ADD THIS INCLUDE

// forward declarations
void setup_server(int port);
using json = nlohmann::json;

// Global flag for graceful shutdown
std::atomic<bool> shutdown_requested{false};

void signal_handler(int signal) {
    std::cout << "\n[Main] Shutdown signal received (" << signal << ")\n";
    shutdown_requested = true;
}

// (replay_wal function is unchanged from before)
void replay_wal() {
    auto entries = global_wal.replay();
    if (entries.empty()) {
        std::cout << "[Main] No WAL entries found (fresh start)\n";
        return;
    }
    std::cout << "[Main] Replaying " << entries.size() << " WAL entries...\n";
    std::map<std::string, Order> live_orders;
    std::map<std::string, StopOrder> live_stop_orders;
    for (const auto& j : entries) {
        try {
            std::string type = j["type"].get<std::string>();
            auto payload = j["payload"];
            if (type == "order") {
                Order o = Order::from_json(payload);
                live_orders[o.order_id] = o;
                g_total_orders.fetch_add(1, std::memory_order_relaxed);
            } 
            else if (type == "stop_order") {
                StopOrder so = StopOrder::from_json(payload);
                live_stop_orders[so.order_id] = so;
                g_total_orders.fetch_add(1, std::memory_order_relaxed);
            }
            else if (type == "trade") {
                std::string maker_id = payload["maker_order_id"].get<std::string>();
                std::string taker_id = payload["taker_order_id"].get<std::string>();
                long long qty = payload["quantity"].get<long long>();
                if (live_orders.count(maker_id)) {
                    live_orders[maker_id].quantity -= qty;
                    if (live_orders[maker_id].quantity <= 0) {
                        live_orders.erase(maker_id);
                    }
                }
                if (live_orders.count(taker_id)) {
                    live_orders[taker_id].quantity -= qty;
                    if (live_orders[taker_id].quantity <= 0) {
                        live_orders.erase(taker_id);
                    }
                }
                g_total_trades.fetch_add(1, std::memory_order_relaxed);
            }
            else if (type == "cancel") {
                std::string id = payload["order_id"].get<std::string>();
                live_orders.erase(id);
                live_stop_orders.erase(id);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Main] WAL replay error: " << e.what() << " on entry: " << j.dump() << std::endl;
        }
    }
    std::lock_guard<std::mutex> lk(g_global_mutex);
    for (const auto& [id, order] : live_orders) {
        g_order_books.try_emplace(order.symbol, order.symbol);
        g_order_books.at(order.symbol).add_order_from_replay(order); 
        g_order_id_to_symbol[id] = order.symbol;
    }
    for (const auto& [id, order] : live_stop_orders) {
        g_stop_order_managers.try_emplace(order.symbol, order.symbol);
        g_stop_order_managers.at(order.symbol).add_stop_order_from_replay(order); 
        g_order_id_to_symbol[id] = order.symbol;
    }
    std::cout << "[Main] WAL replay complete. " 
              << g_order_books.size() << " symbol(s) loaded." << std::endl;
    std::cout << "[Main] Total Orders: " << g_total_orders.load() 
              << ", Total Trades: " << g_total_trades.load() << std::endl;
}


int main(int argc, char** argv) {
    int http_port = 8080;
    int ws_port = 9002;
    
    if (argc > 1) http_port = std::stoi(argv[1]);
    if (argc > 2) ws_port = std::stoi(argv[2]);

    std::cout << "========================================\n";
    std::cout << "  Matching Engine - C++ Implementation  \n";
    std::cout << "========================================\n";
    std::cout << "HTTP API Port: " << http_port << "\n";
    std::cout << "WebSocket Port: " << ws_port << "\n";
    std::cout << "========================================\n\n";

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        replay_wal();
    } catch (const std::exception &e) {
        std::cerr << "[Main] CRITICAL: WAL replay failed: " << e.what() << "\n";
        return 1;
    }

    std::cout << "[Main] Initializing WebSocket server...\n";
    g_ws_server = new WebSocketServer(ws_port);
    
    std::thread ws_thread([&]() {
        try {
            g_ws_server->start();
        } catch (const std::exception &e) {
            std::cerr << "[Main] WebSocket server error: " << e.what() << "\n";
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "\n[Main] Starting HTTP server...\n";
    
    std::thread http_thread([&]() {
        setup_server(http_port);
    });

    std::cout << "\n========================================\n";
    std::cout << "  Server Ready!                         \n";
    std::cout << "========================================\n";
    std::cout << "HTTP API:    http://localhost:" << http_port << "\n";
    std::cout << "WebSocket:   ws://localhost:" << ws_port << "\n";
    std::cout << "Health:      http://localhost:" << http_port << "/health\n";
    std::cout << "Stats:       http://localhost:" << http_port << "/stats\n";
    std::cout << "========================================\n";
    std::cout << "Press Ctrl+C to shutdown\n\n";

    while (!shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // --- UPDATED SHUTDOWN LOGIC ---
    std::cout << "\n[Main] Shutting down gracefully...\n";
    
    if (g_ws_server) {
        std::cout << "[Main] Stopping WebSocket server...\n";
        g_ws_server->stop();
        delete g_ws_server;
        g_ws_server = nullptr;
    }
    
    std::cout << "[Main] Stopping WAL writer thread...\n";
    global_wal.stop(); // Stop async WAL
    
    std::cout << "[Main] Stopping Broadcast queue thread...\n";
    g_broadcast_queue.stop(); // <-- ADD THIS LINE
    
    std::cout << "[Main] Waiting for threads to finish...\n";
    if (ws_thread.joinable()) {
        ws_thread.join();
    }
    
    if (http_thread.joinable()) {
         std::cout << "[Main] HTTP thread joined." << std::endl;
    }
    
    std::cout << "[Main] Shutdown complete\n";
    
    std::exit(0);
}

