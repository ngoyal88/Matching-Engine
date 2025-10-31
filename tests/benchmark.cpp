// ============================================================================
// FILE: tests/benchmark.cpp (Comprehensive Benchmarking Tool)
// ============================================================================
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <sstream>
#include "../vendor/httplib.h"
#include "../vendor/json.hpp"

using json = nlohmann::json;

struct BenchmarkConfig {
    std::string base_url = "http://localhost:8080";
    int num_threads = 4;
    int orders_per_thread = 1000;
    bool mixed_operations = true;  // Mix of buys/sells
};

struct BenchmarkResults {
    std::atomic<uint64_t> total_orders{0};
    std::atomic<uint64_t> successful_orders{0};
    std::atomic<uint64_t> failed_orders{0};
    std::atomic<uint64_t> total_latency_ns{0};
    std::atomic<uint64_t> min_latency_ns{UINT64_MAX};
    std::atomic<uint64_t> max_latency_ns{0};
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
};

class OrderBenchmark {
public:
    OrderBenchmark(const BenchmarkConfig &config) : config_(config) {}
    
    void run() {
        std::cout << "\n========================================\n";
        std::cout << "  Order Submission Benchmark\n";
        std::cout << "========================================\n";
        std::cout << "Threads: " << config_.num_threads << "\n";
        std::cout << "Orders per thread: " << config_.orders_per_thread << "\n";
        std::cout << "Total orders: " << config_.num_threads * config_.orders_per_thread << "\n";
        std::cout << "========================================\n\n";
        
        results_.start_time = std::chrono::steady_clock::now();
        
        std::vector<std::thread> threads;
        for (int i = 0; i < config_.num_threads; ++i) {
            threads.emplace_back(&OrderBenchmark::worker_thread, this, i);
        }
        
        for (auto &t : threads) {
            t.join();
        }
        
        results_.end_time = std::chrono::steady_clock::now();
        
        print_results();
    }
    
private:
    BenchmarkConfig config_;
    BenchmarkResults results_;
    
    void worker_thread(int thread_id) {
        httplib::Client cli("http://localhost:8080");
        cli.set_connection_timeout(10, 0);
        cli.set_read_timeout(10, 0);
        cli.set_write_timeout(10, 0);
        cli.set_keep_alive(true);
        
        std::cout << "Thread " << thread_id << " started\n" << std::flush;
        
        std::random_device rd;
        std::mt19937 gen(rd() + thread_id);
        std::uniform_real_distribution<> price_dist(49000.0, 51000.0);
        std::uniform_real_distribution<> qty_dist(0.1, 2.0);
        std::uniform_int_distribution<> side_dist(0, 1);
        
        for (int i = 0; i < config_.orders_per_thread; ++i) {
            double price = price_dist(gen);
            double quantity = qty_dist(gen);
            std::string side = side_dist(gen) == 0 ? "buy" : "sell";
            
            json order = {
                {"symbol", "BTC-USDT"},
                {"order_type", "limit"},
                {"side", side},
                {"quantity", quantity},
                {"price", price}
            };
            
            std::string payload = order.dump();
            
            auto start = std::chrono::high_resolution_clock::now();
            auto res = cli.Post("/orders", payload, "application/json");
            auto end = std::chrono::high_resolution_clock::now();
            
            auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            
            results_.total_orders++;
            results_.total_latency_ns += latency_ns;
            
            // Update min/max latency
            uint64_t current_min = results_.min_latency_ns.load();
            while (latency_ns < current_min && 
                   !results_.min_latency_ns.compare_exchange_weak(current_min, latency_ns));
            
            uint64_t current_max = results_.max_latency_ns.load();
            while (latency_ns > current_max && 
                   !results_.max_latency_ns.compare_exchange_weak(current_max, latency_ns));
            
            if (res && res->status == 200) {
                results_.successful_orders++;
            } else {
                results_.failed_orders++;
                if (!res) {
                    std::cerr << "Thread " << thread_id << ": Request " << i << " failed (no response)\n" << std::flush;
                } else {
                    std::cerr << "Thread " << thread_id << ": Request " << i << " failed (status " << res->status << ")\n" << std::flush;
                }
            }
            
            // Progress indicator - more frequent for visibility
            if ((i + 1) % 50 == 0 || i == 0) {
                std::cout << "Thread " << thread_id << ": " << (i + 1) << "/" << config_.orders_per_thread << " orders sent\n" << std::flush;
            }
        }
        
        std::cout << "Thread " << thread_id << " completed\n" << std::flush;
    }
    
    void print_results() {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            results_.end_time - results_.start_time
        );
        
        double duration_sec = duration.count() / 1000.0;
        double avg_latency_ms = (results_.total_latency_ns.load() / (double)results_.total_orders.load()) / 1000000.0;
        double min_latency_ms = results_.min_latency_ns.load() / 1000000.0;
        double max_latency_ms = results_.max_latency_ns.load() / 1000000.0;
        double throughput = results_.total_orders.load() / duration_sec;
        
        std::cout << "\n========================================\n";
        std::cout << "  Benchmark Results\n";
        std::cout << "========================================\n";
        std::cout << "Total Orders:      " << results_.total_orders.load() << "\n";
        std::cout << "Successful:        " << results_.successful_orders.load() << "\n";
        std::cout << "Failed:            " << results_.failed_orders.load() << "\n";
        std::cout << "Success Rate:      " << (results_.successful_orders.load() * 100.0 / results_.total_orders.load()) << "%\n";
        std::cout << "----------------------------------------\n";
        std::cout << "Duration:          " << duration_sec << " seconds\n";
        std::cout << "Throughput:        " << throughput << " orders/sec\n";
        std::cout << "----------------------------------------\n";
        std::cout << "Avg Latency:       " << avg_latency_ms << " ms\n";
        std::cout << "Min Latency:       " << min_latency_ms << " ms\n";
        std::cout << "Max Latency:       " << max_latency_ms << " ms\n";
        std::cout << "========================================\n\n";
    }
};

int main(int argc, char **argv) {
    BenchmarkConfig config;
    
    if (argc > 1) config.num_threads = std::stoi(argv[1]);
    if (argc > 2) config.orders_per_thread = std::stoi(argv[2]);
    
    std::cout << "Usage: " << argv[0] << " [threads=4] [orders_per_thread=1000]\n";
    std::cout << "Example: " << argv[0] << " 8 1000\n\n";
    
    OrderBenchmark benchmark(config);
    benchmark.run();
    
    return 0;
}