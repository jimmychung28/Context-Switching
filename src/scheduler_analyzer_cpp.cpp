#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <fstream>
#include <random>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <functional>
#include <future>
#include <unistd.h>
#include <sys/resource.h>

namespace SchedulerAnalysis {

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = Clock::time_point;

enum class WorkloadType {
    CPU_INTENSIVE,
    IO_INTENSIVE,
    MIXED,
    MEMORY_INTENSIVE
};

struct ThreadStats {
    std::atomic<uint64_t> cpu_operations{0};
    std::atomic<uint64_t> io_operations{0};
    std::atomic<uint64_t> memory_operations{0};
    std::atomic<double> cpu_time{0.0};
    std::atomic<double> wall_time{0.0};
    std::atomic<uint64_t> context_switches{0};
    std::atomic<uint64_t> voluntary_switches{0};
    std::atomic<uint64_t> involuntary_switches{0};
    
    int thread_id{0};
    int nice_value{0};
    WorkloadType workload_type{WorkloadType::CPU_INTENSIVE};
    double fairness_score{0.0};
    
    void reset() {
        cpu_operations = 0;
        io_operations = 0;
        memory_operations = 0;
        cpu_time = 0.0;
        wall_time = 0.0;
        context_switches = 0;
        voluntary_switches = 0;
        involuntary_switches = 0;
        fairness_score = 0.0;
    }
};

struct ThreadStatsCopy {
    uint64_t cpu_operations{0};
    uint64_t io_operations{0};
    uint64_t memory_operations{0};
    double cpu_time{0.0};
    double wall_time{0.0};
    uint64_t context_switches{0};
    uint64_t voluntary_switches{0};
    uint64_t involuntary_switches{0};
    
    int thread_id{0};
    int nice_value{0};
    WorkloadType workload_type{WorkloadType::CPU_INTENSIVE};
    double fairness_score{0.0};
};

template<typename RandomGen>
class WorkloadGenerator {
private:
    RandomGen& rng_;
    std::uniform_int_distribution<> int_dist_{1, 1000};
    std::uniform_real_distribution<> real_dist_{0.0, 1.0};
    
public:
    explicit WorkloadGenerator(RandomGen& rng) : rng_(rng) {}
    
    void cpu_intensive_work(std::atomic<bool>& stop_flag, ThreadStats& stats) {
        uint64_t operations = 0;
        double result = 0.0;
        
        while (!stop_flag.load(std::memory_order_relaxed)) {
            // CPU-bound computation
            for (int i = 0; i < 1000; ++i) {
                result += std::sqrt(i * real_dist_(rng_));
                result = std::sin(result) * std::cos(result);
                ++operations;
            }
            
            if (operations % 100000 == 0) {
                stats.cpu_operations.fetch_add(100000, std::memory_order_relaxed);
                operations = 0;
            }
        }
        
        stats.cpu_operations.fetch_add(operations, std::memory_order_relaxed);
    }
    
    void io_intensive_work(std::atomic<bool>& stop_flag, ThreadStats& stats) {
        uint64_t operations = 0;
        
        while (!stop_flag.load(std::memory_order_relaxed)) {
            // Simulate I/O operations with sleeps
            std::this_thread::sleep_for(std::chrono::microseconds(int_dist_(rng_) % 100));
            ++operations;
            
            if (operations % 100 == 0) {
                stats.io_operations.fetch_add(100, std::memory_order_relaxed);
                operations = 0;
            }
        }
        
        stats.io_operations.fetch_add(operations, std::memory_order_relaxed);
    }
    
    void mixed_work(std::atomic<bool>& stop_flag, ThreadStats& stats) {
        uint64_t cpu_ops = 0;
        uint64_t io_ops = 0;
        double result = 0.0;
        
        while (!stop_flag.load(std::memory_order_relaxed)) {
            // Mix of CPU and I/O work
            if (real_dist_(rng_) < 0.7) {
                // CPU work (70% of time)
                for (int i = 0; i < 100; ++i) {
                    result += std::sqrt(i * real_dist_(rng_));
                    ++cpu_ops;
                }
            } else {
                // I/O work (30% of time)
                std::this_thread::sleep_for(std::chrono::microseconds(int_dist_(rng_) % 50));
                ++io_ops;
            }
            
            if ((cpu_ops + io_ops) % 1000 == 0) {
                stats.cpu_operations.fetch_add(cpu_ops, std::memory_order_relaxed);
                stats.io_operations.fetch_add(io_ops, std::memory_order_relaxed);
                cpu_ops = io_ops = 0;
            }
        }
        
        stats.cpu_operations.fetch_add(cpu_ops, std::memory_order_relaxed);
        stats.io_operations.fetch_add(io_ops, std::memory_order_relaxed);
    }
    
    void memory_intensive_work(std::atomic<bool>& stop_flag, ThreadStats& stats) {
        constexpr size_t buffer_size = 1024 * 1024; // 1MB
        auto buffer = std::make_unique<char[]>(buffer_size);
        uint64_t operations = 0;
        
        while (!stop_flag.load(std::memory_order_relaxed)) {
            // Memory-intensive operations
            for (size_t i = 0; i < buffer_size; i += 64) {
                buffer[i] = static_cast<char>(int_dist_(rng_) % 256);
                ++operations;
            }
            
            // Random memory access pattern
            for (int i = 0; i < 1000; ++i) {
                size_t idx = int_dist_(rng_) % buffer_size;
                buffer[idx] = buffer[idx] ^ 0xFF;
                ++operations;
            }
            
            if (operations % 10000 == 0) {
                stats.memory_operations.fetch_add(10000, std::memory_order_relaxed);
                operations = 0;
            }
        }
        
        stats.memory_operations.fetch_add(operations, std::memory_order_relaxed);
    }
};

class ResourceMonitor {
public:
    static void get_thread_stats(ThreadStats& stats) {
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            double user_time = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6;
            double sys_time = usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
            stats.cpu_time.store(user_time + sys_time, std::memory_order_relaxed);
            
            stats.voluntary_switches.store(usage.ru_nvcsw, std::memory_order_relaxed);
            stats.involuntary_switches.store(usage.ru_nivcsw, std::memory_order_relaxed);
            stats.context_switches.store(usage.ru_nvcsw + usage.ru_nivcsw, std::memory_order_relaxed);
        }
    }
};

class FairnessAnalyzer {
public:
    static double calculate_jains_index(const std::vector<double>& values) {
        if (values.empty()) return 0.0;
        
        double sum = std::accumulate(values.begin(), values.end(), 0.0);
        double sum_of_squares = std::inner_product(values.begin(), values.end(), values.begin(), 0.0);
        
        double n = static_cast<double>(values.size());
        return (sum * sum) / (n * sum_of_squares);
    }
    
    static double calculate_coefficient_of_variation(const std::vector<double>& values) {
        if (values.size() < 2) return 0.0;
        
        double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
        double variance = 0.0;
        
        for (double val : values) {
            variance += (val - mean) * (val - mean);
        }
        variance /= (values.size() - 1);
        
        double std_dev = std::sqrt(variance);
        return (mean != 0.0) ? (std_dev / mean) : 0.0;
    }
    
    static void analyze_fairness(std::vector<ThreadStatsCopy>& thread_stats) {
        std::vector<double> cpu_times;
        std::vector<double> operation_counts;
        
        for (const auto& stats : thread_stats) {
            cpu_times.push_back(stats.cpu_time);
            operation_counts.push_back(static_cast<double>(
                stats.cpu_operations + stats.io_operations + stats.memory_operations));
        }
        
        double jains_index_cpu = calculate_jains_index(cpu_times);
        double jains_index_ops = calculate_jains_index(operation_counts);
        double cv_cpu = calculate_coefficient_of_variation(cpu_times);
        double cv_ops = calculate_coefficient_of_variation(operation_counts);
        
        // Store fairness scores in thread stats
        for (size_t i = 0; i < thread_stats.size(); ++i) {
            thread_stats[i].fairness_score = (jains_index_cpu + jains_index_ops) / 2.0;
        }
        
        std::cout << "\n=== Fairness Analysis ===\n";
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "Jain's Fairness Index (CPU time): " << jains_index_cpu << "\n";
        std::cout << "Jain's Fairness Index (Operations): " << jains_index_ops << "\n";
        std::cout << "Coefficient of Variation (CPU): " << cv_cpu << "\n";
        std::cout << "Coefficient of Variation (Ops): " << cv_ops << "\n";
        
        if (jains_index_cpu > 0.9) {
            std::cout << "CPU time distribution: EXCELLENT fairness\n";
        } else if (jains_index_cpu > 0.7) {
            std::cout << "CPU time distribution: GOOD fairness\n";
        } else if (jains_index_cpu > 0.5) {
            std::cout << "CPU time distribution: MODERATE fairness\n";
        } else {
            std::cout << "CPU time distribution: POOR fairness\n";
        }
    }
};

class SchedulerAnalyzer {
private:
    int duration_seconds_;
    int num_threads_;
    bool use_nice_values_;
    bool verbose_;
    std::vector<std::unique_ptr<ThreadStats>> thread_stats_;
    std::atomic<bool> stop_flag_{false};
    
public:
    SchedulerAnalyzer(int duration, int threads, bool nice_values, bool verbose)
        : duration_seconds_(duration), num_threads_(threads), 
          use_nice_values_(nice_values), verbose_(verbose) {
        
        thread_stats_.reserve(num_threads_);
        for (int i = 0; i < num_threads_; ++i) {
            thread_stats_.push_back(std::make_unique<ThreadStats>());
            thread_stats_[i]->thread_id = i;
        }
    }
    
    void run_analysis() {
        std::cout << "Scheduler Fairness Analyzer (C++)\n";
        std::cout << "=================================\n";
        std::cout << "Duration: " << duration_seconds_ << " seconds\n";
        std::cout << "Threads: " << num_threads_ << "\n";
        std::cout << "Nice values: " << (use_nice_values_ ? "enabled" : "disabled") << "\n\n";
        
        configure_workloads();
        run_benchmark();
        analyze_results();
    }
    
private:
    void configure_workloads() {
        std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());
        std::uniform_int_distribution<> workload_dist(0, 3);
        std::uniform_int_distribution<> nice_dist(-10, 10);
        
        for (int i = 0; i < num_threads_; ++i) {
            // Assign workload type
            int workload_choice = workload_dist(rng);
            switch (workload_choice) {
                case 0: thread_stats_[i]->workload_type = WorkloadType::CPU_INTENSIVE; break;
                case 1: thread_stats_[i]->workload_type = WorkloadType::IO_INTENSIVE; break;
                case 2: thread_stats_[i]->workload_type = WorkloadType::MIXED; break;
                case 3: thread_stats_[i]->workload_type = WorkloadType::MEMORY_INTENSIVE; break;
            }
            
            // Assign nice value
            if (use_nice_values_) {
                thread_stats_[i]->nice_value = nice_dist(rng);
            }
        }
        
        if (verbose_) {
            std::cout << "Thread Configuration:\n";
            for (int i = 0; i < num_threads_; ++i) {
                std::cout << "Thread " << i << ": ";
                switch (thread_stats_[i]->workload_type) {
                    case WorkloadType::CPU_INTENSIVE: std::cout << "CPU-intensive"; break;
                    case WorkloadType::IO_INTENSIVE: std::cout << "I/O-intensive"; break;
                    case WorkloadType::MIXED: std::cout << "Mixed workload"; break;
                    case WorkloadType::MEMORY_INTENSIVE: std::cout << "Memory-intensive"; break;
                }
                if (use_nice_values_) {
                    std::cout << " (nice: " << thread_stats_[i]->nice_value << ")";
                }
                std::cout << "\n";
            }
            std::cout << "\n";
        }
    }
    
    void run_benchmark() {
        std::vector<std::thread> threads;
        threads.reserve(num_threads_);
        
        auto start_time = Clock::now();
        
        for (int i = 0; i < num_threads_; ++i) {
            threads.emplace_back(&SchedulerAnalyzer::worker_thread, this, i);
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
        
        // Update wall time for all threads
        for (auto& stats : thread_stats_) {
            stats->wall_time.store(actual_duration);
        }
    }
    
    void worker_thread(int thread_id) {
        auto& stats = *thread_stats_[thread_id];
        
        // Set nice value if enabled
        if (use_nice_values_) {
            if (nice(stats.nice_value) == -1 && errno != 0) {
                std::cerr << "Warning: Failed to set nice value for thread " << thread_id << "\n";
            }
        }
        
        std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count() + thread_id);
        WorkloadGenerator<std::mt19937> workload_gen(rng);
        
        switch (stats.workload_type) {
            case WorkloadType::CPU_INTENSIVE:
                workload_gen.cpu_intensive_work(stop_flag_, stats);
                break;
            case WorkloadType::IO_INTENSIVE:
                workload_gen.io_intensive_work(stop_flag_, stats);
                break;
            case WorkloadType::MIXED:
                workload_gen.mixed_work(stop_flag_, stats);
                break;
            case WorkloadType::MEMORY_INTENSIVE:
                workload_gen.memory_intensive_work(stop_flag_, stats);
                break;
        }
        
        // Get final resource usage
        ResourceMonitor::get_thread_stats(stats);
    }
    
    void analyze_results() {
        // Create a vector of copies for analysis
        std::vector<ThreadStatsCopy> stats_copy;
        
        for (const auto& stats_ptr : thread_stats_) {
            ThreadStatsCopy copy;
            copy.cpu_operations = stats_ptr->cpu_operations.load();
            copy.io_operations = stats_ptr->io_operations.load();
            copy.memory_operations = stats_ptr->memory_operations.load();
            copy.cpu_time = stats_ptr->cpu_time.load();
            copy.wall_time = stats_ptr->wall_time.load();
            copy.context_switches = stats_ptr->context_switches.load();
            copy.voluntary_switches = stats_ptr->voluntary_switches.load();
            copy.involuntary_switches = stats_ptr->involuntary_switches.load();
            copy.thread_id = stats_ptr->thread_id;
            copy.nice_value = stats_ptr->nice_value;
            copy.workload_type = stats_ptr->workload_type;
            copy.fairness_score = stats_ptr->fairness_score;
            stats_copy.push_back(std::move(copy));
        }
        
        FairnessAnalyzer::analyze_fairness(stats_copy);
        print_detailed_stats();
        print_summary();
    }
    
    void print_detailed_stats() {
        if (!verbose_) return;
        
        std::cout << "\n=== Detailed Thread Statistics ===\n";
        std::cout << std::left << std::setw(8) << "Thread"
                  << std::setw(15) << "Workload"
                  << std::setw(8) << "Nice"
                  << std::setw(12) << "CPU Ops"
                  << std::setw(12) << "I/O Ops"
                  << std::setw(12) << "Mem Ops"
                  << std::setw(10) << "CPU Time"
                  << std::setw(12) << "Ctx Switch"
                  << std::setw(8) << "Vol"
                  << std::setw(8) << "Invol" << "\n";
        std::cout << std::string(120, '-') << "\n";
        
        for (const auto& stats_ptr : thread_stats_) {
            const auto& stats = *stats_ptr;
            std::string workload_name;
            switch (stats.workload_type) {
                case WorkloadType::CPU_INTENSIVE: workload_name = "CPU"; break;
                case WorkloadType::IO_INTENSIVE: workload_name = "I/O"; break;
                case WorkloadType::MIXED: workload_name = "Mixed"; break;
                case WorkloadType::MEMORY_INTENSIVE: workload_name = "Memory"; break;
            }
            
            std::cout << std::left << std::setw(8) << stats.thread_id
                      << std::setw(15) << workload_name
                      << std::setw(8) << stats.nice_value
                      << std::setw(12) << stats.cpu_operations.load()
                      << std::setw(12) << stats.io_operations.load()
                      << std::setw(12) << stats.memory_operations.load()
                      << std::fixed << std::setprecision(3)
                      << std::setw(10) << stats.cpu_time.load()
                      << std::setw(12) << stats.context_switches.load()
                      << std::setw(8) << stats.voluntary_switches.load()
                      << std::setw(8) << stats.involuntary_switches.load() << "\n";
        }
    }
    
    void print_summary() {
        uint64_t total_cpu_ops = 0;
        uint64_t total_io_ops = 0;
        uint64_t total_mem_ops = 0;
        double total_cpu_time = 0.0;
        uint64_t total_context_switches = 0;
        
        for (const auto& stats_ptr : thread_stats_) {
            const auto& stats = *stats_ptr;
            total_cpu_ops += stats.cpu_operations.load();
            total_io_ops += stats.io_operations.load();
            total_mem_ops += stats.memory_operations.load();
            total_cpu_time += stats.cpu_time.load();
            total_context_switches += stats.context_switches.load();
        }
        
        std::cout << "\n=== Summary ===\n";
        std::cout << "Total operations: " << (total_cpu_ops + total_io_ops + total_mem_ops) << "\n";
        std::cout << "  CPU operations: " << total_cpu_ops << "\n";
        std::cout << "  I/O operations: " << total_io_ops << "\n";
        std::cout << "  Memory operations: " << total_mem_ops << "\n";
        std::cout << "Total CPU time: " << std::fixed << std::setprecision(3) << total_cpu_time << " seconds\n";
        std::cout << "Total context switches: " << total_context_switches << "\n";
        std::cout << "Average context switches per thread: " 
                  << (total_context_switches / static_cast<double>(num_threads_)) << "\n";
        
        // Calculate throughput
        double wall_time = thread_stats_[0]->wall_time.load();
        if (wall_time > 0) {
            std::cout << "Throughput: " 
                      << static_cast<uint64_t>((total_cpu_ops + total_io_ops + total_mem_ops) / wall_time)
                      << " operations/second\n";
        }
    }
};

void print_usage(const std::string& prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n";
    std::cout << "Options:\n";
    std::cout << "  -d <seconds>  Duration of test (default: 10)\n";
    std::cout << "  -t <threads>  Number of threads (default: 4)\n";
    std::cout << "  -n            Enable nice values for priority testing\n";
    std::cout << "  -v            Verbose output with detailed statistics\n";
    std::cout << "  -h            Show this help message\n";
    std::cout << "\nAnalyzes OS scheduler fairness using multiple workload types:\n";
    std::cout << "  - CPU-intensive workloads\n";
    std::cout << "  - I/O-intensive workloads\n";
    std::cout << "  - Mixed workloads\n";
    std::cout << "  - Memory-intensive workloads\n";
}

} // namespace SchedulerAnalysis

int main(int argc, char* argv[]) {
    using namespace SchedulerAnalysis;
    
    int duration = 10;
    int num_threads = 4;
    bool use_nice_values = false;
    bool verbose = false;
    
    int opt;
    while ((opt = getopt(argc, argv, "d:t:nvh")) != -1) {
        switch (opt) {
            case 'd':
                duration = std::stoi(optarg);
                if (duration <= 0) {
                    std::cerr << "Error: duration must be positive\n";
                    return 1;
                }
                break;
            case 't':
                num_threads = std::stoi(optarg);
                if (num_threads <= 0 || num_threads > 64) {
                    std::cerr << "Error: thread count must be between 1 and 64\n";
                    return 1;
                }
                break;
            case 'n':
                use_nice_values = true;
                break;
            case 'v':
                verbose = true;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }
    
    try {
        SchedulerAnalyzer analyzer(duration, num_threads, use_nice_values, verbose);
        analyzer.run_analysis();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}