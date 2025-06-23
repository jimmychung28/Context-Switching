#include <iostream>
#include <vector>
#include <array>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <numeric>
#include <random>
#include <iomanip>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
#include <functional>
#include <future>
#ifdef __cpp_lib_barrier
#include <barrier>
#endif
#include <cstring>
#include <cstdlib>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#else
#include <sys/sysinfo.h>
#include <numa.h>
#include <sched.h>
#endif

namespace CacheAnalysis {

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = Clock::time_point;

constexpr size_t CACHE_LINE_SIZE = 64;
constexpr size_t KB = 1024;
constexpr size_t MB = 1024 * 1024;
constexpr size_t MAX_THREADS = 64;

enum class AccessPattern {
    Sequential,
    Random,
    Stride,
    PointerChase,
    FalseSharing,
    CacheFriendly
};

enum class TestType {
    CacheHierarchy,
    FalseSharing,
    CacheBouncing,
    NUMAAnalysis,
    Prefetcher,
    All
};

template<typename T>
class AlignedAllocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template<typename U>
    struct rebind {
        using other = AlignedAllocator<U>;
    };

    T* allocate(std::size_t n) {
        void* ptr = std::aligned_alloc(CACHE_LINE_SIZE, n * sizeof(T));
        if (!ptr) throw std::bad_alloc();
        return static_cast<T*>(ptr);
    }

    void deallocate(T* p, std::size_t) noexcept {
        std::free(p);
    }

    bool operator==(const AlignedAllocator&) const noexcept { return true; }
    bool operator!=(const AlignedAllocator&) const noexcept { return false; }
};

template<typename T>
using AlignedVector = std::vector<T, AlignedAllocator<T>>;

struct SystemInfo {
    size_t l1_size{0};
    size_t l2_size{0};
    size_t l3_size{0};
    size_t cache_line_size{CACHE_LINE_SIZE};
    int num_cores{0};
    int num_numa_nodes{0};
    
    void detect() {
#ifdef __APPLE__
        size_t size = sizeof(size_t);
        
        sysctlbyname("hw.l1icachesize", &l1_size, &size, nullptr, 0);
        sysctlbyname("hw.l2cachesize", &l2_size, &size, nullptr, 0);
        sysctlbyname("hw.l3cachesize", &l3_size, &size, nullptr, 0);
        
        int ncpu;
        size = sizeof(int);
        sysctlbyname("hw.ncpu", &ncpu, &size, nullptr, 0);
        num_cores = ncpu;
        num_numa_nodes = 1;
        
        // Fallback values
        if (l1_size == 0) l1_size = 32 * KB;
        if (l2_size == 0) l2_size = 256 * KB;
        if (l3_size == 0) l3_size = 8 * MB;
        
#else
        // Linux detection
        auto read_cache_size = [](const std::string& path) -> size_t {
            std::ifstream file(path);
            if (!file) return 0;
            
            std::string line;
            std::getline(file, line);
            if (line.empty()) return 0;
            
            return std::stoul(line) * KB;
        };
        
        l1_size = read_cache_size("/sys/devices/system/cpu/cpu0/cache/index0/size");
        l2_size = read_cache_size("/sys/devices/system/cpu/cpu0/cache/index2/size");
        l3_size = read_cache_size("/sys/devices/system/cpu/cpu0/cache/index3/size");
        
        num_cores = std::thread::hardware_concurrency();
        
        if (numa_available() != -1) {
            num_numa_nodes = numa_num_configured_nodes();
        } else {
            num_numa_nodes = 1;
        }
        
        // Fallback values
        if (l1_size == 0) l1_size = 32 * KB;
        if (l2_size == 0) l2_size = 256 * KB;
        if (l3_size == 0) l3_size = 8 * MB;
#endif
    }
    
    void print() const {
        std::cout << "=== System Cache Information ===\n";
        std::cout << "L1 Cache Size: " << l1_size / KB << " KB\n";
        std::cout << "L2 Cache Size: " << l2_size / KB << " KB\n";
        std::cout << "L3 Cache Size: " << l3_size / MB << " MB\n";
        std::cout << "Cache Line Size: " << cache_line_size << " bytes\n";
        std::cout << "Number of Cores: " << num_cores << "\n";
        std::cout << "NUMA Nodes: " << num_numa_nodes << "\n\n";
    }
};

struct CachePerformanceMetrics {
    uint64_t total_accesses{0};
    uint64_t estimated_l1_hits{0};
    uint64_t estimated_l2_hits{0};
    uint64_t estimated_l3_hits{0};
    uint64_t estimated_memory_accesses{0};
    double total_time{0.0};
    double avg_latency{0.0};
    double throughput{0.0};
    int thread_id{0};
    int cpu_id{0};
    AccessPattern pattern{AccessPattern::Sequential};
    
    void calculate_estimates(size_t data_size, const SystemInfo& sys_info) {
        double l1_hit_rate = 0.0, l2_hit_rate = 0.0, l3_hit_rate = 0.0;
        
        switch (pattern) {
            case AccessPattern::Sequential:
                if (data_size <= sys_info.l1_size) {
                    l1_hit_rate = 0.95; l2_hit_rate = 0.04; l3_hit_rate = 0.01;
                } else if (data_size <= sys_info.l2_size) {
                    l1_hit_rate = 0.30; l2_hit_rate = 0.65; l3_hit_rate = 0.04;
                } else if (data_size <= sys_info.l3_size) {
                    l1_hit_rate = 0.10; l2_hit_rate = 0.20; l3_hit_rate = 0.65;
                } else {
                    l1_hit_rate = 0.05; l2_hit_rate = 0.10; l3_hit_rate = 0.30;
                }
                break;
                
            case AccessPattern::Random:
                if (data_size <= sys_info.l1_size) {
                    l1_hit_rate = 0.80; l2_hit_rate = 0.15; l3_hit_rate = 0.05;
                } else if (data_size <= sys_info.l2_size) {
                    l1_hit_rate = 0.15; l2_hit_rate = 0.70; l3_hit_rate = 0.10;
                } else if (data_size <= sys_info.l3_size) {
                    l1_hit_rate = 0.05; l2_hit_rate = 0.15; l3_hit_rate = 0.60;
                } else {
                    l1_hit_rate = 0.02; l2_hit_rate = 0.05; l3_hit_rate = 0.20;
                }
                break;
                
            case AccessPattern::Stride:
                if (data_size <= sys_info.l2_size) {
                    l1_hit_rate = 0.60; l2_hit_rate = 0.35; l3_hit_rate = 0.04;
                } else {
                    l1_hit_rate = 0.20; l2_hit_rate = 0.40; l3_hit_rate = 0.35;
                }
                break;
                
            default:
                l1_hit_rate = 0.50; l2_hit_rate = 0.30; l3_hit_rate = 0.15;
                break;
        }
        
        estimated_l1_hits = static_cast<uint64_t>(total_accesses * l1_hit_rate);
        estimated_l2_hits = static_cast<uint64_t>(total_accesses * l2_hit_rate);
        estimated_l3_hits = static_cast<uint64_t>(total_accesses * l3_hit_rate);
        estimated_memory_accesses = total_accesses - estimated_l1_hits - estimated_l2_hits - estimated_l3_hits;
    }
};

template<AccessPattern Pattern>
class AccessPatternGenerator {
public:
    static constexpr AccessPattern pattern = Pattern;
    
    template<typename Container>
    static void initialize_data(Container& data) {
        if constexpr (Pattern == AccessPattern::Random || Pattern == AccessPattern::PointerChase) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 255);
            
            for (auto& item : data) {
                item = static_cast<typename Container::value_type>(dis(gen));
            }
        } else {
            std::iota(data.begin(), data.end(), typename Container::value_type{0});
        }
    }
    
    template<typename Container>
    static uint64_t execute_pattern(Container& data, size_t stride, int iterations) {
        uint64_t accesses = 0;
        volatile typename Container::value_type sum = 0;
        
        if constexpr (Pattern == AccessPattern::Sequential) {
            for (int iter = 0; iter < iterations; ++iter) {
                for (size_t i = 0; i < data.size(); i += stride) {
                    sum += data[i];
                    ++accesses;
                }
            }
        } else if constexpr (Pattern == AccessPattern::Random) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<size_t> dis(0, data.size() - 1);
            
            for (int iter = 0; iter < iterations; ++iter) {
                for (size_t i = 0; i < data.size() / stride; ++i) {
                    size_t idx = dis(gen);
                    sum += data[idx];
                    ++accesses;
                }
            }
        } else if constexpr (Pattern == AccessPattern::Stride) {
            for (int iter = 0; iter < iterations; ++iter) {
                for (size_t i = 0; i < data.size(); i += stride) {
                    sum += data[i];
                    ++accesses;
                }
            }
        }
        
        return accesses;
    }
};

template<typename T>
class PointerChaseList {
private:
    struct alignas(CACHE_LINE_SIZE) Node {
        T data;
        Node* next;
        char padding[CACHE_LINE_SIZE - sizeof(T) - sizeof(Node*)];
    };
    
    std::unique_ptr<Node[]> nodes_;
    size_t size_;
    
public:
    explicit PointerChaseList(size_t num_nodes) : size_(num_nodes) {
        nodes_ = std::make_unique<Node[]>(size_);
        
        // Create random chain
        std::vector<size_t> indices(size_);
        std::iota(indices.begin(), indices.end(), 0);
        
        std::random_device rd;
        std::mt19937 gen(rd());
        std::shuffle(indices.begin(), indices.end(), gen);
        
        for (size_t i = 0; i < size_ - 1; ++i) {
            nodes_[indices[i]].next = &nodes_[indices[i + 1]];
            nodes_[indices[i]].data = static_cast<T>(i);
        }
        nodes_[indices[size_ - 1]].next = &nodes_[indices[0]];
        nodes_[indices[size_ - 1]].data = static_cast<T>(size_ - 1);
    }
    
    uint64_t chase(int iterations) {
        Node* current = &nodes_[0];
        uint64_t accesses = 0;
        
        for (int iter = 0; iter < iterations; ++iter) {
            for (size_t i = 0; i < size_; ++i) {
                current = current->next;
                volatile T data = current->data; // Prevent optimization
                (void)data;
                ++accesses;
            }
        }
        
        return accesses;
    }
};

class CacheAnalyzer {
private:
    SystemInfo sys_info_;
    bool verbose_;
    
public:
    explicit CacheAnalyzer(bool verbose = false) : verbose_(verbose) {
        sys_info_.detect();
        if (verbose_) {
            sys_info_.print();
        }
    }
    
    template<AccessPattern Pattern>
    CachePerformanceMetrics run_memory_test(size_t memory_size, size_t stride, int iterations, int cpu_id = -1) {
        CachePerformanceMetrics metrics;
        metrics.pattern = Pattern;
        metrics.cpu_id = cpu_id;
        
        // Set CPU affinity if specified
        if (cpu_id >= 0) {
#ifdef __APPLE__
            thread_affinity_policy_data_t policy = { cpu_id };
            thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
            thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, 
                              (thread_policy_t)&policy, 1);
#else
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_id, &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
        }
        
        if constexpr (Pattern == AccessPattern::PointerChase) {
            size_t num_nodes = memory_size / CACHE_LINE_SIZE;
            PointerChaseList<char> chase_list(num_nodes);
            
            auto start = Clock::now();
            metrics.total_accesses = chase_list.chase(iterations);
            auto end = Clock::now();
            
            metrics.total_time = Duration(end - start).count();
        } else {
            AlignedVector<char> data(memory_size);
            AccessPatternGenerator<Pattern>::initialize_data(data);
            
            auto start = Clock::now();
            metrics.total_accesses = AccessPatternGenerator<Pattern>::execute_pattern(data, stride, iterations);
            auto end = Clock::now();
            
            metrics.total_time = Duration(end - start).count();
        }
        
        metrics.avg_latency = metrics.total_time / metrics.total_accesses;
        metrics.throughput = metrics.total_accesses / metrics.total_time;
        metrics.calculate_estimates(memory_size, sys_info_);
        
        return metrics;
    }
    
    void cache_hierarchy_analysis(int num_threads) {
        std::cout << "=== Cache Hierarchy Analysis ===\n";
        
        std::vector<size_t> test_sizes = {
            sys_info_.l1_size / 2,      // L1 cache
            sys_info_.l1_size * 2,      // L2 cache
            sys_info_.l2_size * 2,      // L3 cache
            sys_info_.l3_size * 2,      // Main memory
            sys_info_.l3_size * 8       // Large memory
        };
        
        std::vector<std::string> size_names = {"L1", "L2", "L3", "Memory", "Large Memory"};
        
        std::map<AccessPattern, std::string> pattern_names = {
            {AccessPattern::Sequential, "Sequential"},
            {AccessPattern::Random, "Random"},
            {AccessPattern::Stride, "Stride"},
            {AccessPattern::PointerChase, "Pointer Chase"}
        };
        
        for (auto& [pattern, pattern_name] : pattern_names) {
            std::cout << "\n" << pattern_name << " Access Pattern:\n";
            std::cout << std::left << std::setw(12) << "Size"
                      << std::setw(15) << "Latency(ns)"
                      << std::setw(15) << "Throughput"
                      << std::setw(12) << "L1 Hits"
                      << std::setw(12) << "L2 Hits"
                      << std::setw(12) << "Memory" << "\n";
            
            std::string separator(12 + 15 + 15 + 12 + 12 + 12, '-');
            std::cout << separator << "\n";
            
            for (size_t i = 0; i < test_sizes.size(); ++i) {
                std::vector<std::future<CachePerformanceMetrics>> futures;
                
                for (int t = 0; t < num_threads; ++t) {
                    auto* analyzer = this;
                    AccessPattern current_pattern = pattern;
                    futures.push_back(std::async(std::launch::async, [=]() {
                        switch (current_pattern) {
                            case AccessPattern::Sequential:
                                return analyzer->run_memory_test<AccessPattern::Sequential>(test_sizes[i], 1, 100, t % analyzer->sys_info_.num_cores);
                            case AccessPattern::Random:
                                return analyzer->run_memory_test<AccessPattern::Random>(test_sizes[i], 1, 100, t % analyzer->sys_info_.num_cores);
                            case AccessPattern::Stride:
                                return analyzer->run_memory_test<AccessPattern::Stride>(test_sizes[i], CACHE_LINE_SIZE * 2, 100, t % analyzer->sys_info_.num_cores);
                            case AccessPattern::PointerChase:
                                return analyzer->run_memory_test<AccessPattern::PointerChase>(test_sizes[i], 1, 100, t % analyzer->sys_info_.num_cores);
                            default:
                                return analyzer->run_memory_test<AccessPattern::Sequential>(test_sizes[i], 1, 100, t % analyzer->sys_info_.num_cores);
                        }
                    }));
                }
                
                // Collect results
                std::vector<CachePerformanceMetrics> results;
                for (auto& future : futures) {
                    results.push_back(future.get());
                }
                
                // Calculate averages
                double avg_latency = 0.0;
                double avg_throughput = 0.0;
                uint64_t total_l1_hits = 0;
                uint64_t total_l2_hits = 0;
                uint64_t total_memory = 0;
                uint64_t total_accesses = 0;
                
                for (const auto& result : results) {
                    avg_latency += result.avg_latency;
                    avg_throughput += result.throughput;
                    total_l1_hits += result.estimated_l1_hits;
                    total_l2_hits += result.estimated_l2_hits;
                    total_memory += result.estimated_memory_accesses;
                    total_accesses += result.total_accesses;
                }
                
                avg_latency /= num_threads;
                avg_throughput /= num_threads;
                
                double l1_percent = (double)total_l1_hits / total_accesses * 100;
                double l2_percent = (double)total_l2_hits / total_accesses * 100;
                double memory_percent = (double)total_memory / total_accesses * 100;
                
                std::cout << std::left << std::setw(12) << size_names[i]
                          << std::setw(15) << std::fixed << std::setprecision(2) << avg_latency * 1e9
                          << std::setw(15) << std::fixed << std::setprecision(0) << avg_throughput
                          << std::setw(12) << std::fixed << std::setprecision(1) << l1_percent << "%"
                          << std::setw(12) << std::fixed << std::setprecision(1) << l2_percent << "%"
                          << std::setw(12) << std::fixed << std::setprecision(1) << memory_percent << "%" << "\n";
            }
        }
    }
    
    void false_sharing_analysis(int num_threads) {
        std::cout << "\n=== False Sharing Analysis ===\n";
        
        // Test with false sharing - all threads access adjacent memory locations
        std::cout << "Testing FALSE SHARING scenario:\n";
        
        AlignedVector<std::atomic<int>> false_sharing_data(num_threads);
        
        auto false_sharing_test = [&](int thread_id) {
            auto start = Clock::now();
            
            for (int i = 0; i < 100000; ++i) {
                false_sharing_data[thread_id].fetch_add(1, std::memory_order_relaxed);
                
                // Add some computation
                for (int j = 0; j < 100; ++j) {
                    false_sharing_data[thread_id].fetch_add(j % 7, std::memory_order_relaxed);
                }
            }
            
            auto end = Clock::now();
            return Duration(end - start).count();
        };
        
        std::vector<std::future<double>> false_sharing_futures;
        auto false_sharing_start = Clock::now();
        
        for (int t = 0; t < num_threads; ++t) {
            false_sharing_futures.push_back(std::async(std::launch::async, false_sharing_test, t));
        }
        
        for (auto& future : false_sharing_futures) {
            future.wait();
        }
        
        auto false_sharing_end = Clock::now();
        double false_sharing_time = Duration(false_sharing_end - false_sharing_start).count();
        
        // Test without false sharing - each thread accesses its own cache line
        std::cout << "Testing CACHE-FRIENDLY scenario:\n";
        
        struct alignas(CACHE_LINE_SIZE) CacheLinePadded {
            std::atomic<int> counter{0};
            char padding[CACHE_LINE_SIZE - sizeof(std::atomic<int>)];
        };
        
        std::vector<CacheLinePadded> cache_friendly_data(num_threads);
        
        auto cache_friendly_test = [&](int thread_id) {
            auto start = Clock::now();
            
            for (int i = 0; i < 100000; ++i) {
                cache_friendly_data[thread_id].counter.fetch_add(1, std::memory_order_relaxed);
                
                // Add some computation
                for (int j = 0; j < 100; ++j) {
                    cache_friendly_data[thread_id].counter.fetch_add(j % 7, std::memory_order_relaxed);
                }
            }
            
            auto end = Clock::now();
            return Duration(end - start).count();
        };
        
        std::vector<std::future<double>> cache_friendly_futures;
        auto cache_friendly_start = Clock::now();
        
        for (int t = 0; t < num_threads; ++t) {
            cache_friendly_futures.push_back(std::async(std::launch::async, cache_friendly_test, t));
        }
        
        for (auto& future : cache_friendly_futures) {
            future.wait();
        }
        
        auto cache_friendly_end = Clock::now();
        double cache_friendly_time = Duration(cache_friendly_end - cache_friendly_start).count();
        
        // Results
        std::cout << "\nComparison Results:\n";
        std::cout << "False Sharing Time:  " << std::fixed << std::setprecision(3) << false_sharing_time << " seconds\n";
        std::cout << "Cache Friendly Time: " << cache_friendly_time << " seconds\n";
        std::cout << "Performance Ratio:   " << std::setprecision(2) << false_sharing_time / cache_friendly_time << "x (cache-friendly is faster)\n";
        
        // Calculate throughput
        double false_sharing_throughput = (num_threads * 100000 * 101) / false_sharing_time;
        double cache_friendly_throughput = (num_threads * 100000 * 101) / cache_friendly_time;
        
        std::cout << "False Sharing Throughput:  " << std::setprecision(0) << false_sharing_throughput << " ops/sec\n";
        std::cout << "Cache Friendly Throughput: " << cache_friendly_throughput << " ops/sec\n";
    }
    
    void cache_bouncing_analysis(int num_threads) {
        std::cout << "\n=== Cache Line Bouncing Analysis ===\n";
        
        if (num_threads < 2) {
            std::cout << "Cache bouncing test requires at least 2 threads.\n";
            return;
        }
        
        struct alignas(CACHE_LINE_SIZE) BouncingData {
            std::atomic<int> counter{0};
            char padding[CACHE_LINE_SIZE - sizeof(std::atomic<int>)];
        };
        
        BouncingData bouncing_data;
        
        std::cout << "Testing cache line bouncing with " << num_threads << " threads on different cores...\n";
        
        auto bouncing_test = [&](int thread_id) {
            // Set CPU affinity to ensure threads run on different cores
#ifdef __APPLE__
            thread_affinity_policy_data_t policy = { thread_id % sys_info_.num_cores };
            thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
            thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, 
                              (thread_policy_t)&policy, 1);
#else
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(thread_id % sys_info_.num_cores, &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
            
            auto start = Clock::now();
            
            for (int i = 0; i < 50000; ++i) {
                bouncing_data.counter.fetch_add(1, std::memory_order_relaxed);
                
                // Add computation to make bouncing more visible
                for (int j = 0; j < 100; ++j) {
                    bouncing_data.counter.fetch_add(j % 7, std::memory_order_relaxed);
                }
            }
            
            auto end = Clock::now();
            return Duration(end - start).count();
        };
        
        std::vector<std::future<double>> futures;
        auto total_start = Clock::now();
        
        for (int t = 0; t < num_threads; ++t) {
            futures.push_back(std::async(std::launch::async, bouncing_test, t));
        }
        
        std::vector<double> thread_times;
        for (auto& future : futures) {
            thread_times.push_back(future.get());
        }
        
        auto total_end = Clock::now();
        double total_time = Duration(total_end - total_start).count();
        
        std::cout << "Cache bouncing test completed in " << std::fixed << std::setprecision(3) << total_time << " seconds\n";
        std::cout << "Final counter value: " << bouncing_data.counter.load() << "\n";
        
        double total_throughput = 0.0;
        for (int t = 0; t < num_threads; ++t) {
            double ops_per_sec = (50000 * 101) / thread_times[t];
            std::cout << "Thread " << t << " (CPU " << t % sys_info_.num_cores << "): " 
                      << std::fixed << std::setprecision(0) << ops_per_sec << " ops/sec\n";
            total_throughput += ops_per_sec;
        }
        
        std::cout << "Total system throughput: " << total_throughput << " ops/sec\n";
        std::cout << "Average per-thread throughput: " << total_throughput / num_threads << " ops/sec\n";
    }
    
    void prefetcher_analysis() {
        std::cout << "\n=== Hardware Prefetcher Analysis ===\n";
        
        size_t test_size = 4 * MB;
        AlignedVector<char> data(test_size);
        std::iota(data.begin(), data.end(), 0);
        
        std::vector<size_t> strides = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
        
        std::cout << std::left << std::setw(15) << "Stride (bytes)"
                  << std::setw(15) << "Latency (ns)"
                  << std::setw(15) << "Throughput"
                  << std::setw(25) << "Prefetcher Effectiveness" << "\n";
        
        std::string separator(70, '-');
        std::cout << separator << "\n";
        
        for (size_t stride : strides) {
            auto start = Clock::now();
            
            volatile char sum = 0;
            uint64_t accesses = 0;
            
            for (size_t i = 0; i < test_size; i += stride) {
                sum += data[i];
                ++accesses;
            }
            
            auto end = Clock::now();
            
            double time = Duration(end - start).count();
            double latency = time / accesses;
            double throughput = accesses / time;
            
            std::string effectiveness;
            if (stride <= 64) {
                effectiveness = "High";
            } else if (stride <= 256) {
                effectiveness = "Medium";
            } else {
                effectiveness = "Low";
            }
            
            std::cout << std::left << std::setw(15) << stride
                      << std::setw(15) << std::fixed << std::setprecision(2) << latency * 1e9
                      << std::setw(15) << std::setprecision(0) << throughput
                      << std::setw(25) << effectiveness << "\n";
        }
    }
    
    void run_all_tests(int num_threads) {
        cache_hierarchy_analysis(num_threads);
        false_sharing_analysis(num_threads);
        cache_bouncing_analysis(num_threads);
        prefetcher_analysis();
    }
};

void print_usage(const std::string& prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n";
    std::cout << "Options:\n";
    std::cout << "  -t <type>     Test type: hierarchy, false_sharing, bouncing, prefetcher, all\n";
    std::cout << "  -T <threads>  Number of threads (default: 4)\n";
    std::cout << "  -v            Verbose output\n";
    std::cout << "  -h            Show this help message\n";
    std::cout << "\nAnalyzes CPU cache performance characteristics:\n";
    std::cout << "  - Cache hierarchy (L1/L2/L3) performance\n";
    std::cout << "  - False sharing detection and analysis\n";
    std::cout << "  - Cache line bouncing between cores\n";
    std::cout << "  - Hardware prefetcher effectiveness\n";
    std::cout << "  - Template-based access pattern analysis\n";
}

} // namespace CacheAnalysis

int main(int argc, char* argv[]) {
    using namespace CacheAnalysis;
    
    TestType test_type = TestType::All;
    int num_threads = 4;
    bool verbose = false;
    
    int opt;
    while ((opt = getopt(argc, argv, "t:T:vh")) != -1) {
        switch (opt) {
            case 't':
                if (std::string_view(optarg) == "hierarchy") test_type = TestType::CacheHierarchy;
                else if (std::string_view(optarg) == "false_sharing") test_type = TestType::FalseSharing;
                else if (std::string_view(optarg) == "bouncing") test_type = TestType::CacheBouncing;
                else if (std::string_view(optarg) == "prefetcher") test_type = TestType::Prefetcher;
                else if (std::string_view(optarg) == "all") test_type = TestType::All;
                else {
                    std::cerr << "Unknown test type: " << optarg << "\n";
                    return 1;
                }
                break;
            case 'T':
                num_threads = std::stoi(optarg);
                if (num_threads <= 0 || num_threads > static_cast<int>(MAX_THREADS)) {
                    std::cerr << "Invalid number of threads: " << num_threads << "\n";
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
    
    std::cout << "CPU Cache Performance Analyzer (C++)\n";
    std::cout << "====================================\n\n";
    
    try {
        CacheAnalyzer analyzer(verbose);
        
        switch (test_type) {
            case TestType::CacheHierarchy:
                analyzer.cache_hierarchy_analysis(num_threads);
                break;
            case TestType::FalseSharing:
                analyzer.false_sharing_analysis(num_threads);
                break;
            case TestType::CacheBouncing:
                analyzer.cache_bouncing_analysis(num_threads);
                break;
            case TestType::Prefetcher:
                analyzer.prefetcher_analysis();
                break;
            case TestType::NUMAAnalysis:
                std::cout << "NUMA analysis not yet implemented in C++ version.\n";
                break;
            case TestType::All:
                analyzer.run_all_tests(num_threads);
                break;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}