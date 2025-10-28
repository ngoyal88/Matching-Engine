// FILE: src/main.cpp
#include <iostream>
#include <thread>
#include <csignal>
#include <atomic>
#include "../include/order.h"
#include "../include/wal.h"
#include "../include/ws_server.h"

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

    // Optional: Replay WAL for recovery
    std::cout << "[Main] Checking for WAL replay...\n";
    try {
        auto entries = global_wal.replay();
        if (!entries.empty()) {
            std::cout << "[Main] Found " << entries.size() << " WAL entries\n";
            std::cout << "[Main] Note: Full state recovery not implemented yet\n";
            // TODO: Reconstruct order books from WAL entries
        } else {
            std::cout << "[Main] No WAL entries found (fresh start)\n";
        }
    } catch (const std::exception &e) {
        std::cerr << "[Main] WAL replay error: " << e.what() << "\n";
    }

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