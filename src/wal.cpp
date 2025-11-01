// FILE: src/wal.cpp
#include "../include/wal.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <filesystem>
#include <queue> // Include queue

WAL::WAL(const std::string &path)
    : path_(path), running_(false) {
    
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
    
    // Start the writer thread
    running_ = true;
    writer_thread_ = std::thread(&WAL::writer_thread_loop, this);
    std::cout << "[WAL] Initialized. Asynchronous writer thread started." << std::endl;
}

WAL::~WAL() {
    stop(); // Ensure thread is stopped and queue is flushed
    if (ofs_.is_open()) {
        ofs_.close();
    }
    std::cout << "[WAL] Closed. Total entries: " << total_entries_.load() << std::endl;
}

void WAL::stop() {
    if (running_.exchange(false)) { // Ensure this runs only once
        std::cout << "[WAL] Stopping writer thread..." << std::endl;
        cv_.notify_one(); // Wake up the thread
        if (writer_thread_.joinable()) {
            writer_thread_.join(); // Wait for it to finish
        }
        std::cout << "[WAL] Writer thread stopped." << std::endl;
    }
}

void WAL::append_json(const nlohmann::json &j) {
    if (!running_.load()) return; // Don't accept new entries if stopping

    // Serialize outside the lock (fast)
    std::string line = j.dump();

    {
        // Lock, push to queue (very fast)
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push(std::move(line));
    }

    // Increment total and signal writer thread
    total_entries_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
}

void WAL::writer_thread_loop() {
    std::queue<std::string> local_queue;

    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            // Wait until queue has data OR we are stopping
            cv_.wait(lk, [&]{ return !queue_.empty() || !running_.load(); });

            // If we are stopping and the queue is empty, we can exit
            if (!running_.load() && queue_.empty()) {
                break;
            }

            // Atomically swap the global queue with our empty local one
            std::swap(local_queue, queue_);
        } // Lock is released here

        // Now, write all entries from the local queue (no lock held)
        while (!local_queue.empty()) {
            ofs_ << local_queue.front() << '\n';
            local_queue.pop();
        }

        // Flush the entire batch to disk
        if (ofs_.is_open()) {
            ofs_.flush();
        }
    }

    // --- Shutdown sequence ---
    // We've exited the loop, but there might be stragglers
    // that arrived between running_=false and the loop check.
    std::cout << "[WAL] Writer thread shutting down. Draining final queue..." << std::endl;
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::swap(local_queue, queue_); // Get any final items
    }
    
    while (!local_queue.empty()) {
        ofs_ << local_queue.front() << '\n';
        local_queue.pop();
    }
    if (ofs_.is_open()) {
        ofs_.flush(); // Final flush
    }
    std::cout << "[WAL] Writer thread shutdown complete." << std::endl;
}

size_t WAL::pending_writes() {
    std::lock_guard<std::mutex> lk(mu_);
    return queue_.size();
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

void WAL::flush_internal() {
    // This function must be called with the mutex already locked
    if (ofs_.is_open()) {
        ofs_.flush();
    }
}

void WAL::flush() {
    // Public flush just flushes the OS buffer
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
    // Lock to ensure writer thread is not using ofs_
    std::lock_guard<std::mutex> lk(mu_);
    
    // Flush current data
    flush_internal();
    
    // Close current file
    if (ofs_.is_open()) {
        ofs_.close();
    }
    
    // Rename old file
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
    
    std::cout << "[WAL] New file opened: " << path_ << std::endl;
}
