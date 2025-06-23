#include <iostream>
#include <memory>
#include <vector>
#include <array>
#include <chrono>
#include <random>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <cstring>
#include <functional>
#include <optional>
#include <variant>
#include <string_view>
#include <map>
#include <unordered_map>
#include <memory_resource>
#include <sys/resource.h>
#include <unistd.h>

#ifdef __cpp_lib_barrier
#include <barrier>
#endif

namespace MemoryBenchmark {

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = Clock::time_point;

constexpr size_t DEFAULT_ITERATIONS = 1000;
constexpr size_t MAX_THREADS = 32;
constexpr size_t HISTOGRAM_BUCKETS = 50;
constexpr size_t CACHE_LINE_SIZE = 64;

enum class AllocationPattern {
    Sequential,
    Random,
    Exponential,
    Bimodal,
    Realistic
};

enum class TestType {
    Speed,
    Fragmentation,
    Scalability,
    Containers,
    CustomAllocators,
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

struct AllocationRecord {
    std::unique_ptr<char[]> ptr;
    size_t size;
    TimePoint alloc_time;
    TimePoint free_time;
    int thread_id;
    
    AllocationRecord(size_t s, int tid) 
        : ptr(std::make_unique<char[]>(s)), size(s), thread_id(tid) {
        alloc_time = Clock::now();
        std::memset(ptr.get(), 0xAA, size);
    }
};

struct Statistics {
    std::atomic<size_t> total_allocated{0};
    std::atomic<size_t> peak_allocated{0};
    std::atomic<size_t> current_allocated{0};
    std::atomic<size_t> allocation_count{0};
    std::atomic<size_t> free_count{0};
    std::atomic<size_t> failed_allocations{0};
    
    double total_alloc_time{0};
    double total_free_time{0};
    double min_alloc_time{std::numeric_limits<double>::max()};
    double max_alloc_time{0};
    double min_free_time{std::numeric_limits<double>::max()};
    double max_free_time{0};
    
    std::mutex stat_mutex;
    std::vector<double> alloc_times;
    std::vector<double> free_times;
    
    void update_allocation(size_t size, double alloc_time) {
        std::lock_guard<std::mutex> lock(stat_mutex);
        total_allocated += size;
        current_allocated += size;
        allocation_count++;
        total_alloc_time += alloc_time;
        alloc_times.push_back(alloc_time);
        
        if (current_allocated > peak_allocated) {
            peak_allocated = current_allocated.load();
        }
        
        min_alloc_time = std::min(min_alloc_time, alloc_time);
        max_alloc_time = std::max(max_alloc_time, alloc_time);
    }
    
    void update_free(size_t size, double free_time) {
        std::lock_guard<std::mutex> lock(stat_mutex);
        current_allocated -= size;
        free_count++;
        total_free_time += free_time;
        free_times.push_back(free_time);
        
        min_free_time = std::min(min_free_time, free_time);
        max_free_time = std::max(max_free_time, free_time);
    }
    
    double get_percentile(const std::vector<double>& times, double percentile) const {
        if (times.empty()) return 0.0;
        
        std::vector<double> sorted_times = times;
        std::sort(sorted_times.begin(), sorted_times.end());
        
        size_t index = static_cast<size_t>(percentile * sorted_times.size() / 100.0);
        return sorted_times[std::min(index, sorted_times.size() - 1)];
    }
};

class PatternGenerator {
private:
    std::mt19937 rng;
    std::uniform_real_distribution<> uniform_dist{0.0, 1.0};
    
public:
    PatternGenerator() : rng(std::chrono::steady_clock::now().time_since_epoch().count()) {}
    
    size_t generate(AllocationPattern pattern, size_t min_size, size_t max_size, int iteration) {
        size_t range = max_size - min_size;
        
        switch (pattern) {
            case AllocationPattern::Sequential:
                return min_size + (iteration % range);
                
            case AllocationPattern::Random:
                return min_size + (rng() % range);
                
            case AllocationPattern::Exponential: {
                double r = uniform_dist(rng);
                return min_size + static_cast<size_t>(range * (1.0 - std::exp(-3.0 * r)) / (1.0 - std::exp(-3.0)));
            }
            
            case AllocationPattern::Bimodal:
                if (rng() % 100 < 80) {
                    return min_size + (rng() % (range / 10));
                } else {
                    return max_size - (rng() % (range / 10));
                }
                
            case AllocationPattern::Realistic: {
                int choice = rng() % 100;
                if (choice < 40) return 16 + (rng() % 48);
                if (choice < 70) return 64 + (rng() % 192);
                if (choice < 90) return 256 + (rng() % 768);
                if (choice < 98) return 1024 + (rng() % 3072);
                return 4096 + (rng() % 12288);
            }
        }
        
        return min_size;
    }
};

class MemoryBenchmarker {
private:
    Statistics global_stats;
    std::vector<std::unique_ptr<Statistics>> thread_stats;
    std::array<double, HISTOGRAM_BUCKETS> size_histogram{};
    std::array<double, HISTOGRAM_BUCKETS> time_histogram{};
    size_t histogram_max_size;
    std::mutex histogram_mutex;
    
    void update_histogram(size_t size, double time) {
        std::lock_guard<std::mutex> lock(histogram_mutex);
        
        int size_bucket = (size * HISTOGRAM_BUCKETS) / histogram_max_size;
        if (size_bucket >= static_cast<int>(HISTOGRAM_BUCKETS)) size_bucket = HISTOGRAM_BUCKETS - 1;
        size_histogram[size_bucket]++;
        
        int time_bucket = static_cast<int>(time * 1e6 / 10.0);
        if (time_bucket >= static_cast<int>(HISTOGRAM_BUCKETS)) time_bucket = HISTOGRAM_BUCKETS - 1;
        if (time_bucket < 0) time_bucket = 0;
        time_histogram[time_bucket]++;
    }
    
    double calculate_fragmentation() {
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);
        
        size_t rss = usage.ru_maxrss;
        #ifdef __APPLE__
        rss = rss / 1024;
        #endif
        
        size_t heap_used = global_stats.current_allocated;
        
        if (heap_used == 0) return 0.0;
        
        double fragmentation = ((static_cast<double>(rss * 1024) / heap_used) - 1.0) * 100.0;
        return fragmentation;
    }
    
public:
    MemoryBenchmarker(size_t max_size) : histogram_max_size(max_size) {
        for (size_t i = 0; i < MAX_THREADS; i++) {
            thread_stats.push_back(std::make_unique<Statistics>());
        }
    }
    
    void speed_test(AllocationPattern pattern, size_t min_size, size_t max_size, int iterations) {
        std::cout << "\n=== Speed Test ===\n";
        
        PatternGenerator gen;
        std::vector<std::unique_ptr<char[]>> allocations;
        allocations.reserve(iterations);
        
        auto start_time = Clock::now();
        
        for (int i = 0; i < iterations; i++) {
            size_t size = gen.generate(pattern, min_size, max_size, i);
            
            auto alloc_start = Clock::now();
            try {
                auto ptr = std::make_unique<char[]>(size);
                auto alloc_end = Clock::now();
                
                double alloc_time = std::chrono::duration<double>(alloc_end - alloc_start).count();
                
                std::memset(ptr.get(), 0xAA, size);
                allocations.push_back(std::move(ptr));
                
                global_stats.update_allocation(size, alloc_time);
                update_histogram(size, alloc_time);
                
                if (i % 2 == 0 && i > 0) {
                    int free_idx = gen.generate(AllocationPattern::Random, 0, allocations.size() - 1, i);
                    if (allocations[free_idx]) {
                        auto free_start = Clock::now();
                        allocations[free_idx].reset();
                        auto free_end = Clock::now();
                        
                        double free_time = std::chrono::duration<double>(free_end - free_start).count();
                        global_stats.update_free(size, free_time);
                    }
                }
            } catch (const std::bad_alloc&) {
                global_stats.failed_allocations++;
            }
        }
        
        auto end_time = Clock::now();
        double total_time = std::chrono::duration<double>(end_time - start_time).count();
        
        std::cout << "Completed " << iterations << " iterations in " 
                  << std::fixed << std::setprecision(3) << total_time << " seconds\n";
        std::cout << "Operations per second: " 
                  << static_cast<int>((global_stats.allocation_count + global_stats.free_count) / total_time) << "\n";
    }
    
    void fragmentation_test(size_t min_size, size_t max_size) {
        std::cout << "\n=== Fragmentation Test ===\n";
        
        PatternGenerator gen;
        const int num_allocations = 10000;
        
        for (int round = 0; round < 5; round++) {
            std::cout << "\nRound " << round + 1 << ":\n";
            
            std::vector<std::unique_ptr<char[]>> ptrs;
            std::vector<size_t> sizes;
            
            for (int i = 0; i < num_allocations; i++) {
                size_t size = gen.generate(AllocationPattern::Random, min_size, max_size, i);
                try {
                    auto ptr = std::make_unique<char[]>(size);
                    std::memset(ptr.get(), 0xBB, size);
                    ptrs.push_back(std::move(ptr));
                    sizes.push_back(size);
                    global_stats.current_allocated += size;
                } catch (const std::bad_alloc&) {
                    ptrs.push_back(nullptr);
                    sizes.push_back(0);
                }
            }
            
            double frag_before = calculate_fragmentation();
            std::cout << "  Fragmentation before freeing: " << std::fixed << std::setprecision(2) 
                      << frag_before << "%\n";
            
            if (round % 2 == 0) {
                for (int i = 0; i < num_allocations; i += 2) {
                    if (ptrs[i]) {
                        global_stats.current_allocated -= sizes[i];
                        ptrs[i].reset();
                    }
                }
            } else {
                for (int i = num_allocations - 1; i >= 0; i--) {
                    if (gen.generate(AllocationPattern::Random, 0, 2, i) == 0 && ptrs[i]) {
                        global_stats.current_allocated -= sizes[i];
                        ptrs[i].reset();
                    }
                }
            }
            
            double frag_after = calculate_fragmentation();
            std::cout << "  Fragmentation after partial free: " << frag_after << "%\n";
            
            size_t holes = 0;
            size_t total_hole_size = 0;
            for (size_t i = 0; i < ptrs.size(); i++) {
                if (!ptrs[i] && sizes[i] > 0) {
                    holes++;
                    total_hole_size += sizes[i];
                }
            }
            std::cout << "  Memory holes: " << holes << ", Total hole size: " 
                      << std::fixed << std::setprecision(2) 
                      << total_hole_size / (1024.0 * 1024.0) << " MB\n";
        }
    }
    
    void scalability_test(AllocationPattern pattern, size_t min_size, size_t max_size) {
        std::cout << "\n=== Scalability Test ===\n";
        
        std::array<int, 5> thread_counts = {1, 2, 4, 8, 16};
        
        for (int num_threads : thread_counts) {
            if (num_threads > static_cast<int>(MAX_THREADS)) continue;
            
            std::cout << "\n" << num_threads << " Thread(s):\n";
            
            // Reset thread stats
            thread_stats.clear();
            for (size_t i = 0; i < MAX_THREADS; i++) {
                thread_stats.push_back(std::make_unique<Statistics>());
            }
            
            std::vector<std::thread> threads;
            std::mutex start_mutex;
            std::condition_variable start_cv;
            bool ready = false;
            
            auto thread_work = [&](int thread_id) {
                PatternGenerator gen;
                int iterations_per_thread = DEFAULT_ITERATIONS / num_threads;
                
                {
                    std::unique_lock<std::mutex> lock(start_mutex);
                    start_cv.wait(lock, [&ready] { return ready; });
                }
                
                for (int i = 0; i < iterations_per_thread; i++) {
                    size_t size = gen.generate(pattern, min_size, max_size, i);
                    
                    auto alloc_start = Clock::now();
                    try {
                        auto ptr = std::make_unique<char[]>(size);
                        auto alloc_end = Clock::now();
                        
                        double alloc_time = std::chrono::duration<double>(alloc_end - alloc_start).count();
                        std::memset(ptr.get(), 0xCC, size);
                        
                        thread_stats[thread_id]->update_allocation(size, alloc_time);
                        
                        auto free_start = Clock::now();
                        ptr.reset();
                        auto free_end = Clock::now();
                        
                        double free_time = std::chrono::duration<double>(free_end - free_start).count();
                        thread_stats[thread_id]->update_free(size, free_time);
                        
                    } catch (const std::bad_alloc&) {
                        thread_stats[thread_id]->failed_allocations++;
                    }
                }
            };
            
            auto start_time = Clock::now();
            
            for (int i = 0; i < num_threads; i++) {
                threads.emplace_back(thread_work, i);
            }
            
            {
                std::lock_guard<std::mutex> lock(start_mutex);
                ready = true;
            }
            start_cv.notify_all();
            
            for (auto& t : threads) {
                t.join();
            }
            
            auto end_time = Clock::now();
            double total_time = std::chrono::duration<double>(end_time - start_time).count();
            
            size_t total_ops = 0;
            double total_alloc_time = 0;
            double total_free_time = 0;
            
            for (int i = 0; i < num_threads; i++) {
                total_ops += thread_stats[i]->allocation_count;
                total_ops += thread_stats[i]->free_count;
                total_alloc_time += thread_stats[i]->total_alloc_time;
                total_free_time += thread_stats[i]->total_free_time;
            }
            
            std::cout << "  Total operations: " << total_ops << "\n";
            std::cout << "  Operations/second: " << static_cast<int>(total_ops / total_time) << "\n";
            
            if (thread_stats[0]->allocation_count > 0) {
                std::cout << "  Avg allocation time: " << std::fixed << std::setprecision(2)
                          << (total_alloc_time / (total_ops / 2)) * 1e6 << " μs\n";
            }
            
            if (thread_stats[0]->free_count > 0) {
                std::cout << "  Avg free time: "
                          << (total_free_time / (total_ops / 2)) * 1e6 << " μs\n";
            }
        }
    }
    
    void container_test() {
        std::cout << "\n=== STL Container Allocation Test ===\n";
        
        const int iterations = 10000;
        
        auto test_container = [](const std::string& name, auto test_func) {
            auto start = Clock::now();
            test_func();
            auto end = Clock::now();
            
            double time = std::chrono::duration<double>(end - start).count();
            std::cout << "  " << name << ": " << std::fixed << std::setprecision(3) 
                      << time * 1000 << " ms\n";
        };
        
        test_container("vector<int>", [=]() {
            for (int i = 0; i < iterations; i++) {
                std::vector<int> v;
                for (int j = 0; j < 100; j++) {
                    v.push_back(j);
                }
            }
        });
        
        test_container("vector<int> with reserve", [=]() {
            for (int i = 0; i < iterations; i++) {
                std::vector<int> v;
                v.reserve(100);
                for (int j = 0; j < 100; j++) {
                    v.push_back(j);
                }
            }
        });
        
        test_container("unordered_map<int,int>", [=]() {
            for (int i = 0; i < iterations; i++) {
                std::unordered_map<int, int> m;
                for (int j = 0; j < 100; j++) {
                    m[j] = j * 2;
                }
            }
        });
        
        test_container("map<int,int>", [=]() {
            for (int i = 0; i < iterations; i++) {
                std::map<int, int> m;
                for (int j = 0; j < 100; j++) {
                    m[j] = j * 2;
                }
            }
        });
    }
    
    void custom_allocator_test() {
        std::cout << "\n=== Custom Allocator Test ===\n";
        
        const int iterations = 10000;
        
        std::cout << "Standard allocator:\n";
        auto start = Clock::now();
        for (int i = 0; i < iterations; i++) {
            std::vector<int> v;
            for (int j = 0; j < 100; j++) {
                v.push_back(j);
            }
        }
        auto end = Clock::now();
        double std_time = std::chrono::duration<double>(end - start).count();
        std::cout << "  Time: " << std_time * 1000 << " ms\n";
        
        std::cout << "\nAligned allocator:\n";
        start = Clock::now();
        for (int i = 0; i < iterations; i++) {
            std::vector<int, AlignedAllocator<int>> v;
            for (int j = 0; j < 100; j++) {
                v.push_back(j);
            }
        }
        end = Clock::now();
        double aligned_time = std::chrono::duration<double>(end - start).count();
        std::cout << "  Time: " << aligned_time * 1000 << " ms\n";
        std::cout << "  Overhead: " << ((aligned_time / std_time) - 1.0) * 100 << "%\n";
        
        std::cout << "\nPMR allocator with pool:\n";
        std::array<std::byte, 1024 * 1024> buffer;
        std::pmr::monotonic_buffer_resource pool{buffer.data(), buffer.size()};
        
        start = Clock::now();
        for (int i = 0; i < iterations; i++) {
            std::pmr::vector<int> v{&pool};
            for (int j = 0; j < 100; j++) {
                v.push_back(j);
            }
        }
        end = Clock::now();
        double pmr_time = std::chrono::duration<double>(end - start).count();
        std::cout << "  Time: " << pmr_time * 1000 << " ms\n";
        std::cout << "  Speedup: " << (std_time / pmr_time) << "x\n";
    }
    
    void print_summary() {
        std::cout << "\n=== Summary Statistics ===\n";
        
        std::cout << "\nMemory Usage:\n";
        std::cout << "  Total allocated: " << std::fixed << std::setprecision(2)
                  << global_stats.total_allocated / (1024.0 * 1024.0) << " MB\n";
        std::cout << "  Peak allocated: "
                  << global_stats.peak_allocated / (1024.0 * 1024.0) << " MB\n";
        std::cout << "  Total allocations: " << global_stats.allocation_count << "\n";
        std::cout << "  Failed allocations: " << global_stats.failed_allocations << "\n";
        
        if (global_stats.allocation_count > 0) {
            std::cout << "\nAllocation Performance:\n";
            std::cout << "  Average time: " << std::fixed << std::setprecision(2)
                      << (global_stats.total_alloc_time / global_stats.allocation_count) * 1e6 << " μs\n";
            std::cout << "  Min time: " << global_stats.min_alloc_time * 1e6 << " μs\n";
            std::cout << "  Max time: " << global_stats.max_alloc_time * 1e6 << " μs\n";
            std::cout << "  50th percentile: " 
                      << global_stats.get_percentile(global_stats.alloc_times, 50) * 1e6 << " μs\n";
            std::cout << "  90th percentile: " 
                      << global_stats.get_percentile(global_stats.alloc_times, 90) * 1e6 << " μs\n";
            std::cout << "  99th percentile: " 
                      << global_stats.get_percentile(global_stats.alloc_times, 99) * 1e6 << " μs\n";
        }
        
        if (global_stats.free_count > 0) {
            std::cout << "\nFree Performance:\n";
            std::cout << "  Average time: "
                      << (global_stats.total_free_time / global_stats.free_count) * 1e6 << " μs\n";
            std::cout << "  Min time: " << global_stats.min_free_time * 1e6 << " μs\n";
            std::cout << "  Max time: " << global_stats.max_free_time * 1e6 << " μs\n";
        }
        
        print_histogram("Size Distribution", size_histogram, true);
        print_histogram("Time Distribution", time_histogram, false);
    }
    
private:
    void print_histogram(const std::string& title, const std::array<double, HISTOGRAM_BUCKETS>& histogram, bool is_size) {
        std::cout << "\n" << title << ":\n";
        
        double max_count = *std::max_element(histogram.begin(), histogram.end());
        if (max_count == 0) return;
        
        for (size_t i = 0; i < HISTOGRAM_BUCKETS; i++) {
            if (histogram[i] == 0) continue;
            
            if (is_size) {
                size_t bucket_size = (histogram_max_size * i) / HISTOGRAM_BUCKETS;
                std::cout << std::setw(6) << bucket_size / 1024 << " KB: ";
            } else {
                std::cout << std::setw(3) << i * 10 << "-" << std::setw(3) << (i + 1) * 10 << " μs: ";
            }
            
            int bar_len = static_cast<int>((histogram[i] / max_count) * 50);
            for (int j = 0; j < bar_len; j++) std::cout << "█";
            std::cout << " " << static_cast<int>(histogram[i]) << "\n";
        }
    }
};

void print_usage(const std::string& prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n";
    std::cout << "Options:\n";
    std::cout << "  -t <type>     Test type: speed, frag, scale, container, custom, all (default: all)\n";
    std::cout << "  -p <pattern>  Allocation pattern: seq, rand, exp, bimodal, real (default: real)\n";
    std::cout << "  -s <size>     Min allocation size in bytes (default: 8)\n";
    std::cout << "  -S <size>     Max allocation size in bytes (default: 8192)\n";
    std::cout << "  -n <count>    Number of iterations (default: 1000)\n";
    std::cout << "  -h            Show this help message\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << prog_name << " -t speed -p rand -n 10000\n";
    std::cout << "  " << prog_name << " -t scale\n";
    std::cout << "  " << prog_name << " -t container\n";
}

} // namespace MemoryBenchmark

int main(int argc, char* argv[]) {
    using namespace MemoryBenchmark;
    
    TestType test_type = TestType::All;
    AllocationPattern pattern = AllocationPattern::Realistic;
    size_t min_size = 8;
    size_t max_size = 8192;
    int num_iterations = DEFAULT_ITERATIONS;
    
    int opt;
    while ((opt = getopt(argc, argv, "t:p:s:S:n:h")) != -1) {
        switch (opt) {
            case 't':
                if (std::string_view(optarg) == "speed") test_type = TestType::Speed;
                else if (std::string_view(optarg) == "frag") test_type = TestType::Fragmentation;
                else if (std::string_view(optarg) == "scale") test_type = TestType::Scalability;
                else if (std::string_view(optarg) == "container") test_type = TestType::Containers;
                else if (std::string_view(optarg) == "custom") test_type = TestType::CustomAllocators;
                else if (std::string_view(optarg) == "all") test_type = TestType::All;
                break;
                
            case 'p':
                if (std::string_view(optarg) == "seq") pattern = AllocationPattern::Sequential;
                else if (std::string_view(optarg) == "rand") pattern = AllocationPattern::Random;
                else if (std::string_view(optarg) == "exp") pattern = AllocationPattern::Exponential;
                else if (std::string_view(optarg) == "bimodal") pattern = AllocationPattern::Bimodal;
                else if (std::string_view(optarg) == "real") pattern = AllocationPattern::Realistic;
                break;
                
            case 's':
                min_size = std::stoul(optarg);
                break;
                
            case 'S':
                max_size = std::stoul(optarg);
                break;
                
            case 'n':
                num_iterations = std::stoi(optarg);
                break;
                
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }
    
    std::cout << "Memory Allocator Benchmarker (C++)\n";
    std::cout << "==================================\n";
    
    MemoryBenchmarker benchmarker(max_size);
    
    if (test_type == TestType::Speed || test_type == TestType::All) {
        benchmarker.speed_test(pattern, min_size, max_size, num_iterations);
    }
    
    if (test_type == TestType::Fragmentation || test_type == TestType::All) {
        benchmarker.fragmentation_test(min_size, max_size);
    }
    
    if (test_type == TestType::Scalability || test_type == TestType::All) {
        benchmarker.scalability_test(pattern, min_size, max_size);
    }
    
    if (test_type == TestType::Containers || test_type == TestType::All) {
        benchmarker.container_test();
    }
    
    if (test_type == TestType::CustomAllocators || test_type == TestType::All) {
        benchmarker.custom_allocator_test();
    }
    
    if (test_type == TestType::Speed || test_type == TestType::All) {
        benchmarker.print_summary();
    }
    
    return 0;
}