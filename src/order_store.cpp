// FILE: src/order_store.cpp
#include "../include/order_store.h"
#include <iostream>
#include <sstream>

OrderStore global_order_store = OrderStore();

OrderStore::OrderStore(const std::string &path) : id_counter(1), wal_path(path) {
    // WAL is now handled by global_wal, so we don't open it here
    std::cout << "[OrderStore] Initialized (using global WAL)\n";
}

OrderStore::~OrderStore() {
    // No need to close WAL here since it's managed globally
}

std::string OrderStore::next_id() {
    auto id = id_counter.fetch_add(1);
    std::ostringstream ss;
    ss << "ORD-" << id;
    return ss.str();
}

void OrderStore::add_order(const Order &o) {
    std::lock_guard<std::mutex> lk(mu);
    orders[o.order_id] = o;
    // WAL writing is now done in server.cpp using global_wal
}

bool OrderStore::has_order(const std::string &id) {
    std::lock_guard<std::mutex> lk(mu);
    return orders.find(id) != orders.end();
}

Order OrderStore::get_order(const std::string &id) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = orders.find(id);
    if (it == orders.end()) {
        throw std::runtime_error("Order not found: " + id);
    }
    return it->second;
}