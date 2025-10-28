// FILE: src/order_store.cpp
#include "../include/order_store.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

OrderStore global_order_store = OrderStore();

OrderStore::OrderStore(const std::string &path) : id_counter(1), wal_path(path) {
    // open WAL in append mode, ensure directory exists in deployment
    wal.open(wal_path, std::ios::out | std::ios::app);
    if (!wal.is_open()) {
        std::cerr << "Failed to open WAL file: " << wal_path << "\n";
    }
}

OrderStore::~OrderStore() {
    if (wal.is_open()) wal.close();
}

std::string OrderStore::next_id() {
    auto id = id_counter.fetch_add(1);
    std::ostringstream ss;
    ss << "ORD-" << id;
    return ss.str();
}

void OrderStore::wal_write(const std::string &line) {
    if (!wal.is_open()) return;
    wal << line << '\n';
    // flush to ensure persistence for Milestone 1; can batch later
    wal.flush();
}

void OrderStore::add_order(const Order &o) {
    {
        std::lock_guard<std::mutex> lk(mu);
        orders[o.order_id] = o;
    }
    // serialize minimal order info to WAL (JSON line)
    std::ostringstream ss;
    auto t = std::chrono::system_clock::to_time_t(o.timestamp);
    ss << "{\"order_id\":\"" << o.order_id << "\",\"symbol\":\"" << o.symbol
       << "\",\"order_type\":\"" << o.order_type << "\",\"side\":\"" << o.side
       << "\",\"quantity\":\"" << o.quantity << "\",\"price\":\"" << o.price
       << "\",\"timestamp\":\"" << std::put_time(gmtime(&t), "%Y-%m-%dT%H:%M:%SZ") << "\"}";
    wal_write(ss.str());
}

bool OrderStore::has_order(const std::string &id) {
    std::lock_guard<std::mutex> lk(mu);
    return orders.find(id) != orders.end();
}