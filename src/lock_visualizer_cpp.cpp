#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <memory>
#include <string>
#include <string_view>
#include <map>
#include <condition_variable>
#include <future>
#include <functional>
#include <array>
#include <unistd.h>

namespace LockVisualization {

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = Clock::time_point;

enum class LockType {
    MUTEX,
    RECURSIVE_MUTEX,
    SHARED_MUTEX,
    SPINLOCK,
    ADAPTIVE_MUTEX
};

enum class WorkloadPattern {
    BALANCED,
    READ_HEAVY,
    WRITE_HEAVY,
    THUNDERING_HERD,
    RANDOM,
    BURSTY
};

// Type traits for lockable types (C++17 compatible)
template<typename Mutex>
struct is_lockable {
    template<typename T>
    static auto test(int) -> decltype(
        std::declval<T&>().lock(),
        std::declval<T&>().unlock(),
        std::declval<T&>().try_lock(),
        std::true_type{});
    template<typename>
    static std::false_type test(...);
    
    static constexpr bool value = decltype(test<Mutex>(0))::value;
};

template<typename SharedMutex>
struct is_shared_lockable {
    template<typename T>
    static auto test(int) -> decltype(
        std::declval<T&>().lock(),
        std::declval<T&>().unlock(),
        std::declval<T&>().try_lock(),
        std::declval<T&>().lock_shared(),
        std::declval<T&>().unlock_shared(),
        std::declval<T&>().try_lock_shared(),
        std::true_type{});
    template<typename>
    static std::false_type test(...);
    
    static constexpr bool value = decltype(test<SharedMutex>(0))::value;
};

class SpinLock {
private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    
public:
    void lock() {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    
    void unlock() {
        flag_.clear(std::memory_order_release);
    }
    
    bool try_lock() {
        return !flag_.test_and_set(std::memory_order_acquire);
    }
};

class AdaptiveMutex {
private:
    std::atomic<int> state_{0}; // 0: unlocked, 1: locked
    std::mutex fallback_mutex_;
    static constexpr int SPIN_LIMIT = 100;
    
public:
    void lock() {
        int spin_count = 0;
        int expected = 0;
        
        // Try spinning first
        while (spin_count < SPIN_LIMIT) {
            if (state_.compare_exchange_weak(expected, 1, std::memory_order_acquire)) {
                return;
            }
            expected = 0;
            std::this_thread::yield();
            ++spin_count;
        }
        
        // Fall back to blocking mutex
        fallback_mutex_.lock();
        state_.store(1, std::memory_order_release);
    }
    
    void unlock() {
        state_.store(0, std::memory_order_release);
        try {
            fallback_mutex_.unlock();
        } catch (...) {
            // Mutex wasn't locked by fallback
        }
    }
    
    bool try_lock() {
        int expected = 0;
        return state_.compare_exchange_strong(expected, 1, std::memory_order_acquire);
    }
};

struct ThreadStatistics {
    std::atomic<uint64_t> lock_acquisitions{0};
    std::atomic<uint64_t> lock_contentions{0};
    std::atomic<uint64_t> read_operations{0};
    std::atomic<uint64_t> write_operations{0};
    std::atomic<double> total_wait_time{0.0};
    std::atomic<double> max_wait_time{0.0};
    std::atomic<double> total_hold_time{0.0};
    
    int thread_id{0};
    WorkloadPattern pattern{WorkloadPattern::BALANCED};
    
    std::vector<double> wait_times;
    std::mutex wait_times_mutex;
    
    void add_wait_time(double time) {
        double old_wait = total_wait_time.load(std::memory_order_relaxed);
        while (!total_wait_time.compare_exchange_weak(old_wait, old_wait + time, std::memory_order_relaxed)) {
            // Keep trying
        }
        
        double current_max = max_wait_time.load(std::memory_order_relaxed);
        while (time > current_max && 
               !max_wait_time.compare_exchange_weak(current_max, time, std::memory_order_relaxed)) {
            // Keep trying until we successfully update the max
        }
        
        std::lock_guard<std::mutex> lock(wait_times_mutex);
        wait_times.push_back(time);
    }
    
    void add_hold_time(double time) {
        double old_hold = total_hold_time.load(std::memory_order_relaxed);
        while (!total_hold_time.compare_exchange_weak(old_hold, old_hold + time, std::memory_order_relaxed)) {
            // Keep trying
        }
    }
    
    double get_average_wait_time() const {
        uint64_t acquisitions = lock_acquisitions.load();
        return acquisitions > 0 ? total_wait_time.load() / acquisitions : 0.0;
    }
    
    double get_contention_rate() const {
        uint64_t acquisitions = lock_acquisitions.load();
        return acquisitions > 0 ? 
            static_cast<double>(lock_contentions.load()) / acquisitions : 0.0;
    }
    
    std::vector<double> get_wait_time_percentiles() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(wait_times_mutex));
        if (wait_times.empty()) return {};
        
        std::vector<double> sorted_times = wait_times;
        std::sort(sorted_times.begin(), sorted_times.end());
        
        std::vector<double> percentiles;
        std::array<double, 5> p_values = {0.5, 0.75, 0.9, 0.95, 0.99};
        
        for (double p : p_values) {
            size_t index = static_cast<size_t>(p * sorted_times.size());
            if (index >= sorted_times.size()) index = sorted_times.size() - 1;
            percentiles.push_back(sorted_times[index]);
        }
        
        return percentiles;
    }
};

template<typename LockT>
class LockContentionAnalyzer {
private:
    std::vector<std::unique_ptr<ThreadStatistics>> thread_stats_;
    std::atomic<bool> stop_flag_{false};
    LockT shared_lock_;
    int num_threads_;
    int duration_seconds_;
    WorkloadPattern pattern_;
    bool real_time_visualization_;
    
    // Shared data protected by the lock
    std::atomic<uint64_t> shared_counter_{0};
    std::vector<int> shared_data_;
    
public:
    LockContentionAnalyzer(int threads, int duration, WorkloadPattern pattern, bool visualization)
        : num_threads_(threads), duration_seconds_(duration), 
          pattern_(pattern), real_time_visualization_(visualization) {
        
        thread_stats_.reserve(num_threads_);
        for (int i = 0; i < num_threads_; ++i) {
            auto stats = std::make_unique<ThreadStatistics>();
            stats->thread_id = i;
            stats->pattern = pattern_;
            thread_stats_.push_back(std::move(stats));
        }
        
        shared_data_.resize(10000, 0);
    }
    
    void run_analysis() {
        std::cout << "Lock Contention Visualizer (C++)\n";
        std::cout << "================================\n";
        std::cout << "Lock type: " << get_lock_type_name() << "\n";
        std::cout << "Threads: " << num_threads_ << "\n";
        std::cout << "Duration: " << duration_seconds_ << " seconds\n";
        std::cout << "Pattern: " << get_pattern_name() << "\n";
        std::cout << "Real-time visualization: " << (real_time_visualization_ ? "enabled" : "disabled") << "\n\n";
        
        std::vector<std::thread> threads;
        threads.reserve(num_threads_);
        
        auto start_time = Clock::now();
        
        // Start visualization thread if enabled
        std::future<void> viz_future;
        if (real_time_visualization_) {
            auto viz_promise = std::promise<void>();
            viz_future = viz_promise.get_future();
            std::thread viz_thread(&LockContentionAnalyzer::visualization_thread, this, std::move(viz_promise));
            viz_thread.detach();
        }
        
        // Start worker threads
        for (int i = 0; i < num_threads_; ++i) {
            threads.emplace_back(&LockContentionAnalyzer::worker_thread, this, i);
        }
        
        // Run for specified duration
        std::this_thread::sleep_for(std::chrono::seconds(duration_seconds_));
        stop_flag_.store(true);
        
        // Wait for all threads to complete
        for (auto& t : threads) {
            t.join();
        }
        
        auto end_time = Clock::now();
        double actual_duration = Duration(end_time - start_time).count();
        
        analyze_results(actual_duration);
    }
    
private:
    std::string get_lock_type_name() const {
        if constexpr (std::is_same_v<LockT, std::mutex>) {
            return "std::mutex";
        } else if constexpr (std::is_same_v<LockT, std::recursive_mutex>) {
            return "std::recursive_mutex";
        } else if constexpr (std::is_same_v<LockT, std::shared_mutex>) {
            return "std::shared_mutex";
        } else if constexpr (std::is_same_v<LockT, SpinLock>) {
            return "SpinLock";
        } else if constexpr (std::is_same_v<LockT, AdaptiveMutex>) {
            return "AdaptiveMutex";
        } else {
            return "Unknown";
        }
    }
    
    std::string get_pattern_name() const {
        switch (pattern_) {
            case WorkloadPattern::BALANCED: return "Balanced";
            case WorkloadPattern::READ_HEAVY: return "Read-heavy";
            case WorkloadPattern::WRITE_HEAVY: return "Write-heavy";
            case WorkloadPattern::THUNDERING_HERD: return "Thundering herd";
            case WorkloadPattern::RANDOM: return "Random";
            case WorkloadPattern::BURSTY: return "Bursty";
            default: return "Unknown";
        }
    }
    
    void worker_thread(int thread_id) {
        auto& stats = *thread_stats_[thread_id];
        std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count() + thread_id);
        std::uniform_real_distribution<> real_dist(0.0, 1.0);
        std::uniform_int_distribution<> int_dist(0, shared_data_.size() - 1);
        
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            bool should_read = determine_operation_type(rng, real_dist);
            
            if constexpr (is_shared_lockable<LockT>::value) {
                if (should_read) {
                    perform_read_operation(stats, rng, int_dist);
                } else {
                    perform_write_operation(stats, rng, int_dist);
                }
            } else {
                perform_write_operation(stats, rng, int_dist);
            }
            
            // Add some variability based on pattern
            add_pattern_delay(rng, real_dist);
        }
    }
    
    bool determine_operation_type(std::mt19937& rng, std::uniform_real_distribution<>& dist) {
        double r = dist(rng);
        
        switch (pattern_) {
            case WorkloadPattern::READ_HEAVY:
                return r < 0.8; // 80% reads
            case WorkloadPattern::WRITE_HEAVY:
                return r < 0.2; // 20% reads
            case WorkloadPattern::BALANCED:
                return r < 0.5; // 50% reads
            case WorkloadPattern::THUNDERING_HERD:
                return r < 0.1; // Mostly writes to create contention
            case WorkloadPattern::RANDOM:
                return r < 0.6; // Slightly read-favored
            case WorkloadPattern::BURSTY:
                return r < 0.4; // Moderately read-favored
            default:
                return r < 0.5;
        }
    }
    
    void perform_read_operation(ThreadStatistics& stats, std::mt19937& rng, 
                               std::uniform_int_distribution<>& int_dist) {
        if constexpr (!is_shared_lockable<LockT>::value) {
            perform_write_operation(stats, rng, int_dist);
            return;
        }
        
        auto wait_start = Clock::now();
        bool contended = !shared_lock_.try_lock_shared();
        
        if (contended) {
            shared_lock_.lock_shared();
            stats.lock_contentions.fetch_add(1, std::memory_order_relaxed);
        }
        
        auto lock_acquired = Clock::now();
        auto wait_time = Duration(lock_acquired - wait_start).count();
        stats.add_wait_time(wait_time);
        stats.lock_acquisitions.fetch_add(1, std::memory_order_relaxed);
        
        // Simulate read work
        int index = int_dist(rng);
        volatile int value = shared_data_[index];
        (void)value; // Suppress unused variable warning
        
        // Hold the lock for some time
        auto hold_start = Clock::now();
        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 100));
        
        shared_lock_.unlock_shared();
        
        auto hold_time = Duration(Clock::now() - hold_start).count();
        stats.add_hold_time(hold_time);
        stats.read_operations.fetch_add(1, std::memory_order_relaxed);
    }
    
    void perform_write_operation(ThreadStatistics& stats, std::mt19937& rng, 
                                std::uniform_int_distribution<>& int_dist) {
        auto wait_start = Clock::now();
        bool contended = !shared_lock_.try_lock();
        
        if (contended) {
            shared_lock_.lock();
            stats.lock_contentions.fetch_add(1, std::memory_order_relaxed);
        }
        
        auto lock_acquired = Clock::now();
        auto wait_time = Duration(lock_acquired - wait_start).count();
        stats.add_wait_time(wait_time);
        stats.lock_acquisitions.fetch_add(1, std::memory_order_relaxed);
        
        // Simulate write work
        int index = int_dist(rng);
        shared_data_[index] = rng();
        shared_counter_.fetch_add(1, std::memory_order_relaxed);
        
        // Hold the lock for some time
        auto hold_start = Clock::now();
        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 200));
        
        shared_lock_.unlock();
        
        auto hold_time = Duration(Clock::now() - hold_start).count();
        stats.add_hold_time(hold_time);
        stats.write_operations.fetch_add(1, std::memory_order_relaxed);
    }
    
    void add_pattern_delay(std::mt19937& rng, std::uniform_real_distribution<>& dist) {
        switch (pattern_) {
            case WorkloadPattern::THUNDERING_HERD:
                // Very short delays to maximize contention
                if (dist(rng) < 0.1) {
                    std::this_thread::sleep_for(std::chrono::microseconds(rng() % 10));
                }
                break;
            case WorkloadPattern::BURSTY:
                // Occasional longer delays to create bursts
                if (dist(rng) < 0.05) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 10));
                }
                break;
            case WorkloadPattern::RANDOM:
                // Random delays
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 1000));
                break;
            default:
                // Small random delay
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 100));
                break;
        }
    }
    
    void visualization_thread(std::promise<void> promise) {
        promise.set_value(); // Signal that visualization thread started
        
        const int update_interval_ms = 1000;
        
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(update_interval_ms));
            
            // Clear screen and move cursor to top
            std::cout << "\033[2J\033[H";
            
            std::cout << "Real-time Lock Contention Monitor\n";
            std::cout << "=================================\n";
            
            uint64_t total_acquisitions = 0;
            uint64_t total_contentions = 0;
            double total_wait_time = 0.0;
            
            for (const auto& stats : thread_stats_) {
                total_acquisitions += stats->lock_acquisitions.load();
                total_contentions += stats->lock_contentions.load();
                total_wait_time += stats->total_wait_time.load();
            }
            
            double contention_rate = total_acquisitions > 0 ? 
                static_cast<double>(total_contentions) / total_acquisitions : 0.0;
            double avg_wait_time = total_acquisitions > 0 ? 
                total_wait_time / total_acquisitions : 0.0;
            
            std::cout << "Total acquisitions: " << total_acquisitions << "\n";
            std::cout << "Total contentions: " << total_contentions << "\n";
            std::cout << "Contention rate: " << std::fixed << std::setprecision(2) 
                      << contention_rate * 100 << "%\n";
            std::cout << "Average wait time: " << avg_wait_time * 1e6 << " μs\n";
            std::cout << "Shared counter: " << shared_counter_.load() << "\n\n";
            
            // Print per-thread statistics
            std::cout << "Per-thread statistics:\n";
            for (const auto& stats : thread_stats_) {
                uint64_t acquisitions = stats->lock_acquisitions.load();
                uint64_t contentions = stats->lock_contentions.load();
                double thread_contention_rate = acquisitions > 0 ? 
                    static_cast<double>(contentions) / acquisitions : 0.0;
                
                std::cout << "Thread " << std::setw(2) << stats->thread_id 
                          << ": " << std::setw(8) << acquisitions << " acq, "
                          << std::setw(6) << contentions << " cont ("
                          << std::fixed << std::setprecision(1)
                          << thread_contention_rate * 100 << "%)\n";
            }
        }
    }
    
    void analyze_results(double duration) {
        std::cout << "\n=== Final Analysis ===\n";
        
        uint64_t total_acquisitions = 0;
        uint64_t total_contentions = 0;
        uint64_t total_reads = 0;
        uint64_t total_writes = 0;
        double total_wait_time = 0.0;
        double max_wait_time = 0.0;
        
        for (const auto& stats : thread_stats_) {
            total_acquisitions += stats->lock_acquisitions.load();
            total_contentions += stats->lock_contentions.load();
            total_reads += stats->read_operations.load();
            total_writes += stats->write_operations.load();
            total_wait_time += stats->total_wait_time.load();
            max_wait_time = std::max(max_wait_time, stats->max_wait_time.load());
        }
        
        double contention_rate = total_acquisitions > 0 ? 
            static_cast<double>(total_contentions) / total_acquisitions : 0.0;
        double avg_wait_time = total_acquisitions > 0 ? 
            total_wait_time / total_acquisitions : 0.0;
        double throughput = (total_reads + total_writes) / duration;
        
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "Duration: " << duration << " seconds\n";
        std::cout << "Total operations: " << (total_reads + total_writes) << "\n";
        std::cout << "  Read operations: " << total_reads << "\n";
        std::cout << "  Write operations: " << total_writes << "\n";
        std::cout << "Lock acquisitions: " << total_acquisitions << "\n";
        std::cout << "Lock contentions: " << total_contentions << "\n";
        std::cout << "Contention rate: " << std::setprecision(2) << contention_rate * 100 << "%\n";
        std::cout << "Average wait time: " << std::setprecision(3) << avg_wait_time * 1e6 << " μs\n";
        std::cout << "Maximum wait time: " << max_wait_time * 1e6 << " μs\n";
        std::cout << "Throughput: " << std::setprecision(0) << throughput << " ops/sec\n";
        
        print_detailed_statistics();
        provide_recommendations(contention_rate, avg_wait_time);
    }
    
    void print_detailed_statistics() {
        std::cout << "\n=== Per-Thread Detailed Statistics ===\n";
        std::cout << std::left << std::setw(8) << "Thread"
                  << std::setw(12) << "Acquisitions"
                  << std::setw(12) << "Contentions"
                  << std::setw(12) << "Cont. Rate"
                  << std::setw(12) << "Avg Wait"
                  << std::setw(12) << "Max Wait"
                  << std::setw(10) << "Reads"
                  << std::setw(10) << "Writes" << "\n";
        std::cout << std::string(88, '-') << "\n";
        
        for (const auto& stats : thread_stats_) {
            uint64_t acquisitions = stats->lock_acquisitions.load();
            uint64_t contentions = stats->lock_contentions.load();
            double contention_rate = acquisitions > 0 ? 
                static_cast<double>(contentions) / acquisitions : 0.0;
            
            std::cout << std::left << std::setw(8) << stats->thread_id
                      << std::setw(12) << acquisitions
                      << std::setw(12) << contentions
                      << std::fixed << std::setprecision(1)
                      << std::setw(12) << contention_rate * 100
                      << std::setprecision(2)
                      << std::setw(12) << stats->get_average_wait_time() * 1e6
                      << std::setw(12) << stats->max_wait_time.load() * 1e6
                      << std::setw(10) << stats->read_operations.load()
                      << std::setw(10) << stats->write_operations.load() << "\n";
        }
        
        // Print wait time percentiles for first thread as example
        if (!thread_stats_.empty()) {
            auto percentiles = thread_stats_[0]->get_wait_time_percentiles();
            if (!percentiles.empty()) {
                std::cout << "\nWait Time Percentiles (Thread 0, μs):\n";
                std::array<std::string, 5> labels = {"50th", "75th", "90th", "95th", "99th"};
                for (size_t i = 0; i < percentiles.size() && i < labels.size(); ++i) {
                    std::cout << "  " << labels[i] << ": " 
                              << std::fixed << std::setprecision(2) 
                              << percentiles[i] * 1e6 << " μs\n";
                }
            }
        }
    }
    
    void provide_recommendations(double contention_rate, double avg_wait_time) {
        std::cout << "\n=== Performance Recommendations ===\n";
        
        if (contention_rate > 0.5) {
            std::cout << "⚠️  HIGH CONTENTION DETECTED (" << std::fixed << std::setprecision(1) 
                      << contention_rate * 100 << "%)\n";
            std::cout << "Recommendations:\n";
            std::cout << "  - Consider using lock-free data structures\n";
            std::cout << "  - Reduce critical section size\n";
            std::cout << "  - Use finer-grained locking\n";
            std::cout << "  - Consider read-write locks for read-heavy workloads\n";
        } else if (contention_rate > 0.2) {
            std::cout << "⚠️  MODERATE CONTENTION (" << contention_rate * 100 << "%)\n";
            std::cout << "Recommendations:\n";
            std::cout << "  - Monitor performance under increased load\n";
            std::cout << "  - Consider optimizing critical sections\n";
        } else {
            std::cout << "✅ LOW CONTENTION (" << contention_rate * 100 << "%)\n";
            std::cout << "Current lock strategy appears effective.\n";
        }
        
        if (avg_wait_time > 1e-3) { // > 1ms
            std::cout << "⚠️  HIGH AVERAGE WAIT TIME (" 
                      << std::fixed << std::setprecision(2) << avg_wait_time * 1e3 << " ms)\n";
            std::cout << "Consider reducing work done while holding locks.\n";
        }
        
        std::cout << "\nLock Type Recommendations:\n";
        if constexpr (std::is_same_v<LockT, std::mutex>) {
            std::cout << "  - Current: std::mutex (good general purpose)\n";
            if (contention_rate > 0.3) {
                std::cout << "  - Consider: std::shared_mutex for read-heavy workloads\n";
                std::cout << "  - Consider: SpinLock for very short critical sections\n";
            }
        } else if constexpr (std::is_same_v<LockT, SpinLock>) {
            std::cout << "  - Current: SpinLock (good for short critical sections)\n";
            if (contention_rate > 0.4) {
                std::cout << "  - Consider: std::mutex for longer critical sections\n";
            }
        } else if constexpr (std::is_same_v<LockT, std::shared_mutex>) {
            std::cout << "  - Current: std::shared_mutex (good for read-heavy workloads)\n";
            if (total_reads < total_writes) {
                std::cout << "  - Consider: std::mutex for write-heavy workloads\n";
            }
        }
    }
    
    uint64_t total_reads = 0;
    uint64_t total_writes = 0;
};

template<LockType lock_type>
void run_lock_analysis(int threads, int duration, WorkloadPattern pattern, bool visualization) {
    if constexpr (lock_type == LockType::MUTEX) {
        LockContentionAnalyzer<std::mutex> analyzer(threads, duration, pattern, visualization);
        analyzer.run_analysis();
    } else if constexpr (lock_type == LockType::RECURSIVE_MUTEX) {
        LockContentionAnalyzer<std::recursive_mutex> analyzer(threads, duration, pattern, visualization);
        analyzer.run_analysis();
    } else if constexpr (lock_type == LockType::SHARED_MUTEX) {
        LockContentionAnalyzer<std::shared_mutex> analyzer(threads, duration, pattern, visualization);
        analyzer.run_analysis();
    } else if constexpr (lock_type == LockType::SPINLOCK) {
        LockContentionAnalyzer<SpinLock> analyzer(threads, duration, pattern, visualization);
        analyzer.run_analysis();
    } else if constexpr (lock_type == LockType::ADAPTIVE_MUTEX) {
        LockContentionAnalyzer<AdaptiveMutex> analyzer(threads, duration, pattern, visualization);
        analyzer.run_analysis();
    }
}

void print_usage(const std::string& prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n";
    std::cout << "Options:\n";
    std::cout << "  -t <threads>  Number of threads (default: 8)\n";
    std::cout << "  -d <seconds>  Duration of test (default: 10)\n";
    std::cout << "  -l <type>     Lock type: mutex, recursive, shared, spin, adaptive (default: mutex)\n";
    std::cout << "  -w <pattern>  Workload pattern: balanced, read, write, thunder, random, bursty (default: balanced)\n";
    std::cout << "  -z            Enable real-time visualization\n";
    std::cout << "  -h            Show this help message\n";
    std::cout << "\nAnalyzes lock contention patterns and provides performance recommendations.\n";
}

} // namespace LockVisualization

int main(int argc, char* argv[]) {
    using namespace LockVisualization;
    
    int num_threads = 8;
    int duration = 10;
    LockType lock_type = LockType::MUTEX;
    WorkloadPattern pattern = WorkloadPattern::BALANCED;
    bool visualization = false;
    
    int opt;
    while ((opt = getopt(argc, argv, "t:d:l:w:zh")) != -1) {
        switch (opt) {
            case 't':
                num_threads = std::stoi(optarg);
                if (num_threads <= 0 || num_threads > 64) {
                    std::cerr << "Error: thread count must be between 1 and 64\n";
                    return 1;
                }
                break;
            case 'd':
                duration = std::stoi(optarg);
                if (duration <= 0) {
                    std::cerr << "Error: duration must be positive\n";
                    return 1;
                }
                break;
            case 'l':
                if (std::string_view(optarg) == "mutex") lock_type = LockType::MUTEX;
                else if (std::string_view(optarg) == "recursive") lock_type = LockType::RECURSIVE_MUTEX;
                else if (std::string_view(optarg) == "shared") lock_type = LockType::SHARED_MUTEX;
                else if (std::string_view(optarg) == "spin") lock_type = LockType::SPINLOCK;
                else if (std::string_view(optarg) == "adaptive") lock_type = LockType::ADAPTIVE_MUTEX;
                else {
                    std::cerr << "Error: unknown lock type '" << optarg << "'\n";
                    return 1;
                }
                break;
            case 'w':
                if (std::string_view(optarg) == "balanced") pattern = WorkloadPattern::BALANCED;
                else if (std::string_view(optarg) == "read") pattern = WorkloadPattern::READ_HEAVY;
                else if (std::string_view(optarg) == "write") pattern = WorkloadPattern::WRITE_HEAVY;
                else if (std::string_view(optarg) == "thunder") pattern = WorkloadPattern::THUNDERING_HERD;
                else if (std::string_view(optarg) == "random") pattern = WorkloadPattern::RANDOM;
                else if (std::string_view(optarg) == "bursty") pattern = WorkloadPattern::BURSTY;
                else {
                    std::cerr << "Error: unknown workload pattern '" << optarg << "'\n";
                    return 1;
                }
                break;
            case 'z':
                visualization = true;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }
    
    try {
        switch (lock_type) {
            case LockType::MUTEX:
                run_lock_analysis<LockType::MUTEX>(num_threads, duration, pattern, visualization);
                break;
            case LockType::RECURSIVE_MUTEX:
                run_lock_analysis<LockType::RECURSIVE_MUTEX>(num_threads, duration, pattern, visualization);
                break;
            case LockType::SHARED_MUTEX:
                run_lock_analysis<LockType::SHARED_MUTEX>(num_threads, duration, pattern, visualization);
                break;
            case LockType::SPINLOCK:
                run_lock_analysis<LockType::SPINLOCK>(num_threads, duration, pattern, visualization);
                break;
            case LockType::ADAPTIVE_MUTEX:
                run_lock_analysis<LockType::ADAPTIVE_MUTEX>(num_threads, duration, pattern, visualization);
                break;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}