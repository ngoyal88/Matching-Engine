// FILE: src/ws_server.cpp
#include "../include/ws_server.h"
#include "../include/order_book.h"
#include "../vendor/json.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <set>
#include <chrono>
#include <sstream>
#include <iomanip>

// Simple WebSocket implementation using raw TCP sockets
// For production, consider using uWebSockets or Boost.Beast
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
typedef int socket_t;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define CLOSE_SOCKET close
#endif

using json = nlohmann::json;

// Simple WebSocket frame encoding (RFC 6455)
namespace ws_protocol {
    std::string encode_frame(const std::string &payload) {
        std::vector<uint8_t> frame;
        
        // FIN=1, opcode=1 (text)
        frame.push_back(0x81);
        
        size_t len = payload.size();
        if (len <= 125) {
            frame.push_back(static_cast<uint8_t>(len));
        } else if (len <= 65535) {
            frame.push_back(126);
            frame.push_back((len >> 8) & 0xFF);
            frame.push_back(len & 0xFF);
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; --i) {
                frame.push_back((len >> (i * 8)) & 0xFF);
            }
        }
        
        frame.insert(frame.end(), payload.begin(), payload.end());
        return std::string(frame.begin(), frame.end());
    }
    
    std::string create_handshake_response(const std::string &key) {
        // WebSocket handshake per RFC 6455
        std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string accept_key = key + magic;
        
        // Simple SHA1 would go here - for now we'll use a placeholder
        // In production, use OpenSSL or similar
        std::string accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="; // placeholder
        
        std::ostringstream response;
        response << "HTTP/1.1 101 Switching Protocols\r\n";
        response << "Upgrade: websocket\r\n";
        response << "Connection: Upgrade\r\n";
        response << "Sec-WebSocket-Accept: " << accept << "\r\n";
        response << "\r\n";
        
        return response.str();
    }
}

// Connection management
struct WSConnection {
    socket_t socket;
    bool active;
    std::string id;
    
    WSConnection(socket_t s) : socket(s), active(true) {
        static std::atomic<uint64_t> counter{1};
        id = "conn_" + std::to_string(counter.fetch_add(1));
    }
};

class WebSocketServerImpl {
public:
    socket_t server_socket;
    std::vector<std::shared_ptr<WSConnection>> connections;
    std::mutex connections_mutex;
    std::atomic<bool> running;
    std::thread accept_thread;
    
    WebSocketServerImpl() : server_socket(INVALID_SOCKET), running(false) {}
    
    ~WebSocketServerImpl() {
        stop();
    }
    
    bool start(int port) {
        #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "[WS] WSAStartup failed\n";
            return false;
        }
        #endif
        
        server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket == INVALID_SOCKET) {
            std::cerr << "[WS] Failed to create socket\n";
            return false;
        }
        
        // Set SO_REUSEADDR
        int opt = 1;
        setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, 
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
        
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        
        if (bind(server_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::cerr << "[WS] Bind failed on port " << port << "\n";
            CLOSE_SOCKET(server_socket);
            return false;
        }
        
        if (listen(server_socket, 10) == SOCKET_ERROR) {
            std::cerr << "[WS] Listen failed\n";
            CLOSE_SOCKET(server_socket);
            return false;
        }
        
        running = true;
        accept_thread = std::thread(&WebSocketServerImpl::accept_loop, this);
        
        std::cout << "[WS] Server started on port " << port << "\n";
        return true;
    }
    
    void stop() {
        if (!running) return;
        running = false;
        
        if (server_socket != INVALID_SOCKET) {
            CLOSE_SOCKET(server_socket);
            server_socket = INVALID_SOCKET;
        }
        
        if (accept_thread.joinable()) {
            accept_thread.join();
        }
        
        std::lock_guard<std::mutex> lock(connections_mutex);
        for (auto &conn : connections) {
            if (conn->active) {
                CLOSE_SOCKET(conn->socket);
                conn->active = false;
            }
        }
        connections.clear();
        
        #ifdef _WIN32
        WSACleanup();
        #endif
        
        std::cout << "[WS] Server stopped\n";
    }
    
    void accept_loop() {
        while (running) {
            sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            
            socket_t client_socket = accept(server_socket, 
                                           reinterpret_cast<sockaddr*>(&client_addr), 
                                           &addr_len);
            
            if (client_socket == INVALID_SOCKET) {
                if (running) {
                    std::cerr << "[WS] Accept failed\n";
                }
                continue;
            }
            
            std::cout << "[WS] New connection from " 
                      << inet_ntoa(client_addr.sin_addr) << "\n";
            
            // Handle WebSocket handshake and client in separate thread
            std::thread(&WebSocketServerImpl::handle_client, this, client_socket).detach();
        }
    }
    
    void handle_client(socket_t client_socket) {
        char buffer[4096];
        
        // Read HTTP upgrade request
        int bytes = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) {
            CLOSE_SOCKET(client_socket);
            return;
        }
        
        buffer[bytes] = '\0';
        std::string request(buffer);
        
        // Extract Sec-WebSocket-Key
        std::string key;
        size_t key_pos = request.find("Sec-WebSocket-Key:");
        if (key_pos != std::string::npos) {
            size_t start = request.find_first_not_of(" \t", key_pos + 18);
            size_t end = request.find("\r\n", start);
            key = request.substr(start, end - start);
        }
        
        // Send handshake response
        std::string handshake = ws_protocol::create_handshake_response(key);
        send(client_socket, handshake.c_str(), handshake.size(), 0);
        
        // Add to connections
        auto conn = std::make_shared<WSConnection>(client_socket);
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            connections.push_back(conn);
        }
        
        std::cout << "[WS] Client " << conn->id << " connected\n";
        
        // Send welcome message
        json welcome = {
            {"type", "connected"},
            {"message", "Connected to matching engine WebSocket"},
            {"connection_id", conn->id}
        };
        send_to_client(conn, welcome.dump());
        
        // Keep connection alive and read messages
        while (running && conn->active) {
            bytes = recv(client_socket, buffer, sizeof(buffer), 0);
            if (bytes <= 0) {
                conn->active = false;
                break;
            }
            
            // Simple ping/pong to keep alive
            // In production, properly decode WebSocket frames
        }
        
        std::cout << "[WS] Client " << conn->id << " disconnected\n";
        CLOSE_SOCKET(client_socket);
        
        // Remove from connections
        std::lock_guard<std::mutex> lock(connections_mutex);
        connections.erase(
            std::remove_if(connections.begin(), connections.end(),
                          [&](const auto &c) { return c->id == conn->id; }),
            connections.end()
        );
    }
    
    void broadcast(const std::string &message) {
        std::string frame = ws_protocol::encode_frame(message);
        
        std::lock_guard<std::mutex> lock(connections_mutex);
        for (auto &conn : connections) {
            if (conn->active) {
                send_to_client(conn, message);
            }
        }
    }
    
    void send_to_client(std::shared_ptr<WSConnection> conn, const std::string &message) {
        std::string frame = ws_protocol::encode_frame(message);
        send(conn->socket, frame.c_str(), frame.size(), 0);
    }
};

// WebSocketServer implementation
WebSocketServer::WebSocketServer(int port) 
    : port_(port), running_(false), server_impl_(nullptr) {
    server_impl_ = new WebSocketServerImpl();
}

WebSocketServer::~WebSocketServer() {
    stop();
    if (server_impl_) {
        delete static_cast<WebSocketServerImpl*>(server_impl_);
    }
}

void WebSocketServer::start() {
    auto impl = static_cast<WebSocketServerImpl*>(server_impl_);
    if (impl->start(port_)) {
        running_ = true;
    }
}

void WebSocketServer::stop() {
    if (!running_) return;
    running_ = false;
    auto impl = static_cast<WebSocketServerImpl*>(server_impl_);
    impl->stop();
}

void WebSocketServer::broadcast_trade(const Trade &trade) {
    json j = {
        {"type", "trade"},
        {"data", {
            {"trade_id", trade.trade_id},
            {"symbol", trade.symbol},
            {"price", trade.price},
            {"quantity", trade.quantity},
            {"aggressor_side", trade.aggressor_side},
            {"maker_order_id", trade.maker_order_id},
            {"taker_order_id", trade.taker_order_id},
            {"timestamp", trade.timestamp_iso}
        }}
    };
    
    broadcast_json(j.dump());
}

void WebSocketServer::broadcast_orderbook_update(
    const std::string &symbol,
    const std::vector<std::pair<long long, long long>> &bids,
    const std::vector<std::pair<long long, long long>> &asks) {
    
    json bids_array = json::array();
    for (const auto &[price, qty] : bids) {
        bids_array.push_back({{"price", price}, {"quantity", qty}});
    }
    
    json asks_array = json::array();
    for (const auto &[price, qty] : asks) {
        asks_array.push_back({{"price", price}, {"quantity", qty}});
    }
    
    json j = {
        {"type", "orderbook"},
        {"data", {
            {"symbol", symbol},
            {"bids", bids_array},
            {"asks", asks_array}
        }}
    };
    
    broadcast_json(j.dump());
}

void WebSocketServer::broadcast_json(const std::string &json_msg) {
    if (!running_) return;
    auto impl = static_cast<WebSocketServerImpl*>(server_impl_);
    impl->broadcast(json_msg);
}

// Global instance (nullptr by default, initialized in main)
WebSocketServer* global_ws_server = nullptr;