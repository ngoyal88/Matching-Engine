// ============================================================================
// FILE: src/ws_server.cpp (Production-Ready WebSocket Implementation)
// ============================================================================
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
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
typedef int socket_t;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define CLOSE_SOCKET close
#endif

using json = nlohmann::json;

// WebSocket utilities per RFC 6455
namespace ws {
    
    std::string base64_encode(const std::string &input) {
        static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string output;
        int val = 0, valb = -6;
        
        for (unsigned char c : input) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                output.push_back(chars[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) output.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
        while (output.size() % 4) output.push_back('=');
        return output;
    }
    
    std::string sha1(const std::string &input) {
        #ifdef _WIN32
        HCRYPTPROV hProv = 0;
        HCRYPTHASH hHash = 0;
        std::string out;
        if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
            return out;
        }
        if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
            CryptReleaseContext(hProv, 0);
            return out;
        }
        if (!CryptHashData(hHash, reinterpret_cast<const BYTE*>(input.data()), static_cast<DWORD>(input.size()), 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            return out;
        }
        DWORD hashLen = 0;
        DWORD cbHashLen = sizeof(DWORD);
        if (!CryptGetHashParam(hHash, HP_HASHSIZE, reinterpret_cast<BYTE*>(&hashLen), &cbHashLen, 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            return out;
        }
        std::vector<BYTE> hash(hashLen);
        if (!CryptGetHashParam(hHash, HP_HASHVAL, hash.data(), &hashLen, 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            return out;
        }
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        out.assign(reinterpret_cast<const char*>(hash.data()), hashLen);
        return out;
        #else
        // Non-Windows: not implemented in this minimal build
        return std::string();
        #endif
    }
    
    std::string compute_accept_key(const std::string &key) {
        const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string combined = key + magic;
        std::string hash = sha1(combined);
        return base64_encode(hash);
    }
    
    std::string encode_frame(const std::string &payload) {
        std::vector<uint8_t> frame;
        
        frame.push_back(0x81); // FIN=1, opcode=1 (text)
        
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
    
    struct Frame {
        bool fin;
        uint8_t opcode;
        bool masked;
        std::string payload;
        bool valid;
    };
    
    Frame decode_frame(const uint8_t* data, size_t len) {
        Frame frame;
        frame.valid = false;
        
        if (len < 2) return frame;
        
        frame.fin = (data[0] & 0x80) != 0;
        frame.opcode = data[0] & 0x0F;
        frame.masked = (data[1] & 0x80) != 0;
        
        uint64_t payload_len = data[1] & 0x7F;
        size_t pos = 2;
        
        if (payload_len == 126) {
            if (len < 4) return frame;
            payload_len = (data[2] << 8) | data[3];
            pos = 4;
        } else if (payload_len == 127) {
            if (len < 10) return frame;
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) | data[2 + i];
            }
            pos = 10;
        }
        
        uint8_t mask[4] = {0};
        if (frame.masked) {
            if (len < pos + 4) return frame;
            memcpy(mask, data + pos, 4);
            pos += 4;
        }
        
        if (len < pos + payload_len) return frame;
        
        frame.payload.resize(payload_len);
        for (size_t i = 0; i < payload_len; ++i) {
            frame.payload[i] = data[pos + i] ^ (frame.masked ? mask[i % 4] : 0);
        }
        
        frame.valid = true;
        return frame;
    }
}

struct WSConnection {
    socket_t socket;
    std::atomic<bool> active;
    std::string id;
    std::mutex send_mutex;
    
    WSConnection(socket_t s) : socket(s), active(true) {
        static std::atomic<uint64_t> counter{1};
        id = "conn_" + std::to_string(counter.fetch_add(1));
    }
    
    bool send(const std::string &data) {
        std::lock_guard<std::mutex> lock(send_mutex);
        if (!active.load()) return false;
        
        size_t total_sent = 0;
        while (total_sent < data.size()) {
            int sent = ::send(socket, data.c_str() + total_sent, 
                            data.size() - total_sent, 0);
            if (sent <= 0) {
                active = false;
                return false;
            }
            total_sent += sent;
        }
        return true;
    }
};

class WebSocketServerImpl {
public:
    socket_t server_socket;
    std::vector<std::shared_ptr<WSConnection>> connections;
    std::mutex connections_mutex;
    std::atomic<bool> running;
    std::thread accept_thread;
    std::thread cleanup_thread;
    int port_;
    
    WebSocketServerImpl(int port) : server_socket(INVALID_SOCKET), running(false), port_(port) {}
    
    ~WebSocketServerImpl() {
        stop();
    }
    
    bool start() {
        #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "[WS] WSAStartup failed\n";
            return false;
        }
        #endif
        
        server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket == INVALID_SOCKET) {
            std::cerr << "[WS] Socket creation failed\n";
            return false;
        }
        
        int opt = 1;
        setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, 
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
        
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);
        
        if (bind(server_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::cerr << "[WS] Bind failed on port " << port_ << "\n";
            CLOSE_SOCKET(server_socket);
            return false;
        }
        
        if (listen(server_socket, 128) == SOCKET_ERROR) {
            std::cerr << "[WS] Listen failed\n";
            CLOSE_SOCKET(server_socket);
            return false;
        }
        
        running = true;
        accept_thread = std::thread(&WebSocketServerImpl::accept_loop, this);
        cleanup_thread = std::thread(&WebSocketServerImpl::cleanup_loop, this);
        
        std::cout << "[WS] Server started on port " << port_ << "\n";
        return true;
    }
    
    void stop() {
        if (!running.load()) return;
        
        std::cout << "[WS] Stopping server...\n";
        running = false;
        
        if (server_socket != INVALID_SOCKET) {
            CLOSE_SOCKET(server_socket);
            server_socket = INVALID_SOCKET;
        }
        
        if (accept_thread.joinable()) {
            accept_thread.join();
        }
        
        if (cleanup_thread.joinable()) {
            cleanup_thread.join();
        }
        
        std::lock_guard<std::mutex> lock(connections_mutex);
        for (auto &conn : connections) {
            if (conn->active.load()) {
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
        while (running.load()) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(server_socket, &readfds);
            
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            
            int activity = select(server_socket + 1, &readfds, nullptr, nullptr, &tv);
            
            if (activity < 0 && running.load()) {
                std::cerr << "[WS] Select error\n";
                break;
            }
            
            if (activity == 0 || !FD_ISSET(server_socket, &readfds)) {
                continue;
            }
            
            sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            
            socket_t client_socket = accept(server_socket, 
                                           reinterpret_cast<sockaddr*>(&client_addr), 
                                           &addr_len);
            
            if (client_socket == INVALID_SOCKET) {
                if (running.load()) {
                    std::cerr << "[WS] Accept failed\n";
                }
                continue;
            }
            
            std::thread(&WebSocketServerImpl::handle_client, this, client_socket).detach();
        }
    }
    
    void cleanup_loop() {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            std::lock_guard<std::mutex> lock(connections_mutex);
            connections.erase(
                std::remove_if(connections.begin(), connections.end(),
                    [](const auto &conn) { return !conn->active.load(); }),
                connections.end()
            );
        }
    }
    
    void handle_client(socket_t client_socket) {
        char buffer[8192];
        
        int bytes = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) {
            CLOSE_SOCKET(client_socket);
            return;
        }
        
        buffer[bytes] = '\0';
        std::string request(buffer);
        
        std::string key;
        size_t key_pos = request.find("Sec-WebSocket-Key:");
        if (key_pos != std::string::npos) {
            size_t start = request.find_first_not_of(" \t", key_pos + 18);
            size_t end = request.find("\r\n", start);
            if (start != std::string::npos && end != std::string::npos) {
                key = request.substr(start, end - start);
            }
        }
        
        if (key.empty()) {
            CLOSE_SOCKET(client_socket);
            return;
        }
        
        std::string accept_key = ws::compute_accept_key(key);
        if (accept_key.empty()) {
            CLOSE_SOCKET(client_socket);
            return;
        }
        
        std::ostringstream response;
        response << "HTTP/1.1 101 Switching Protocols\r\n";
        response << "Upgrade: websocket\r\n";
        response << "Connection: Upgrade\r\n";
        response << "Sec-WebSocket-Accept: " << accept_key << "\r\n";
        response << "\r\n";
        
        std::string handshake = response.str();
        send(client_socket, handshake.c_str(), handshake.size(), 0);
        
        auto conn = std::make_shared<WSConnection>(client_socket);
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            connections.push_back(conn);
        }
        
        std::cout << "[WS] Client " << conn->id << " connected (total: " 
                  << connections.size() << ")\n";
        
        json welcome = {
            {"type", "connected"},
            {"message", "Connected to matching engine"},
            {"connection_id", conn->id},
            {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()}
        };
        conn->send(ws::encode_frame(welcome.dump()));
        
        while (running.load() && conn->active.load()) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(client_socket, &readfds);
            
            struct timeval tv;
            tv.tv_sec = 30;
            tv.tv_usec = 0;
            
            int activity = select(client_socket + 1, &readfds, nullptr, nullptr, &tv);
            
            if (activity < 0) {
                break;
            }
            
            if (activity == 0) {
                // Timeout - send ping
                std::string ping_frame;
                ping_frame.push_back(0x89); // Ping opcode
                ping_frame.push_back(0x00); // No payload
                conn->send(ping_frame);
                continue;
            }
            
            bytes = recv(client_socket, buffer, sizeof(buffer), 0);
            if (bytes <= 0) {
                break;
            }
            
            ws::Frame frame = ws::decode_frame(reinterpret_cast<uint8_t*>(buffer), bytes);
            
            if (!frame.valid) continue;
            
            if (frame.opcode == 0x8) { // Close
                break;
            } else if (frame.opcode == 0x9) { // Ping
                std::string pong_frame;
                pong_frame.push_back(0x8A); // Pong opcode
                pong_frame.push_back(0x00);
                conn->send(pong_frame);
            } else if (frame.opcode == 0xA) { // Pong
                // Keep alive received
            }
        }
        
        std::cout << "[WS] Client " << conn->id << " disconnected\n";
        conn->active = false;
        CLOSE_SOCKET(client_socket);
    }
    
    void broadcast(const std::string &message) {
        std::string frame = ws::encode_frame(message);
        
        std::lock_guard<std::mutex> lock(connections_mutex);
        for (auto &conn : connections) {
            if (conn->active.load()) {
                conn->send(frame);
            }
        }
    }
    
    size_t client_count() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(connections_mutex));
        return std::count_if(connections.begin(), connections.end(),
                            [](const auto &c) { return c->active.load(); });
    }
};

// WebSocketServer wrapper implementation
WebSocketServer::WebSocketServer(int port) 
    : port_(port), running_(false), server_impl_(nullptr) {
    server_impl_ = new WebSocketServerImpl(port);
}

WebSocketServer::~WebSocketServer() {
    stop();
    if (server_impl_) {
        delete static_cast<WebSocketServerImpl*>(server_impl_);
    }
}

void WebSocketServer::start() {
    auto impl = static_cast<WebSocketServerImpl*>(server_impl_);
    if (impl->start()) {
        running_ = true;
    }
}

void WebSocketServer::stop() {
    if (!running_.load()) return;
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
            {"maker_fee", trade.maker_fee},
            {"taker_fee", trade.taker_fee},
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
            {"asks", asks_array},
            {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()}
        }}
    };
    
    broadcast_json(j.dump());
}

void WebSocketServer::broadcast_json(const std::string &json_msg) {
    if (!running_.load()) return;
    auto impl = static_cast<WebSocketServerImpl*>(server_impl_);
    impl->broadcast(json_msg);
}

size_t WebSocketServer::client_count() const {
    if (!server_impl_) return 0;
    auto impl = static_cast<WebSocketServerImpl*>(server_impl_);
    return impl->client_count();
}

WebSocketServer* global_ws_server = nullptr;
