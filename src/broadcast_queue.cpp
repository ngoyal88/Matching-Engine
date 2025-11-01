// FILE: src/broadcast_queue.cpp
#include "../include/broadcast_queue.h"
#include "../include/global_state.h" // Include this to get g_ws_server
#include "../include/order_book.h"   // For Trade struct
#include <iostream>

// Define the global instance
BroadcastQueue g_broadcast_queue;

BroadcastQueue::BroadcastQueue() {
    // --- UPDATED: Create a pool of threads ---
    num_threads_ = std::thread::hardware_concurrency();
    if (num_threads_ == 0) num_threads_ = 4; // Default to 4 if detection fails
    
    std::cout << "[BroadcastQueue] Starting " << num_threads_ << " writer threads." << std::endl;
    for(unsigned int i = 0; i < num_threads_; ++i) {
        writer_threads_.emplace_back(&BroadcastQueue::writer_thread_loop, this);
    }
}

BroadcastQueue::~BroadcastQueue() {
    if (running_) {
        stop();
    }
    // --- UPDATED: Join all threads ---
    std::cout << "[BroadcastQueue] Joining writer threads..." << std::endl;
    for(auto& t : writer_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void BroadcastQueue::stop() {
    running_ = false;
    cv_.notify_all(); // <-- UPDATED: Wake up ALL threads to exit
}

// (push_trade is unchanged)
void BroadcastQueue::push_trade(const json& trade_json) {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push({BroadcastMessage::Type::Trade, "", trade_json});
    }
    cv_.notify_one(); // Wake up one available thread
}

// (push_book_update is unchanged)
void BroadcastQueue::push_book_update(const std::string& symbol, 
                                      const std::vector<std::pair<long long, long long>>& bids, 
                                      const std::vector<std::pair<long long, long long>>& asks) {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push({BroadcastMessage::Type::BookUpdate, symbol, {}, bids, asks});
    }
    cv_.notify_one(); // Wake up one available thread
}

void BroadcastQueue::writer_thread_loop() {
    // --- UPDATED: Thread loop logic ---
    while (running_) {
        BroadcastMessage msg;
        
        {
            std::unique_lock<std::mutex> lk(mu_);
            // Wait until queue has data or we are stopping
            cv_.wait(lk, [&]{ return !queue_.empty() || !running_; });

            if (!running_ && queue_.empty()) {
                return; // Exit
            }
            
            // Grab ONE message from the queue
            msg = std::move(queue_.front());
            queue_.pop();
        } // --- Mutex is unlocked here ---

        // Process this ONE message outside the lock
        // This allows other threads to be processing other messages in parallel
        try {
            if (!g_ws_server || !g_ws_server->is_running()) continue;

            if (msg.type == BroadcastMessage::Type::Trade) {
                // Manually parse JSON to Trade object
                Trade trade;
                trade.trade_id = msg.data.value("trade_id", "");
                trade.symbol = msg.data.value("symbol", "");
                trade.price = msg.data.value("price", 0LL);
                trade.quantity = msg.data.value("quantity", 0LL);
                trade.aggressor_side = msg.data.value("aggressor_side", "");
                trade.maker_order_id = msg.data.value("maker_order_id", "");
                trade.taker_order_id = msg.data.value("taker_order_id", "");
                trade.maker_fee = msg.data.value("maker_fee", 0LL);
                trade.taker_fee = msg.data.value("taker_fee", 0LL);
                trade.timestamp_iso = msg.data.value("timestamp", "");
                
                g_ws_server->broadcast_trade(trade);
            } 
            else if (msg.type == BroadcastMessage::Type::BookUpdate) {
                g_ws_server->broadcast_orderbook_update(msg.symbol, msg.bids, msg.asks);
            }
        } catch (const std::exception& e) {
            std::cerr << "[BroadcastThread] Error: " << e.what() << std::endl;
        }
    }
    // --- End of update ---
}

