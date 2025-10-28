// FILE: include/order_store.h
#pragma once

#include "order.h"
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <string>
#include <fstream>

class OrderStore {
public:
    OrderStore(const std::string &wal_path = "./data/wal.jsonl");
    ~OrderStore();

    std::string next_id();
    void add_order(const Order &o);
    bool has_order(const std::string &id);

private:
    std::unordered_map<std::string, Order> orders;
    std::mutex mu;
    std::atomic<uint64_t> id_counter;

    // Write-ahead log for audit & recovery
    std::ofstream wal;
    std::string wal_path;

    void wal_write(const std::string &line);
};

extern OrderStore global_order_store;