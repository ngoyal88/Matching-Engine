// FILE: src/main.cpp (UPDATED with WAL Replay)
#include <iostream>
#include <thread>
#include <csignal>
#include <atomic>
#include "../include/order.h"
#include "../include/wal.h"
#include "../include/ws_server.h"
#include "../include/global_state.h" // <-- ADDED
#include <map>                       // <-- ADDED

// forward declarations
void setup_server(int port);
class OrderStore;
extern OrderStore global_order_store;

// Global flag for graceful shutdown
std::atomic<bool> shutdown_requested{false};

void signal_handler(int signal) {
    std::cout << "\n[Main] Shutdown signal received (" << signal << ")\n";
    shutdown_requested = true;
}

/**
 * @brief Reconstructs the engine's state from WAL entries.
 */
void replay_wal() {
    std::cout << "[WAL] Starting WAL replay...\n";
    auto entries = global_wal.replay();
    
    if (entries.empty()) {
        std::cout << "[WAL] No entries found (fresh start).\n";
        return;
    }

    std::cout << "[WAL] Replaying " << entries.size() << " entries...\n";
    
    int order_count = 0;
    int cancel_count = 0;
    int trade_count = 0;
    int unknown_count = 0;

    for (const auto& entry : entries) {
        try {
            std::string type = entry.at("type").get<std::string>();
            auto payload = entry.at("payload");
            
            if (type == "order") {
                std::string symbol = payload.at("symbol").get<std::string>();
                
                // Ensure book and stop manager exist for this symbol
                g_order_books.try_emplace(symbol, symbol);
                g_stop_order_managers.try_emplace(symbol, symbol);

                std::string order_type = payload.value("order_type", "");
                
                if (order_type == "stop") {
                    // This is a stop order
                    StopOrder stop_order = StopOrder::from_json(payload);
                    g_stop_order_managers.at(symbol).add_stop_order(stop_order);
                    g_order_to_symbol[stop_order.order_id] = symbol;
                } else {
                    // This is a regular limit/market/ioc/fok order
                    Order order = Order::from_json(payload);
                    // Replay the order by adding it to the book.
                    // We ignore the returned trades, as we only care about state.
                    g_order_books.at(symbol).add_order(order);
                    // Track the order ID
                    g_order_to_symbol[order.order_id] = symbol;
                }
                order_count++;

            } else if (type == "cancel") {
                std::string order_id = payload.at("order_id").get<std::string>();
                
                auto it = g_order_to_symbol.find(order_id);
                if (it != g_order_to_symbol.end()) {
                    std::string symbol = it->second;
                    
                    // Try to cancel from both books
                    g_order_books.at(symbol).cancel_order(order_id);
                    g_stop_order_managers.at(symbol).cancel_stop_order(order_id);
                    
                    g_order_to_symbol.erase(it); // Remove from tracking
                }
                cancel_count++;

            } else if (type == "trade") {
                // Trades are a result, not state.
                // Replaying orders will rebuild the book state.
                // We just count them for info.
                trade_count++;
            }
        } catch (const std::exception& e) {
            std::cerr << "[WAL] Error replaying entry: " << e.what() << "\n" << entry.dump(2) << std::endl;
            unknown_count++;
        }
    }

    std::cout << "[WAL] Replay complete. Orders: " << order_count
              << ", Cancels: " << cancel_count
              << ", (Ignored) Trades: " << trade_count
              << ", Errors: " << unknown_count << "\n";
    
    std::cout << "[WAL] Current State:\n";
    for (auto& [symbol, book] : g_order_books) {
        std::cout << "  - Symbol " << symbol 
                  << ": Bids=" << book.top_bids(1).size() 
                  << ", Asks=" << book.top_asks(1).size() << "\n";
    }
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

    // Setup signal handlers for graceful shutdown
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // --- UPDATED: Replay WAL *before* starting servers ---
    try {
        replay_wal();
    } catch (const std::exception &e) {
        std::cerr << "[Main] CRITICAL: WAL replay failed: " << e.what() << "\n";
        return 1;
    }
    // --- END UPDATED ---

    // Initialize WebSocket server
    std::cout << "[Main] Initializing WebSocket server...\n";
    global_ws_server = new WebSocketServer(ws_port);
    
    // Start WebSocket server in separate thread
    std::thread ws_thread([&]() {
        try {
            global_ws_server->start();
        } catch (const std::exception &e) {
            std::cerr << "[Main] WebSocket server error: " << e.what() << "\n";
        }
    });

    // Give WebSocket server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "\n[Main] Starting HTTP server...\n";
    
    // Start HTTP server (blocking)
    // Run in separate thread so we can handle shutdown
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

    // Wait for shutdown signal
    while (!shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "\n[Main] Shutting down gracefully...\n";
    
    // Stop WebSocket server
    if (global_ws_server) {
        std::cout << "[Main] Stopping WebSocket server...\n";
        global_ws_server->stop();
        delete global_ws_server;
        global_ws_server = nullptr;
    }
    
    // Flush WAL
    std::cout << "[Main] Flushing WAL...\n";
    global_wal.flush();
    
    // Wait for threads (with timeout)
    std::cout << "[Main] Waiting for threads to finish...\n";
    if (ws_thread.joinable()) {
        ws_thread.join();
    }
    
    // Note: httplib server doesn't have clean shutdown, so we just exit
    // In production, use a server with proper shutdown support
    
    std::cout << "[Main] Shutdown complete\n";
    return 0;
}
