// FILE: include/wal.h
#pragma once

#include <string>
#include <mutex>
#include <fstream>
#include <vector>
#include <chrono>
#include <atomic>
#include <queue>
#include <thread>
#include <condition_variable>
#include "../vendor/json.hpp"

class WAL {
public:
    // Constructor no longer takes flush intervals
    explicit WAL(const std::string &path = "./data/wal.jsonl");
    ~WAL();

    // Append a generic JSON object (now very fast)
    void append_json(const nlohmann::json &j);

    // Convenience helpers (unchanged)
    void append_order(const nlohmann::json &order_json);
    void append_trade(const nlohmann::json &trade_json);
    void append_cancel(const std::string &order_id, const std::string &reason);
    
    // Force flush of the ofstream buffer
    void flush();
    
    // Recovery: replay all entries from WAL (unchanged)
    std::vector<nlohmann::json> replay();
    
    // Rotate WAL file (now thread-safe against the writer)
    void rotate(const std::string &new_path);

    // Stop the writer thread gracefully
    void stop();
    
    // Get stats
    size_t pending_writes(); // Now returns queue_size
    size_t total_entries() const { return total_entries_; }

private:
    std::string path_;
    std::ofstream ofs_;
    std::mutex mu_; // Protects the queue AND ofs_
    
    // --- Asynchronous Writer Components ---
    std::atomic<bool> running_{false};
    std::thread writer_thread_;
    std::queue<std::string> queue_;
    std::condition_variable cv_;
    
    std::atomic<size_t> total_entries_{0};
    
    // Internal writer loop
    void writer_thread_loop();

    // Internal flush helper (must be called while mu_ is locked)
    void flush_internal();
};

// global WAL instance (defined in wal_integration.cpp)
extern WAL global_wal;
