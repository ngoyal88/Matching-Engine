#pragma once
#include "order_book.h"
#include "stop_order_manager.h"
#include <unordered_map>
#include <string>
#include <mutex>

/**
 * @brief Defines the global state of the matching engine.
 * * These variables are declared here (extern) and defined in global_state.cpp.
 * This allows both main.cpp (for WAL replay) and server.cpp (for operations)
 * to access and modify the same state.
 */

// Global map of order books, keyed by symbol (e.g., "BTC-USDT")
extern std::unordered_map<std::string, OrderBook> g_order_books;

// Global map of stop order managers, keyed by symbol
extern std::unordered_map<std::string, StopOrderManager> g_stop_order_managers;

// Global map to find an order's symbol by its ID (for cancellations)
extern std::unordered_map<std::string, std::string> g_order_to_symbol;

// Mutex to protect g_order_to_symbol from concurrent access
extern std::mutex g_order_to_symbol_mutex;

extern std::atomic<uint64_t> g_total_orders;
extern std::atomic<uint64_t> g_total_trades;