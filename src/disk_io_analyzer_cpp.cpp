#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>
#include <memory>
#include <algorithm>
#include <numeric>
#include <random>
#include <iomanip>
#include <fstream>
#include <map>
#include <queue>
#include <condition_variable>
#include <mutex>
#include <barrier>
#include <filesystem>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>

#ifdef __APPLE__
#include <sys/disk.h>
#include <sys/mount.h>
#else
#include <sys/statvfs.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

namespace DiskIOAnalysis {

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = Clock::time_point;

constexpr size_t KB = 1024;
constexpr size_t MB = 1024 * 1024;
constexpr size_t GB = 1024 * 1024 * 1024;
constexpr size_t DEFAULT_FILE_SIZE = 100 * MB;
constexpr int DEFAULT_TEST_DURATION = 10;
constexpr size_t MAX_THREADS = 64;
constexpr size_t HISTOGRAM_BUCKETS = 50;
constexpr size_t ALIGNMENT = 4096;

enum class IOPattern {
    SequentialRead,
    SequentialWrite,
    RandomRead,
    RandomWrite,
    Mixed
};

enum class IOMode {
    Buffered,
    Direct,
    Synchronous
};

enum class TestType {
    PatternAnalysis,
    BlockSizeAnalysis,
    SyncOverhead,
    IOModeComparison,
    QueueDepthAnalysis,
    All
};

struct IOStatistics {
    std::atomic<uint64_t> bytes_read{0};
    std::atomic<uint64_t> bytes_written{0};
    std::atomic<uint64_t> read_operations{0};
    std::atomic<uint64_t> write_operations{0};
    std::atomic<double> total_time{0.0};
    std::atomic<double> min_latency{std::numeric_limits<double>::infinity()};
    std::atomic<double> max_latency{0.0};
    std::atomic<double> total_latency{0.0};
    std::atomic<double> sync_time{0.0};
    
    int thread_id{0};
    IOPattern pattern{IOPattern::SequentialRead};
    IOMode mode{IOMode::Buffered};
    size_t block_size{0};
    
    std::vector<double> latency_samples;
    std::mutex latency_mutex;
    
    void add_latency(double latency) {
        double current_total = total_latency.load(std::memory_order_relaxed);
        while (!total_latency.compare_exchange_weak(current_total, current_total + latency, std::memory_order_relaxed)) {
            // Keep trying
        }
        
        double current_min = min_latency.load(std::memory_order_relaxed);
        while (latency < current_min && 
               !min_latency.compare_exchange_weak(current_min, latency, std::memory_order_relaxed)) {
            // Keep trying
        }
        
        double current_max = max_latency.load(std::memory_order_relaxed);
        while (latency > current_max && 
               !max_latency.compare_exchange_weak(current_max, latency, std::memory_order_relaxed)) {
            // Keep trying
        }
        
        std::lock_guard<std::mutex> lock(latency_mutex);
        latency_samples.push_back(latency);
    }
    
    std::vector<double> get_percentiles() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(latency_mutex));
        if (latency_samples.empty()) return {};
        
        std::vector<double> sorted_samples = latency_samples;
        std::sort(sorted_samples.begin(), sorted_samples.end());
        
        std::vector<double> percentiles;
        std::vector<double> p_values = {0.5, 0.75, 0.9, 0.95, 0.99};
        
        for (double p : p_values) {
            size_t index = static_cast<size_t>(p * sorted_samples.size());
            if (index >= sorted_samples.size()) index = sorted_samples.size() - 1;
            percentiles.push_back(sorted_samples[index]);
        }
        
        return percentiles;
    }
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
        void* ptr = std::aligned_alloc(ALIGNMENT, n * sizeof(T));
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

class FileManager {
public:
    static bool create_test_file(const std::string& filename, size_t size) {
        std::ofstream file(filename, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "Failed to create test file: " << filename << std::endl;
            return false;
        }
        
        AlignedVector<char> buffer(MB, 0);
        size_t remaining = size;
        
        while (remaining > 0) {
            size_t write_size = std::min(remaining, static_cast<size_t>(MB));
            file.write(buffer.data(), write_size);
            if (!file) {
                std::cerr << "Failed to write to test file: " << filename << std::endl;
                return false;
            }
            remaining -= write_size;
        }
        
        file.close();
        sync(); // Ensure data is written to disk
        return true;
    }
    
    static void cleanup_test_files(const std::string& base_filename, int num_threads) {
        for (int i = 0; i < num_threads; ++i) {
            std::string thread_filename = base_filename + "." + std::to_string(i);
            std::filesystem::remove(thread_filename);
        }
    }
};

template<IOPattern Pattern>
class IOPatternExecutor {
public:
    static uint64_t execute(int fd, AlignedVector<char>& buffer, size_t file_size, 
                           size_t block_size, uint64_t& operations, IOStatistics& stats,
                           const std::atomic<bool>& stop_flag) {
        uint64_t bytes_transferred = 0;
        size_t file_offset = 0;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> offset_dist(0, (file_size / block_size) - 1);
        
        while (!stop_flag.load(std::memory_order_relaxed)) {
            auto start_time = Clock::now();
            ssize_t result = 0;
            
            if constexpr (Pattern == IOPattern::SequentialRead) {
                result = pread(fd, buffer.data(), block_size, file_offset);
                if (result > 0) {
                    stats.bytes_read.fetch_add(result, std::memory_order_relaxed);
                    stats.read_operations.fetch_add(1, std::memory_order_relaxed);
                }
                file_offset = (file_offset + block_size) % file_size;
                
            } else if constexpr (Pattern == IOPattern::SequentialWrite) {
                std::iota(buffer.begin(), buffer.begin() + block_size, operations & 0xFF);
                result = pwrite(fd, buffer.data(), block_size, file_offset);
                if (result > 0) {
                    stats.bytes_written.fetch_add(result, std::memory_order_relaxed);
                    stats.write_operations.fetch_add(1, std::memory_order_relaxed);
                }
                file_offset = (file_offset + block_size) % file_size;
                
            } else if constexpr (Pattern == IOPattern::RandomRead) {
                file_offset = offset_dist(gen) * block_size;
                result = pread(fd, buffer.data(), block_size, file_offset);
                if (result > 0) {
                    stats.bytes_read.fetch_add(result, std::memory_order_relaxed);
                    stats.read_operations.fetch_add(1, std::memory_order_relaxed);
                }
                
            } else if constexpr (Pattern == IOPattern::RandomWrite) {
                file_offset = offset_dist(gen) * block_size;
                std::iota(buffer.begin(), buffer.begin() + block_size, operations & 0xFF);
                result = pwrite(fd, buffer.data(), block_size, file_offset);
                if (result > 0) {
                    stats.bytes_written.fetch_add(result, std::memory_order_relaxed);
                    stats.write_operations.fetch_add(1, std::memory_order_relaxed);
                }
                
            } else if constexpr (Pattern == IOPattern::Mixed) {
                if (operations % 3 == 0) {
                    // Write operation
                    file_offset = offset_dist(gen) * block_size;
                    std::iota(buffer.begin(), buffer.begin() + block_size, operations & 0xFF);
                    result = pwrite(fd, buffer.data(), block_size, file_offset);
                    if (result > 0) {
                        stats.bytes_written.fetch_add(result, std::memory_order_relaxed);
                        stats.write_operations.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    // Read operation
                    file_offset = offset_dist(gen) * block_size;
                    result = pread(fd, buffer.data(), block_size, file_offset);
                    if (result > 0) {
                        stats.bytes_read.fetch_add(result, std::memory_order_relaxed);
                        stats.read_operations.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            
            auto end_time = Clock::now();
            double latency = Duration(end_time - start_time).count();
            
            if (result > 0) {
                stats.add_latency(latency);
                bytes_transferred += result;
                ++operations;
            }
        }
        
        return bytes_transferred;
    }
};

class DiskIOAnalyzer {
private:
    std::string base_filename_;
    size_t file_size_;
    int num_threads_;
    int duration_seconds_;
    bool verbose_;
    
public:
    DiskIOAnalyzer(const std::string& filename, size_t file_size, int threads, int duration, bool verbose = false)
        : base_filename_(filename), file_size_(file_size), num_threads_(threads), 
          duration_seconds_(duration), verbose_(verbose) {}
    
    template<IOPattern Pattern>
    std::vector<IOStatistics> run_io_test(IOMode mode, size_t block_size) {
        std::vector<IOStatistics> stats(num_threads_);
        std::vector<std::future<void>> futures;
        std::atomic<bool> stop_flag{false};
        
        // Create barrier for thread synchronization
        std::barrier sync_barrier(num_threads_);
        
        // Start worker threads
        for (int i = 0; i < num_threads_; ++i) {
            std::string thread_filename = base_filename_ + "." + std::to_string(i);
            
            // Create test file for this thread
            if (!FileManager::create_test_file(thread_filename, file_size_)) {
                throw std::runtime_error("Failed to create test file: " + thread_filename);
            }
            
            futures.push_back(std::async(std::launch::async, [this, i, thread_filename, mode, block_size, &stats, &stop_flag, &sync_barrier]() {
                worker_thread<Pattern>(i, thread_filename, mode, block_size, stats[i], stop_flag, sync_barrier);
            }));
        }
        
        // Run test for specified duration
        std::this_thread::sleep_for(std::chrono::seconds(duration_seconds_));
        stop_flag.store(true);
        
        // Wait for all threads to complete
        for (auto& future : futures) {
            future.wait();
        }
        
        // Cleanup test files
        FileManager::cleanup_test_files(base_filename_, num_threads_);
        
        return stats;
    }
    
private:
    template<IOPattern Pattern>
    void worker_thread(int thread_id, const std::string& filename, IOMode mode, size_t block_size,
                      IOStatistics& stats, const std::atomic<bool>& stop_flag, std::barrier<>& sync_barrier) {
        
        // Open file with appropriate flags
        int flags = get_open_flags<Pattern>(mode);
        int fd = open(filename.c_str(), flags);
        if (fd < 0) {
            std::cerr << "Failed to open file " << filename << ": " << std::strerror(errno) << std::endl;
            return;
        }
        
        // Allocate aligned buffer
        AlignedVector<char> buffer(block_size);
        
        // Initialize statistics
        stats.thread_id = thread_id;
        stats.pattern = Pattern;
        stats.mode = mode;
        stats.block_size = block_size;
        
        // Wait for all threads to be ready
        sync_barrier.arrive_and_wait();
        
        auto start_time = Clock::now();
        uint64_t operations = 0;
        
        // Execute I/O pattern
        IOPatternExecutor<Pattern>::execute(
            fd, buffer, file_size_, block_size, operations, stats, stop_flag);
        
        auto end_time = Clock::now();
        stats.total_time.store(Duration(end_time - start_time).count());
        
        close(fd);
    }
    
    template<IOPattern Pattern>
    int get_open_flags(IOMode mode) const {
        int base_flags = 0;
        
        // Determine read/write flags based on pattern
        if constexpr (Pattern == IOPattern::SequentialRead || Pattern == IOPattern::RandomRead) {
            base_flags = O_RDONLY;
        } else {
            base_flags = O_RDWR;
        }
        
        // Add mode-specific flags
        switch (mode) {
            case IOMode::Buffered:
                return base_flags;
            case IOMode::Direct:
#ifdef O_DIRECT
                return base_flags | O_DIRECT;
#else
                return base_flags;
#endif
            case IOMode::Synchronous:
                return base_flags | O_SYNC;
        }
        
        return base_flags;
    }
    
public:
    void run_pattern_analysis() {
        std::cout << "\n=== I/O Pattern Analysis ===\n";
        
        struct PatternTest {
            std::string name;
            std::function<std::vector<IOStatistics>()> test_func;
        };
        
        std::vector<PatternTest> tests = {
            {"Sequential Read", [this]() { return run_io_test<IOPattern::SequentialRead>(IOMode::Buffered, 64 * KB); }},
            {"Sequential Write", [this]() { return run_io_test<IOPattern::SequentialWrite>(IOMode::Buffered, 64 * KB); }},
            {"Random Read", [this]() { return run_io_test<IOPattern::RandomRead>(IOMode::Buffered, 64 * KB); }},
            {"Random Write", [this]() { return run_io_test<IOPattern::RandomWrite>(IOMode::Buffered, 64 * KB); }},
            {"Mixed (33% write, 67% read)", [this]() { return run_io_test<IOPattern::Mixed>(IOMode::Buffered, 64 * KB); }}
        };
        
        for (const auto& test : tests) {
            auto stats = test.test_func();
            print_io_statistics(stats, test.name);
        }
    }
    
    void run_block_size_analysis() {
        std::cout << "\n=== Block Size Analysis ===\n";
        
        std::vector<size_t> block_sizes = {4*KB, 8*KB, 16*KB, 32*KB, 64*KB, 128*KB, 256*KB, 512*KB, 1*MB};
        
        std::cout << std::left << std::setw(12) << "Block Size"
                  << std::setw(15) << "Read MB/s"
                  << std::setw(15) << "Write MB/s"
                  << std::setw(15) << "Read IOPS"
                  << std::setw(15) << "Write IOPS" << "\n";
        std::cout << std::string(72, '-') << "\n";
        
        for (size_t block_size : block_sizes) {
            auto read_stats = run_io_test<IOPattern::SequentialRead>(IOMode::Buffered, block_size);
            auto write_stats = run_io_test<IOPattern::SequentialWrite>(IOMode::Buffered, block_size);
            
            auto read_metrics = calculate_aggregate_metrics(read_stats);
            auto write_metrics = calculate_aggregate_metrics(write_stats);
            
            std::cout << std::left << std::setw(12) << format_size(block_size)
                      << std::setw(15) << std::fixed << std::setprecision(1) << read_metrics.throughput_mbps
                      << std::setw(15) << write_metrics.throughput_mbps
                      << std::setw(15) << std::fixed << std::setprecision(0) << read_metrics.iops
                      << std::setw(15) << write_metrics.iops << "\n";
        }
    }
    
    void run_sync_overhead_analysis() {
        std::cout << "\n=== Sync Overhead Analysis ===\n";
        
        // Test with explicit fsync calls
        auto buffered_stats = run_io_test<IOPattern::SequentialWrite>(IOMode::Buffered, 64 * KB);
        auto sync_stats = run_io_test<IOPattern::SequentialWrite>(IOMode::Synchronous, 64 * KB);
        
        auto buffered_metrics = calculate_aggregate_metrics(buffered_stats);
        auto sync_metrics = calculate_aggregate_metrics(sync_stats);
        
        std::cout << "Buffered I/O:\n";
        std::cout << "  Throughput: " << std::fixed << std::setprecision(1) << buffered_metrics.throughput_mbps << " MB/s\n";
        std::cout << "  IOPS: " << std::fixed << std::setprecision(0) << buffered_metrics.iops << "\n";
        std::cout << "  Average latency: " << std::fixed << std::setprecision(3) << buffered_metrics.avg_latency * 1000 << " ms\n";
        
        std::cout << "\nSynchronous I/O:\n";
        std::cout << "  Throughput: " << std::fixed << std::setprecision(1) << sync_metrics.throughput_mbps << " MB/s\n";
        std::cout << "  IOPS: " << std::fixed << std::setprecision(0) << sync_metrics.iops << "\n";
        std::cout << "  Average latency: " << std::fixed << std::setprecision(3) << sync_metrics.avg_latency * 1000 << " ms\n";
        
        double sync_overhead = (sync_metrics.avg_latency - buffered_metrics.avg_latency) * 1000;
        std::cout << "\nSync overhead: " << std::fixed << std::setprecision(3) << sync_overhead << " ms per operation\n";
        std::cout << "Performance ratio: " << std::fixed << std::setprecision(2) 
                  << buffered_metrics.throughput_mbps / sync_metrics.throughput_mbps << "x\n";
    }
    
    void run_io_mode_comparison() {
        std::cout << "\n=== I/O Mode Comparison ===\n";
        
        std::vector<std::pair<IOMode, std::string>> modes = {
            {IOMode::Buffered, "Buffered I/O"},
            {IOMode::Direct, "Direct I/O"},
            {IOMode::Synchronous, "Synchronous I/O"}
        };
        
        std::cout << std::left << std::setw(20) << "Mode"
                  << std::setw(15) << "Read MB/s"
                  << std::setw(15) << "Write MB/s"
                  << std::setw(15) << "Avg Latency (ms)" << "\n";
        std::cout << std::string(65, '-') << "\n";
        
        for (const auto& [mode, name] : modes) {
            auto stats = run_io_test<IOPattern::Mixed>(mode, 64 * KB);
            auto metrics = calculate_aggregate_metrics(stats);
            
            double read_mbps = metrics.total_read_bytes / static_cast<double>(MB) / metrics.total_time;
            double write_mbps = metrics.total_write_bytes / static_cast<double>(MB) / metrics.total_time;
            
            std::cout << std::left << std::setw(20) << name
                      << std::setw(15) << std::fixed << std::setprecision(1) << read_mbps
                      << std::setw(15) << write_mbps
                      << std::setw(15) << std::setprecision(3) << metrics.avg_latency * 1000 << "\n";
        }
    }
    
    void run_all_tests() {
        run_pattern_analysis();
        run_block_size_analysis();
        run_sync_overhead_analysis();
        run_io_mode_comparison();
    }

private:
    struct AggregateMetrics {
        double total_time{0.0};
        uint64_t total_read_bytes{0};
        uint64_t total_write_bytes{0};
        uint64_t total_read_ops{0};
        uint64_t total_write_ops{0};
        double throughput_mbps{0.0};
        double iops{0.0};
        double avg_latency{0.0};
        double min_latency{std::numeric_limits<double>::infinity()};
        double max_latency{0.0};
    };
    
    AggregateMetrics calculate_aggregate_metrics(const std::vector<IOStatistics>& stats) const {
        AggregateMetrics metrics;
        double total_latency = 0.0;
        
        for (const auto& stat : stats) {
            metrics.total_read_bytes += stat.bytes_read.load();
            metrics.total_write_bytes += stat.bytes_written.load();
            metrics.total_read_ops += stat.read_operations.load();
            metrics.total_write_ops += stat.write_operations.load();
            total_latency += stat.total_latency.load();
            
            if (stat.total_time.load() > metrics.total_time) {
                metrics.total_time = stat.total_time.load();
            }
            if (stat.min_latency.load() < metrics.min_latency) {
                metrics.min_latency = stat.min_latency.load();
            }
            if (stat.max_latency.load() > metrics.max_latency) {
                metrics.max_latency = stat.max_latency.load();
            }
        }
        
        uint64_t total_ops = metrics.total_read_ops + metrics.total_write_ops;
        uint64_t total_bytes = metrics.total_read_bytes + metrics.total_write_bytes;
        
        if (metrics.total_time > 0) {
            metrics.throughput_mbps = (total_bytes / static_cast<double>(MB)) / metrics.total_time;
            metrics.iops = total_ops / metrics.total_time;
        }
        
        if (total_ops > 0) {
            metrics.avg_latency = total_latency / total_ops;
        }
        
        return metrics;
    }
    
    void print_io_statistics(const std::vector<IOStatistics>& stats, const std::string& test_name) const {
        std::cout << "\n=== " << test_name << " Results ===\n";
        
        auto metrics = calculate_aggregate_metrics(stats);
        uint64_t total_bytes = metrics.total_read_bytes + metrics.total_write_bytes;
        uint64_t total_ops = metrics.total_read_ops + metrics.total_write_ops;
        
        std::cout << "Duration: " << std::fixed << std::setprecision(2) << metrics.total_time << " seconds\n";
        std::cout << "Total operations: " << total_ops 
                  << " (reads: " << metrics.total_read_ops << ", writes: " << metrics.total_write_ops << ")\n";
        std::cout << "Total data: " << std::fixed << std::setprecision(2) << total_bytes / static_cast<double>(MB) << " MB"
                  << " (read: " << metrics.total_read_bytes / static_cast<double>(MB) << " MB"
                  << ", written: " << metrics.total_write_bytes / static_cast<double>(MB) << " MB)\n";
        
        std::cout << "Throughput: " << std::fixed << std::setprecision(2) << metrics.throughput_mbps << " MB/s\n";
        std::cout << "IOPS: " << std::fixed << std::setprecision(0) << metrics.iops 
                  << " (read: " << (metrics.total_time > 0 ? metrics.total_read_ops / metrics.total_time : 0)
                  << ", write: " << (metrics.total_time > 0 ? metrics.total_write_ops / metrics.total_time : 0) << ")\n";
        
        std::cout << "Average latency: " << std::fixed << std::setprecision(3) << metrics.avg_latency * 1000 << " ms\n";
        std::cout << "Min latency: " << std::fixed << std::setprecision(3) << metrics.min_latency * 1000 << " ms\n";
        std::cout << "Max latency: " << std::fixed << std::setprecision(3) << metrics.max_latency * 1000 << " ms\n";
        
        std::cout << "Block size: " << format_size(stats[0].block_size) << "\n";
        std::cout << "Threads: " << num_threads_ << "\n";
        
        if (verbose_ && !stats.empty()) {
            auto percentiles = stats[0].get_percentiles();
            if (!percentiles.empty()) {
                std::cout << "\nLatency Percentiles (Thread 0):\n";
                std::vector<std::string> labels = {"50th", "75th", "90th", "95th", "99th"};
                for (size_t i = 0; i < percentiles.size() && i < labels.size(); ++i) {
                    std::cout << "  " << labels[i] << ": " 
                              << std::fixed << std::setprecision(3) 
                              << percentiles[i] * 1000 << " ms\n";
                }
            }
        }
    }
    
    std::string format_size(size_t bytes) const {
        if (bytes >= GB) {
            return std::to_string(bytes / GB) + "GB";
        } else if (bytes >= MB) {
            return std::to_string(bytes / MB) + "MB";
        } else if (bytes >= KB) {
            return std::to_string(bytes / KB) + "KB";
        } else {
            return std::to_string(bytes) + "B";
        }
    }
};

void print_usage(const std::string& prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n";
    std::cout << "Options:\n";
    std::cout << "  -f <filename>  Test file path (default: ./diskio_test)\n";
    std::cout << "  -s <size>      File size in MB (default: 100)\n";
    std::cout << "  -t <type>      Test type: pattern, block, sync, modes, all (default: all)\n";
    std::cout << "  -T <threads>   Number of threads (default: 4)\n";
    std::cout << "  -d <duration>  Test duration in seconds (default: 10)\n";
    std::cout << "  -v             Verbose output with detailed statistics\n";
    std::cout << "  -h             Show this help message\n";
    std::cout << "\nAnalyzes disk I/O performance characteristics:\n";
    std::cout << "  - Sequential vs random access patterns\n";
    std::cout << "  - Block size optimization analysis\n";
    std::cout << "  - fsync/fdatasync overhead measurement\n";
    std::cout << "  - Buffered vs direct vs synchronous I/O\n";
    std::cout << "  - Multi-threaded I/O scaling\n";
    std::cout << "  - Template-based pattern analysis\n";
}

} // namespace DiskIOAnalysis

int main(int argc, char* argv[]) {
    using namespace DiskIOAnalysis;
    
    std::string filename = "./diskio_test";
    size_t file_size_mb = 100;
    TestType test_type = TestType::All;
    int num_threads = 4;
    int duration = DEFAULT_TEST_DURATION;
    bool verbose = false;
    
    int opt;
    while ((opt = getopt(argc, argv, "f:s:t:T:d:vh")) != -1) {
        switch (opt) {
            case 'f':
                filename = optarg;
                break;
            case 's':
                file_size_mb = std::stoi(optarg);
                if (file_size_mb <= 0) {
                    std::cerr << "Invalid file size: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 't':
                if (std::string_view(optarg) == "pattern") test_type = TestType::PatternAnalysis;
                else if (std::string_view(optarg) == "block") test_type = TestType::BlockSizeAnalysis;
                else if (std::string_view(optarg) == "sync") test_type = TestType::SyncOverhead;
                else if (std::string_view(optarg) == "modes") test_type = TestType::IOModeComparison;
                else if (std::string_view(optarg) == "all") test_type = TestType::All;
                else {
                    std::cerr << "Unknown test type: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 'T':
                num_threads = std::stoi(optarg);
                if (num_threads <= 0 || num_threads > static_cast<int>(MAX_THREADS)) {
                    std::cerr << "Invalid number of threads: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 'd':
                duration = std::stoi(optarg);
                if (duration <= 0) {
                    std::cerr << "Invalid duration: " << optarg << std::endl;
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
    
    size_t file_size = file_size_mb * MB;
    
    std::cout << "Disk I/O Performance Analyzer (C++)\n";
    std::cout << "====================================\n";
    std::cout << "Test file: " << filename << "\n";
    std::cout << "File size: " << file_size_mb << " MB\n";
    std::cout << "Threads: " << num_threads << "\n";
    std::cout << "Duration: " << duration << " seconds\n";
    std::cout << "Verbose: " << (verbose ? "enabled" : "disabled") << "\n";
    std::cout << "Test type: ";
    
    switch (test_type) {
        case TestType::PatternAnalysis: std::cout << "Pattern Analysis\n"; break;
        case TestType::BlockSizeAnalysis: std::cout << "Block Size Analysis\n"; break;
        case TestType::SyncOverhead: std::cout << "Sync Overhead Analysis\n"; break;
        case TestType::IOModeComparison: std::cout << "I/O Mode Comparison\n"; break;
        case TestType::All: std::cout << "All Tests\n"; break;
        default: std::cout << "Unknown\n"; break;
    }
    
    try {
        DiskIOAnalyzer analyzer(filename, file_size, num_threads, duration, verbose);
        
        switch (test_type) {
            case TestType::PatternAnalysis:
                analyzer.run_pattern_analysis();
                break;
            case TestType::BlockSizeAnalysis:
                analyzer.run_block_size_analysis();
                break;
            case TestType::SyncOverhead:
                analyzer.run_sync_overhead_analysis();
                break;
            case TestType::IOModeComparison:
                analyzer.run_io_mode_comparison();
                break;
            case TestType::All:
                analyzer.run_all_tests();
                break;
            case TestType::QueueDepthAnalysis:
                std::cout << "Queue depth analysis not yet implemented.\n";
                break;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}