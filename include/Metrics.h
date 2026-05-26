#pragma once

#include <cstdint>
#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>
#include <string>

// Simple struct to hold performance metrics
struct EngineMetrics {
    // Orders
    uint64_t orders_submitted = 0;
    uint64_t orders_matched = 0;

    // Latency (in microseconds)
    std::vector<long long> latencies; // Stores individual latencies

    // Throughput (orders/second)
    double throughput_ops = 0.0;

    // Internal engine state
    uint64_t resting_orders = 0;
    uint64_t total_market_value = 0; // Sum of price*quantity for all resting orders

    // Timestamps for throughput calculation
    std::chrono::steady_clock::time_point last_reset_time;

    EngineMetrics() {
        reset();
    }

    void reset() {
        orders_submitted = 0;
        orders_matched = 0;
        latencies.clear();
        latencies.reserve(100000); // Pre-allocate for typical test sizes
        throughput_ops = 0.0;
        resting_orders = 0;
        total_market_value = 0;
        last_reset_time = std::chrono::steady_clock::now();
    }

    void add_latency(long long us) {
        if (us >= 0) { // Only add positive latencies
            latencies.push_back(us);
        }
    }

    // Call this periodically to calculate current throughput
    void calculate_throughput() {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - last_reset_time).count();
        if (duration > 0) {
            // Calculate submitted orders per second since last reset
            // This is a simplified throughput, might need more advanced logic
            // e.g., orders_submitted - prev_submitted / duration
            // For now, let's calculate based on overall orders_submitted and total time if needed.
            // Or calculate instantaneous throughput over the reporting period.
            // Let's make this simple: throughput is number of orders matched / time duration of benchmark
        }
    }

    // Helper to calculate percentiles
    long long get_latency_p50() const {
        if (latencies.empty()) return 0;
        std::vector<long long> sorted = latencies;
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() / 2];
    }

    long long get_latency_p99() const {
        if (latencies.empty()) return 0;
        std::vector<long long> sorted = latencies;
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() * 99 / 100];
    }
};
