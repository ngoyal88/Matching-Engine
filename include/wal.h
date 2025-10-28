// FILE: include/wal.h
#pragma once

#include <string>
#include <mutex>
#include <fstream>
#include <vector>
#include <chrono>
#include <atomic>
#include "../vendor/json.hpp"

class WAL {
public:
    explicit WAL(const std::string &path = "./data/wal.jsonl", 
                 size_t flush_interval = 100,
                 std::chrono::milliseconds flush_timeout = std::chrono::milliseconds(100));
    ~WAL();

    // Append a generic JSON object as a single line (thread-safe)
    void append_json(const nlohmann::json &j);

    // Convenience helpers
    void append_order(const nlohmann::json &order_json);
    void append_trade(const nlohmann::json &trade_json);
    void append_cancel(const std::string &order_id, const std::string &reason);
    
    // Force flush to disk
    void flush();
    
    // Recovery: replay all entries from WAL
    std::vector<nlohmann::json> replay();
    
    // Rotate WAL file (for maintenance)
    void rotate(const std::string &new_path);
    
    // Get stats
    size_t pending_writes() const { return write_count_; }
    size_t total_entries() const { return total_entries_; }

private:
    std::string path_;
    std::ofstream ofs_;
    std::mutex mu_;
    
    // Batch flushing configuration
    size_t flush_interval_;
    std::chrono::milliseconds flush_timeout_;
    
    // Counters
    std::atomic<size_t> write_count_{0};
    std::atomic<size_t> total_entries_{0};
    std::chrono::steady_clock::time_point last_flush_;
    
    // Internal flush helper
    void flush_internal();
    
    // Check if flush is needed
    bool should_flush();
};

// global WAL instance (defined in wal_integration.cpp)
extern WAL global_wal;