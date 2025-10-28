// FILE: include/wal.h
#pragma once

#include <string>
#include <mutex>
#include <fstream>
#include "../vendor/json.hpp"

class WAL {
public:
    explicit WAL(const std::string &path = "./data/wal.jsonl");
    ~WAL();

    // Append a generic JSON object as a single line (thread-safe)
    void append_json(const nlohmann::json &j);

    // Convenience helpers
    void append_order(const nlohmann::json &order_json);
    void append_trade(const nlohmann::json &trade_json);

private:
    std::string path_;
    std::ofstream ofs_;
    std::mutex mu_;
};

// global WAL instance (defined in wal_integration.cpp)
extern WAL global_wal;
