// FILE: src/wal.cpp
#include "../include/wal.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <filesystem>

WAL::WAL(const std::string &path, size_t flush_interval, std::chrono::milliseconds flush_timeout)
    : path_(path), flush_interval_(flush_interval), flush_timeout_(flush_timeout) {
    
    // Ensure directory exists
    std::filesystem::path file_path(path_);
    std::filesystem::path dir_path = file_path.parent_path();
    
    if (!dir_path.empty() && !std::filesystem::exists(dir_path)) {
        try {
            std::filesystem::create_directories(dir_path);
            std::cout << "[WAL] Created directory: " << dir_path << std::endl;
        } catch (const std::exception &e) {
            std::cerr << "[WAL] Failed to create directory: " << e.what() << std::endl;
        }
    }
    
    // Open in append mode
    ofs_.open(path_, std::ios::out | std::ios::app);
    if (!ofs_.is_open()) {
        std::cerr << "[WAL] Failed to open WAL file: " << path_ << std::endl;
        throw std::runtime_error("Cannot open WAL file: " + path_);
    }
    
    last_flush_ = std::chrono::steady_clock::now();
    std::cout << "[WAL] Initialized: " << path_ << std::endl;
}

WAL::~WAL() {
    flush();
    if (ofs_.is_open()) {
        ofs_.close();
    }
    std::cout << "[WAL] Closed. Total entries: " << total_entries_.load() << std::endl;
}

void WAL::append_json(const nlohmann::json &j) {
    std::lock_guard<std::mutex> lk(mu_);
    
    if (!ofs_.is_open()) {
        throw std::runtime_error("WAL file is not open");
    }
    
    try {
        ofs_ << j.dump() << '\n';
        write_count_++;
        total_entries_++;
        
        // Check if we should flush
        if (should_flush()) {
            flush_internal();
        }
    } catch (const std::exception &e) {
        std::cerr << "[WAL] Write error: " << e.what() << std::endl;
        throw;
    }
}

void WAL::append_order(const nlohmann::json &order_json) {
    nlohmann::json j = {
        {"type", "order"},
        {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()},
        {"payload", order_json}
    };
    append_json(j);
}

void WAL::append_trade(const nlohmann::json &trade_json) {
    nlohmann::json j = {
        {"type", "trade"},
        {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()},
        {"payload", trade_json}
    };
    append_json(j);
}

void WAL::append_cancel(const std::string &order_id, const std::string &reason) {
    nlohmann::json j = {
        {"type", "cancel"},
        {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()},
        {"payload", {
            {"order_id", order_id},
            {"reason", reason}
        }}
    };
    append_json(j);
}

bool WAL::should_flush() {
    // Flush if we've reached the write count threshold
    if (write_count_ >= flush_interval_) {
        return true;
    }
    
    // Flush if timeout has elapsed since last flush
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_);
    if (elapsed >= flush_timeout_ && write_count_ > 0) {
        return true;
    }
    
    return false;
}

void WAL::flush_internal() {
    if (!ofs_.is_open()) return;
    
    ofs_.flush();
    write_count_ = 0;
    last_flush_ = std::chrono::steady_clock::now();
}

void WAL::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    flush_internal();
}

std::vector<nlohmann::json> WAL::replay() {
    std::vector<nlohmann::json> entries;
    
    std::cout << "[WAL] Replaying from: " << path_ << std::endl;
    
    std::ifstream ifs(path_);
    if (!ifs.is_open()) {
        std::cerr << "[WAL] Cannot open file for replay: " << path_ << std::endl;
        return entries;
    }
    
    std::string line;
    size_t line_num = 0;
    size_t errors = 0;
    
    while (std::getline(ifs, line)) {
        line_num++;
        
        if (line.empty()) continue;
        
        try {
            nlohmann::json j = nlohmann::json::parse(line);
            entries.push_back(j);
        } catch (const std::exception &e) {
            std::cerr << "[WAL] Parse error at line " << line_num 
                      << ": " << e.what() << std::endl;
            errors++;
        }
    }
    
    ifs.close();
    
    std::cout << "[WAL] Replay complete. Entries: " << entries.size() 
              << ", Errors: " << errors << std::endl;
    
    return entries;
}

void WAL::rotate(const std::string &new_path) {
    std::lock_guard<std::mutex> lk(mu_);
    
    // Flush current data
    flush_internal();
    
    // Close current file
    if (ofs_.is_open()) {
        ofs_.close();
    }
    
    // Rename old file with timestamp
    try {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::system_clock::to_time_t(now);
        std::string backup_path = path_ + "." + std::to_string(timestamp);
        
        std::filesystem::rename(path_, backup_path);
        std::cout << "[WAL] Rotated to: " << backup_path << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "[WAL] Rotation error: " << e.what() << std::endl;
    }
    
    // Open new file
    path_ = new_path;
    ofs_.open(path_, std::ios::out | std::ios::app);
    if (!ofs_.is_open()) {
        throw std::runtime_error("Failed to open new WAL file: " + path_);
    }
    
    last_flush_ = std::chrono::steady_clock::now();
    std::cout << "[WAL] New file opened: " << path_ << std::endl;
}