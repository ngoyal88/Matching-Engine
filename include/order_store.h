// FILE: include/order_store.h
#pragma once

#include "order.h"
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <string>

class OrderStore {
public:
    OrderStore(const std::string &wal_path = "./data/wal.jsonl");
    ~OrderStore();

    std::string next_id();
    void add_order(const Order &o);
    bool has_order(const std::string &id);
    Order get_order(const std::string &id);

private:
    std::unordered_map<std::string, Order> orders;
    std::mutex mu;
    std::atomic<uint64_t> id_counter;
    std::string wal_path; // kept for compatibility, actual WAL is global
};

extern OrderStore global_order_store;