// ============================================================================
// FILE: include/ws_server.h
// ============================================================================
#pragma once
// Forward declarations to avoid pulling in headers that "using namespace std" (Windows byte conflict)
struct Trade;
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <set>
#include <mutex>

class WebSocketServer {
public:
    explicit WebSocketServer(int port);
    ~WebSocketServer();
    
    void start();
    void stop();
    
    void broadcast_trade(const Trade &trade);
    void broadcast_orderbook_update(const std::string &symbol, 
                                    const std::vector<std::pair<long long, long long>> &bids,
                                    const std::vector<std::pair<long long, long long>> &asks);
    
    bool is_running() const { return running_.load(); }
    size_t client_count() const;
    
private:
    int port_;
    std::atomic<bool> running_;
    void* server_impl_;
    
    void broadcast_json(const std::string &json_msg);
};

extern WebSocketServer* global_ws_server;
