// FILE: src/wal.cpp
#include "../include/wal.h"
#include <iostream>

WAL::WAL(const std::string &path) : path_(path) {
    // Ensure directory exists is left to deployment; open append
    ofs_.open(path_, std::ios::out | std::ios::app);
    if (!ofs_.is_open()) {
        std::cerr << "Failed to open WAL file: " << path_ << std::endl;
    }
}

WAL::~WAL() {
    if (ofs_.is_open()) ofs_.close();
}

void WAL::append_json(const nlohmann::json &j) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ofs_.is_open()) return;
    ofs_ << j.dump() << '\n';
    // flush to make durable for Milestone 3; can be batched later
    ofs_.flush();
}

void WAL::append_order(const nlohmann::json &order_json) {
    nlohmann::json j = {
        {"type", "order"},
        {"payload", order_json}
    };
    append_json(j);
}

void WAL::append_trade(const nlohmann::json &trade_json) {
    nlohmann::json j = {
        {"type", "trade"},
        {"payload", trade_json}
    };
    append_json(j);
}
