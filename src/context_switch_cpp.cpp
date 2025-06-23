#include <iostream>
#include <chrono>
#include <vector>
#include <memory>
#include <thread>
#include <future>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <string>
#include <optional>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>

namespace ContextSwitch {

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = Clock::time_point;

constexpr int DEFAULT_ITERATIONS = 1000;
constexpr int PIPE_READ = 0;
constexpr int PIPE_WRITE = 1;

class PipeRAII {
private:
    int fds[2];
    bool valid;
    
public:
    PipeRAII() : valid(false) {
        if (pipe(fds) == 0) {
            valid = true;
        }
    }
    
    ~PipeRAII() {
        if (valid) {
            close(fds[PIPE_READ]);
            close(fds[PIPE_WRITE]);
        }
    }
    
    bool is_valid() const { return valid; }
    int read_fd() const { return fds[PIPE_READ]; }
    int write_fd() const { return fds[PIPE_WRITE]; }
    
    // Non-copyable
    PipeRAII(const PipeRAII&) = delete;
    PipeRAII& operator=(const PipeRAII&) = delete;
    
    // Movable
    PipeRAII(PipeRAII&& other) noexcept : valid(other.valid) {
        fds[0] = other.fds[0];
        fds[1] = other.fds[1];
        other.valid = false;
    }
    
    PipeRAII& operator=(PipeRAII&& other) noexcept {
        if (this != &other) {
            if (valid) {
                close(fds[PIPE_READ]);
                close(fds[PIPE_WRITE]);
            }
            fds[0] = other.fds[0];
            fds[1] = other.fds[1];
            valid = other.valid;
            other.valid = false;
        }
        return *this;
    }
};

class SharedMemory {
private:
    void* addr;
    size_t size;
    int fd;
    std::string name;
    
public:
    explicit SharedMemory(size_t sz) : size(sz) {
        name = "/context_switch_" + std::to_string(getpid());
        fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd == -1) {
            throw std::runtime_error("Failed to create shared memory");
        }
        
        if (ftruncate(fd, size) == -1) {
            close(fd);
            shm_unlink(name.c_str());
            throw std::runtime_error("Failed to set shared memory size");
        }
        
        addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (addr == MAP_FAILED) {
            close(fd);
            shm_unlink(name.c_str());
            throw std::runtime_error("Failed to map shared memory");
        }
    }
    
    ~SharedMemory() {
        if (addr != MAP_FAILED) {
            munmap(addr, size);
        }
        if (fd != -1) {
            close(fd);
            shm_unlink(name.c_str());
        }
    }
    
    template<typename T>
    T* get() { return static_cast<T*>(addr); }
    
    // Non-copyable, movable
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    SharedMemory(SharedMemory&&) = default;
    SharedMemory& operator=(SharedMemory&&) = default;
};

struct ContextSwitchStats {
    std::vector<double> times;
    double mean{0.0};
    double median{0.0};
    double min_time{0.0};
    double max_time{0.0};
    double std_dev{0.0};
    double percentile_95{0.0};
    double percentile_99{0.0};
    
    void calculate() {
        if (times.empty()) return;
        
        std::sort(times.begin(), times.end());
        
        min_time = times.front();
        max_time = times.back();
        mean = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
        median = times[times.size() / 2];
        
        if (times.size() >= 20) {
            percentile_95 = times[static_cast<size_t>(times.size() * 0.95)];
        }
        if (times.size() >= 100) {
            percentile_99 = times[static_cast<size_t>(times.size() * 0.99)];
        }
        
        double variance = 0.0;
        for (double time : times) {
            variance += (time - mean) * (time - mean);
        }
        std_dev = std::sqrt(variance / times.size());
    }
    
    void print() const {
        std::cout << "\n=== Context Switch Performance Analysis ===\n";
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "Iterations: " << times.size() << "\n";
        std::cout << "Mean time: " << mean * 1e6 << " μs\n";
        std::cout << "Median time: " << median * 1e6 << " μs\n";
        std::cout << "Min time: " << min_time * 1e6 << " μs\n";
        std::cout << "Max time: " << max_time * 1e6 << " μs\n";
        std::cout << "Std deviation: " << std_dev * 1e6 << " μs\n";
        
        if (percentile_95 > 0) {
            std::cout << "95th percentile: " << percentile_95 * 1e6 << " μs\n";
        }
        if (percentile_99 > 0) {
            std::cout << "99th percentile: " << percentile_99 * 1e6 << " μs\n";
        }
        
        print_histogram();
    }
    
private:
    void print_histogram() const {
        if (times.size() < 10) return;
        
        std::cout << "\nTime Distribution Histogram:\n";
        constexpr int buckets = 20;
        std::vector<int> histogram(buckets, 0);
        
        double range = max_time - min_time;
        if (range == 0) return;
        
        for (double time : times) {
            int bucket = static_cast<int>((time - min_time) / range * (buckets - 1));
            if (bucket >= buckets) bucket = buckets - 1;
            histogram[bucket]++;
        }
        
        int max_count = *std::max_element(histogram.begin(), histogram.end());
        if (max_count == 0) return;
        
        for (int i = 0; i < buckets; i++) {
            double bucket_start = min_time + (range * i / buckets);
            double bucket_end = min_time + (range * (i + 1) / buckets);
            
            std::cout << std::setw(6) << std::fixed << std::setprecision(1)
                      << bucket_start * 1e6 << "-" << std::setw(6) 
                      << bucket_end * 1e6 << " μs: ";
            
            int bar_length = (histogram[i] * 50) / max_count;
            for (int j = 0; j < bar_length; j++) {
                std::cout << "█";
            }
            std::cout << " " << histogram[i] << "\n";
        }
    }
};

class ContextSwitchMeasurer {
public:
    static ContextSwitchStats measure_with_pipes(int iterations) {
        std::cout << "Measuring context switch overhead using pipes...\n";
        
        PipeRAII pipe1, pipe2;
        if (!pipe1.is_valid() || !pipe2.is_valid()) {
            throw std::runtime_error("Failed to create pipes");
        }
        
        ContextSwitchStats stats;
        stats.times.reserve(iterations);
        
        pid_t child_pid = fork();
        if (child_pid == -1) {
            throw std::runtime_error("Failed to fork");
        }
        
        if (child_pid == 0) {
            // Child process
            child_process(pipe1.read_fd(), pipe2.write_fd(), iterations);
            std::exit(0);
        } else {
            // Parent process
            parent_process(pipe1.write_fd(), pipe2.read_fd(), iterations, stats);
            
            int status;
            waitpid(child_pid, &status, 0);
        }
        
        stats.calculate();
        return stats;
    }
    
    static ContextSwitchStats measure_with_shared_memory(int iterations) {
        std::cout << "Measuring context switch overhead using shared memory...\n";
        
        struct SyncData {
            volatile int turn;
            volatile bool done;
            TimePoint timestamps[2];
        };
        
        SharedMemory shm(sizeof(SyncData));
        auto* sync_data = shm.get<SyncData>();
        sync_data->turn = 0;
        sync_data->done = false;
        
        ContextSwitchStats stats;
        stats.times.reserve(iterations);
        
        pid_t child_pid = fork();
        if (child_pid == -1) {
            throw std::runtime_error("Failed to fork");
        }
        
        if (child_pid == 0) {
            // Child process
            for (int i = 0; i < iterations; i++) {
                while (sync_data->turn != 1 && !sync_data->done) {
                    std::this_thread::yield();
                }
                if (sync_data->done) break;
                
                sync_data->timestamps[1] = Clock::now();
                sync_data->turn = 0;
            }
            std::exit(0);
        } else {
            // Parent process
            for (int i = 0; i < iterations; i++) {
                sync_data->timestamps[0] = Clock::now();
                sync_data->turn = 1;
                
                while (sync_data->turn != 0) {
                    std::this_thread::yield();
                }
                
                auto context_switch_time = Duration(sync_data->timestamps[1] - sync_data->timestamps[0]).count();
                stats.times.push_back(context_switch_time);
            }
            
            sync_data->done = true;
            
            int status;
            waitpid(child_pid, &status, 0);
        }
        
        stats.calculate();
        return stats;
    }
    
    static ContextSwitchStats measure_with_threads(int iterations) {
        std::cout << "Measuring thread context switch overhead...\n";
        
        ContextSwitchStats stats;
        stats.times.reserve(iterations);
        
        std::atomic<int> turn{0};
        std::atomic<bool> done{false};
        std::vector<TimePoint> timestamps(2);
        
        auto thread_func = [&](int thread_id) {
            int my_turn = thread_id;
            int other_turn = 1 - thread_id;
            
            for (int i = 0; i < iterations; i++) {
                while (turn.load() != my_turn && !done.load()) {
                    std::this_thread::yield();
                }
                if (done.load()) break;
                
                timestamps[thread_id] = Clock::now();
                turn.store(other_turn);
            }
        };
        
        std::thread t1(thread_func, 1);
        
        for (int i = 0; i < iterations; i++) {
            auto start_time = Clock::now();
            timestamps[0] = start_time;
            turn.store(1);
            
            while (turn.load() != 0) {
                std::this_thread::yield();
            }
            
            auto context_switch_time = Duration(timestamps[1] - timestamps[0]).count();
            stats.times.push_back(context_switch_time);
        }
        
        done.store(true);
        t1.join();
        
        stats.calculate();
        return stats;
    }

private:
    static void parent_process(int write_fd, int read_fd, int iterations, ContextSwitchStats& stats) {
        char byte = 1;
        
        for (int i = 0; i < iterations; i++) {
            auto start_time = Clock::now();
            
            if (write(write_fd, &byte, 1) != 1) {
                throw std::runtime_error("Parent write failed");
            }
            
            if (read(read_fd, &byte, 1) != 1) {
                throw std::runtime_error("Parent read failed");
            }
            
            auto end_time = Clock::now();
            auto context_switch_time = Duration(end_time - start_time).count();
            stats.times.push_back(context_switch_time);
        }
    }
    
    static void child_process(int read_fd, int write_fd, int iterations) {
        char byte;
        
        for (int i = 0; i < iterations; i++) {
            if (read(read_fd, &byte, 1) != 1) {
                break;
            }
            
            if (write(write_fd, &byte, 1) != 1) {
                break;
            }
        }
    }
};

class ContextSwitchBenchmark {
private:
    int iterations_;
    bool verbose_;
    
public:
    ContextSwitchBenchmark(int iterations = DEFAULT_ITERATIONS, bool verbose = false)
        : iterations_(iterations), verbose_(verbose) {}
    
    void run_all_tests() {
        std::cout << "Context Switch Measurement Benchmark (C++)\n";
        std::cout << "==========================================\n";
        std::cout << "Iterations per test: " << iterations_ << "\n\n";
        
        std::vector<std::pair<std::string, ContextSwitchStats>> results;
        
        try {
            auto pipe_stats = ContextSwitchMeasurer::measure_with_pipes(iterations_);
            results.emplace_back("Pipe-based IPC", std::move(pipe_stats));
        } catch (const std::exception& e) {
            std::cerr << "Pipe test failed: " << e.what() << "\n";
        }
        
        try {
            auto shm_stats = ContextSwitchMeasurer::measure_with_shared_memory(iterations_);
            results.emplace_back("Shared Memory", std::move(shm_stats));
        } catch (const std::exception& e) {
            std::cerr << "Shared memory test failed: " << e.what() << "\n";
        }
        
        try {
            auto thread_stats = ContextSwitchMeasurer::measure_with_threads(iterations_);
            results.emplace_back("Thread Switching", std::move(thread_stats));
        } catch (const std::exception& e) {
            std::cerr << "Thread test failed: " << e.what() << "\n";
        }
        
        print_summary(results);
    }
    
private:
    void print_summary(const std::vector<std::pair<std::string, ContextSwitchStats>>& results) {
        if (verbose_) {
            for (const auto& [name, stats] : results) {
                std::cout << "\n" << name << " Results:\n";
                std::cout << std::string(name.length() + 9, '=') << "\n";
                stats.print();
            }
        }
        
        std::cout << "\n=== Summary Comparison ===\n";
        std::cout << std::left << std::setw(20) << "Method"
                  << std::right << std::setw(12) << "Mean (μs)"
                  << std::setw(12) << "Median (μs)"
                  << std::setw(12) << "Min (μs)"
                  << std::setw(12) << "Max (μs)"
                  << std::setw(12) << "Std Dev (μs)" << "\n";
        std::cout << std::string(80, '-') << "\n";
        
        for (const auto& [name, stats] : results) {
            std::cout << std::left << std::setw(20) << name
                      << std::right << std::fixed << std::setprecision(2)
                      << std::setw(12) << stats.mean * 1e6
                      << std::setw(12) << stats.median * 1e6
                      << std::setw(12) << stats.min_time * 1e6
                      << std::setw(12) << stats.max_time * 1e6
                      << std::setw(12) << stats.std_dev * 1e6 << "\n";
        }
        
        if (!results.empty()) {
            auto best_it = std::min_element(results.begin(), results.end(),
                [](const auto& a, const auto& b) {
                    return a.second.median < b.second.median;
                });
            
            std::cout << "\nFastest method: " << best_it->first
                      << " (median: " << std::fixed << std::setprecision(2)
                      << best_it->second.median * 1e6 << " μs)\n";
        }
    }
};

void print_usage(const std::string& prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n";
    std::cout << "Options:\n";
    std::cout << "  -n <count>    Number of iterations (default: 1000)\n";
    std::cout << "  -v            Verbose output with detailed statistics\n";
    std::cout << "  -h            Show this help message\n";
    std::cout << "\nMeasures context switch overhead using multiple methods:\n";
    std::cout << "  - Pipe-based IPC (process context switches)\n";
    std::cout << "  - Shared memory synchronization\n";
    std::cout << "  - Thread context switches\n";
}

} // namespace ContextSwitch

int main(int argc, char* argv[]) {
    using namespace ContextSwitch;
    
    int iterations = DEFAULT_ITERATIONS;
    bool verbose = false;
    
    int opt;
    while ((opt = getopt(argc, argv, "n:vh")) != -1) {
        switch (opt) {
            case 'n':
                iterations = std::stoi(optarg);
                if (iterations <= 0) {
                    std::cerr << "Error: iterations must be positive\n";
                    return 1;
                }
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
        ContextSwitchBenchmark benchmark(iterations, verbose);
        benchmark.run_all_tests();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}