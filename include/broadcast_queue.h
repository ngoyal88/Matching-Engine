// FILE: include/broadcast_queue.h
#pragma once
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include "../vendor/json.hpp"
#include "../include/ws_server.h" // Forward-declare global_state is tricky, just include ws_server

using json = nlohmann::json;

// Pre-declare global_state variables to break circular dependency
extern WebSocketServer* g_ws_server;

// A variant-like struct to hold different message types
struct BroadcastMessage {
    enum Type { Trade, BookUpdate };
    Type type;
    std::string symbol;
    json data; // Can hold trade json
    std::vector<std::pair<long long, long long>> bids;
    std::vector<std::pair<long long, long long>> asks;
};

class BroadcastQueue {
public:
    BroadcastQueue();
    ~BroadcastQueue();

    // Fast, non-blocking push for the server thread
    void push_trade(const json& trade_json);
    void push_book_update(const std::string& symbol, 
                          const std::vector<std::pair<long long, long long>>& bids, 
                          const std::vector<std::pair<long long, long long>>& asks);
    
    // Graceful shutdown
    void stop();

private:
    // The consumer thread loop
    void writer_thread_loop();

    std::queue<BroadcastMessage> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};
    
    // --- UPDATED: From single thread to thread pool ---
    unsigned int num_threads_;
    std::vector<std::thread> writer_threads_;
};

