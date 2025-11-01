// FILE: src/global_state.cpp
#include "../include/global_state.h"

// Definitions of the global variables
std::mutex g_global_mutex;

std::unordered_map<std::string, OrderBook> g_order_books;
std::unordered_map<std::string, StopOrderManager> g_stop_order_managers;
std::unordered_map<std::string, std::string> g_order_id_to_symbol;

std::atomic<uint64_t> g_total_orders{0};
std::atomic<uint64_t> g_total_trades{0};

WebSocketServer* g_ws_server = nullptr;