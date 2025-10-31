#include "../include/global_state.h"

/**
 * @brief Defines the global state variables.
 * * This file provides the actual storage for the 'extern' variables
 * declared in global_state.h.
 */

std::unordered_map<std::string, OrderBook> g_order_books;
std::unordered_map<std::string, StopOrderManager> g_stop_order_managers;
std::unordered_map<std::string, std::string> g_order_to_symbol;
std::mutex g_order_to_symbol_mutex;
std::atomic<uint64_t> g_total_orders{0};
std::atomic<uint64_t> g_total_trades{0};
