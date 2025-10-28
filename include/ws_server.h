// FILE: include/ws_server.h
#pragma once
#include "order_book.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>

// Forward declarations to avoid heavy includes in header
class WebSocketConnection;

class WebSocketServer {
public:
    explicit WebSocketServer(int port);
    ~WebSocketServer();
    
    // Start server in separate thread
    void start();
    
    // Stop server gracefully
    void stop();
    
    // Broadcast messages to all connected clients
    void broadcast_trade(const Trade &trade);
    void broadcast_orderbook_update(const std::string &symbol, 
                                    const std::vector<std::pair<long long, long long>> &bids,
                                    const std::vector<std::pair<long long, long long>> &asks);
    
    // Check if server is running
    bool is_running() const { return running_; }
    
private:
    int port_;
    std::atomic<bool> running_;
    void* server_impl_; // Opaque pointer to implementation (uWebSockets App)
    
    // Internal broadcast helper
    void broadcast_json(const std::string &json_msg);
};

// Global WebSocket server instance (initialized in ws_server.cpp)
extern WebSocketServer* global_ws_server;