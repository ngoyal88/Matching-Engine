// FILE: include/global_state.h
#pragma once
#include <mutex>
#include <string>
#include <unordered_map>
#include <atomic>
#include "order_book.h"
#include "stop_order_manager.h"
#include "ws_server.h"
#include "../include/broadcast_queue.h"

// This mutex protects shared access to the order books and maps
extern std::mutex g_global_mutex;

// Maps for tracking and managing books
extern std::unordered_map<std::string, OrderBook> g_order_books;
extern std::unordered_map<std::string, StopOrderManager> g_stop_order_managers;
extern std::unordered_map<std::string, std::string> g_order_id_to_symbol;

// Global server stats
extern std::atomic<uint64_t> g_total_orders;
extern std::atomic<uint64_t> g_total_trades;

// Global WebSocket server instance
extern WebSocketServer* g_ws_server;

// Global Broadcast Queue
extern BroadcastQueue g_broadcast_queue;
