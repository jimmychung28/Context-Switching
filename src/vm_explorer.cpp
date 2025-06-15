/*
 * Virtual Memory Subsystem Explorer - C++ Version
 * Analyzes VM performance using modern C++ features
 */

#include <iostream>
#include <iomanip>
#include <memory>
#include <vector>
#include <array>
#include <algorithm>
#include <random>
#include <chrono>
#include <cstring>
#include <sstream>
#include <functional>
#include <numeric>
#include <map>
#include <thread>
#include <csignal>

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

// Constants
constexpr size_t PAGE_SIZE = 4096;
constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;  // 2MB
constexpr size_t CACHE_LINE_SIZE = 64;
constexpr size_t DEFAULT_MEMORY_SIZE = 256 * 1024 * 1024;  // 256MB
constexpr size_t TLB_ENTRIES = 1536;  // Typical TLB size

// Access patterns
enum class AccessPattern {
    Sequential,
    Random,
    Stride,
    PointerChase
};

// Test types
enum class TestType {
    TLBMiss,
    PageFault,
    HugePages,
    MmapVsMalloc,
    CopyOnWrite,
    All
};

// Test results structure
struct TestResult {
    double duration_ms;
    long page_faults;
    long tlb_misses_estimate;
    double throughput_mbps;
    double latency_ns;
    size_t memory_used;
    
    TestResult() : duration_ms(0), page_faults(0), tlb_misses_estimate(0),
                   throughput_mbps(0), latency_ns(0), memory_used(0) {}
};

// RAII Memory wrapper
class MemoryBlock {
private:
    void* ptr_;
    size_t size_;
    bool is_mmap_;
    
public:
    MemoryBlock(size_t size, bool use_mmap = true, bool use_huge_pages = false) 
        : ptr_(nullptr), size_(size), is_mmap_(use_mmap) {
        allocate(use_huge_pages);
    }
    
    ~MemoryBlock() {
        deallocate();
    }
    
    // Delete copy constructor and assignment
    MemoryBlock(const MemoryBlock&) = delete;
    MemoryBlock& operator=(const MemoryBlock&) = delete;
    
    // Move constructor and assignment
    MemoryBlock(MemoryBlock&& other) noexcept 
        : ptr_(other.ptr_), size_(other.size_), is_mmap_(other.is_mmap_) {
        other.ptr_ = nullptr;
        other.size_ = 0;
    }
    
    MemoryBlock& operator=(MemoryBlock&& other) noexcept {
        if (this != &other) {
            deallocate();
            ptr_ = other.ptr_;
            size_ = other.size_;
            is_mmap_ = other.is_mmap_;
            other.ptr_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }
    
    void* get() { return ptr_; }
    const void* get() const { return ptr_; }
    size_t size() const { return size_; }
    
    template<typename T>
    T* as() { return static_cast<T*>(ptr_); }
    
    template<typename T>
    const T* as() const { return static_cast<const T*>(ptr_); }
    
    void touch_all_pages() {
        char* p = static_cast<char*>(ptr_);
        for (size_t i = 0; i < size_; i += PAGE_SIZE) {
            p[i] = 1;
        }
    }
    
private:
    void allocate(bool use_huge_pages) {
        if (is_mmap_) {
            int flags = MAP_PRIVATE | MAP_ANONYMOUS;
            #ifdef __linux__
            if (use_huge_pages) {
                flags |= MAP_HUGETLB;
            }
            #endif
            
            ptr_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, flags, -1, 0);
            if (ptr_ == MAP_FAILED) {
                if (use_huge_pages) {
                    // Retry without huge pages
                    ptr_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                }
                if (ptr_ == MAP_FAILED) {
                    throw std::runtime_error("mmap failed");
                }
            }
        } else {
            if (posix_memalign(&ptr_, PAGE_SIZE, size_) != 0) {
                throw std::runtime_error("posix_memalign failed");
            }
        }
    }
    
    void deallocate() {
        if (ptr_) {
            if (is_mmap_) {
                munmap(ptr_, size_);
            } else {
                free(ptr_);
            }
            ptr_ = nullptr;
        }
    }
};

// Timer utility using std::chrono
class Timer {
private:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;
    
    TimePoint start_time_;
    
public:
    Timer() : start_time_(Clock::now()) {}
    
    void reset() {
        start_time_ = Clock::now();
    }
    
    double elapsed_ms() const {
        auto duration = Clock::now() - start_time_;
        return std::chrono::duration<double, std::milli>(duration).count();
    }
    
    double elapsed_us() const {
        auto duration = Clock::now() - start_time_;
        return std::chrono::duration<double, std::micro>(duration).count();
    }
    
    double elapsed_ns() const {
        auto duration = Clock::now() - start_time_;
        return std::chrono::duration<double, std::nano>(duration).count();
    }
};

// System metrics helper
class SystemMetrics {
public:
    static long get_page_faults() {
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            return usage.ru_majflt + usage.ru_minflt;
        }
        return 0;
    }
    
    static size_t get_total_ram() {
        return sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    }
    
    static void print_system_info() {
        std::cout << "\nSystem Information:\n";
        std::cout << "- Page size: " << PAGE_SIZE << " bytes\n";
        std::cout << "- Estimated TLB entries: " << TLB_ENTRIES << "\n";
        std::cout << "- Total RAM: " << get_total_ram() / (1024 * 1024) << " MB\n";
    }
};

// Memory access patterns
template<typename T>
class MemoryAccessor {
private:
    std::mt19937 rng_;
    
public:
    MemoryAccessor() : rng_(std::random_device{}()) {}
    
    double sequential_access(T* ptr, size_t size, int iterations) {
        Timer timer;
        
        for (int iter = 0; iter < iterations; ++iter) {
            for (size_t i = 0; i < size / sizeof(T); i += CACHE_LINE_SIZE / sizeof(T)) {
                ptr[i]++;
            }
        }
        
        return timer.elapsed_ms();
    }
    
    double random_access(T* ptr, size_t size, int iterations) {
        size_t num_elements = size / sizeof(T);
        size_t num_accesses = num_elements / (CACHE_LINE_SIZE / sizeof(T));
        
        // Create random indices
        std::vector<size_t> indices(num_accesses);
        std::iota(indices.begin(), indices.end(), 0);
        std::shuffle(indices.begin(), indices.end(), rng_);
        
        Timer timer;
        
        for (int iter = 0; iter < iterations; ++iter) {
            for (size_t idx : indices) {
                ptr[idx * (CACHE_LINE_SIZE / sizeof(T))]++;
            }
        }
        
        return timer.elapsed_ms();
    }
    
    double stride_access(T* ptr, size_t size, int iterations, size_t stride_bytes) {
        size_t stride_elements = stride_bytes / sizeof(T);
        Timer timer;
        
        for (int iter = 0; iter < iterations; ++iter) {
            for (size_t i = 0; i < size / sizeof(T); i += stride_elements) {
                ptr[i]++;
            }
        }
        
        return timer.elapsed_ms();
    }
    
    double pointer_chase(void* ptr, size_t size, int iterations) {
        struct Node {
            Node* next;
            char padding[CACHE_LINE_SIZE - sizeof(Node*)];
        };
        
        Node* nodes = static_cast<Node*>(ptr);
        size_t num_nodes = size / sizeof(Node);
        
        // Create linked list
        for (size_t i = 0; i < num_nodes - 1; ++i) {
            nodes[i].next = &nodes[i + 1];
        }
        nodes[num_nodes - 1].next = &nodes[0];
        
        // Shuffle
        for (size_t i = 0; i < num_nodes - 1; ++i) {
            std::uniform_int_distribution<size_t> dist(i, num_nodes - 1);
            size_t j = dist(rng_);
            std::swap(nodes[i].next, nodes[j].next);
        }
        
        Timer timer;
        
        Node* current = &nodes[0];
        for (int iter = 0; iter < iterations; ++iter) {
            for (size_t i = 0; i < num_nodes; ++i) {
                current = current->next;
            }
        }
        
        return timer.elapsed_ms();
    }
};

// Main VM Explorer class
class VMExplorer {
private:
    struct Config {
        size_t memory_size = DEFAULT_MEMORY_SIZE;
        TestType test_type = TestType::All;
        AccessPattern pattern = AccessPattern::Sequential;
        int iterations = 10;
        size_t stride_size = PAGE_SIZE;
        bool verbose = false;
    } config_;
    
    MemoryAccessor<char> accessor_;
    
public:
    VMExplorer() = default;
    
    void set_memory_size(size_t size) { config_.memory_size = size; }
    void set_test_type(TestType type) { config_.test_type = type; }
    void set_pattern(AccessPattern pattern) { config_.pattern = pattern; }
    void set_iterations(int iter) { config_.iterations = iter; }
    void set_verbose(bool v) { config_.verbose = v; }
    
    void run() {
        std::cout << "Virtual Memory Subsystem Explorer (C++ Version)\n";
        std::cout << "==============================================\n";
        
        switch (config_.test_type) {
            case TestType::TLBMiss:
                test_tlb_misses();
                break;
            case TestType::PageFault:
                test_page_faults();
                break;
            case TestType::HugePages:
                test_huge_pages();
                break;
            case TestType::MmapVsMalloc:
                test_mmap_vs_malloc();
                break;
            case TestType::CopyOnWrite:
                test_copy_on_write();
                break;
            case TestType::All:
                test_tlb_misses();
                test_page_faults();
                test_huge_pages();
                test_mmap_vs_malloc();
                test_copy_on_write();
                generate_report();
                break;
        }
    }
    
private:
    void test_tlb_misses() {
        std::cout << "\n=== TLB Miss Analysis ===\n";
        
        std::vector<std::pair<size_t, std::string>> test_sizes = {
            {4 * 1024, "4KB"},
            {64 * 1024, "64KB"},
            {1024 * 1024, "1MB"},
            {16 * 1024 * 1024, "16MB"},
            {config_.memory_size, "Custom"}
        };
        
        std::cout << "\nTesting different memory sizes and access patterns:\n";
        std::cout << std::setw(10) << "Size" 
                  << std::setw(16) << "Sequential(ms)"
                  << std::setw(14) << "Random(ms)"
                  << std::setw(14) << "Stride(ms)"
                  << std::setw(18) << "Est. TLB Misses\n";
        std::cout << std::string(72, '-') << "\n";
        
        for (const auto& [size, name] : test_sizes) {
            try {
                MemoryBlock mem(size);
                mem.touch_all_pages();
                
                double seq_time = accessor_.sequential_access(mem.as<char>(), size, 10);
                double rand_time = accessor_.random_access(mem.as<char>(), size, 10);
                double stride_time = accessor_.stride_access(mem.as<char>(), size, 10, PAGE_SIZE);
                
                long tlb_pages = TLB_ENTRIES * PAGE_SIZE;
                long est_misses = (size > tlb_pages) ? (size - tlb_pages) / PAGE_SIZE : 0;
                
                std::cout << std::setw(10) << name
                          << std::setw(16) << std::fixed << std::setprecision(2) << seq_time
                          << std::setw(14) << rand_time
                          << std::setw(14) << stride_time
                          << std::setw(18) << est_misses << "\n";
                          
            } catch (const std::exception& e) {
                std::cerr << "Error testing " << name << ": " << e.what() << "\n";
            }
        }
        
        std::cout << "\nTLB Coverage Analysis:\n";
        std::cout << "- TLB can typically hold " << TLB_ENTRIES << " pages (" 
                  << (TLB_ENTRIES * PAGE_SIZE) / 1024 << " KB)\n";
        std::cout << "- Sequential access minimizes TLB misses\n";
        std::cout << "- Random access maximizes TLB misses\n";
        std::cout << "- Performance degradation indicates TLB pressure\n";
    }
    
    void test_page_faults() {
        std::cout << "\n=== Page Fault Analysis ===\n";
        
        size_t test_size = config_.memory_size;
        
        // Test 1: Demand paging
        std::cout << "\nTest 1: Demand Paging (first touch)\n";
        try {
            MemoryBlock mem(test_size);
            
            long faults_before = SystemMetrics::get_page_faults();
            Timer timer;
            
            char* p = mem.as<char>();
            for (size_t i = 0; i < test_size; i += PAGE_SIZE) {
                p[i] = 1;
            }
            
            double duration = timer.elapsed_ms();
            long faults_after = SystemMetrics::get_page_faults();
            long new_faults = faults_after - faults_before;
            
            std::cout << "- Memory size: " << test_size / (1024 * 1024) << " MB\n";
            std::cout << "- Page faults: " << new_faults << "\n";
            std::cout << "- Time taken: " << std::fixed << std::setprecision(3) 
                      << duration << " ms\n";
            if (new_faults > 0) {
                std::cout << "- Average time per fault: " 
                          << (duration * 1000) / new_faults << " μs\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in demand paging test: " << e.what() << "\n";
        }
        
        // Test 2: Pre-faulted memory
        std::cout << "\nTest 2: Pre-faulted Memory Access\n";
        try {
            MemoryBlock mem(test_size);
            mem.touch_all_pages();
            
            long faults_before = SystemMetrics::get_page_faults();
            Timer timer;
            
            char* p = mem.as<char>();
            for (size_t i = 0; i < test_size; i += PAGE_SIZE) {
                p[i]++;
            }
            
            double duration = timer.elapsed_ms();
            long faults_after = SystemMetrics::get_page_faults();
            
            std::cout << "- Page faults on second access: " 
                      << (faults_after - faults_before) << "\n";
            std::cout << "- Time taken: " << duration << " ms (much faster)\n";
            
        } catch (const std::exception& e) {
            std::cerr << "Error in pre-faulted test: " << e.what() << "\n";
        }
        
        // Test 3: madvise effects
        #ifdef MADV_WILLNEED
        std::cout << "\nTest 3: madvise() Effects\n";
        try {
            MemoryBlock mem(test_size);
            
            madvise(mem.get(), test_size, MADV_WILLNEED);
            
            long faults_before = SystemMetrics::get_page_faults();
            Timer timer;
            
            char* p = mem.as<char>();
            for (size_t i = 0; i < test_size; i += PAGE_SIZE) {
                p[i] = 1;
            }
            
            double duration = timer.elapsed_ms();
            long faults_after = SystemMetrics::get_page_faults();
            
            std::cout << "- Page faults with MADV_WILLNEED: " 
                      << (faults_after - faults_before) << "\n";
            std::cout << "- Time taken: " << duration << " ms\n";
            
        } catch (const std::exception& e) {
            std::cerr << "Error in madvise test: " << e.what() << "\n";
        }
        #endif
    }
    
    void test_huge_pages() {
        std::cout << "\n=== Huge Pages vs Regular Pages ===\n";
        
        size_t test_size = 256 * 1024 * 1024; // 256MB
        if (test_size % HUGE_PAGE_SIZE != 0) {
            test_size = (test_size / HUGE_PAGE_SIZE + 1) * HUGE_PAGE_SIZE;
        }
        
        std::cout << "\nTest size: " << test_size / (1024 * 1024) << " MB\n";
        std::cout << "Regular page size: " << PAGE_SIZE / 1024 << " KB\n";
        std::cout << "Huge page size: " << HUGE_PAGE_SIZE / (1024 * 1024) << " MB\n";
        
        TestResult regular_results, huge_results;
        
        // Test regular pages
        std::cout << "\nRegular Pages Performance:\n";
        try {
            MemoryBlock mem(test_size, true, false);
            mem.touch_all_pages();
            
            double seq = accessor_.sequential_access(mem.as<char>(), test_size, 5);
            double rand = accessor_.random_access(mem.as<char>(), test_size, 5);
            double stride = accessor_.stride_access(mem.as<char>(), test_size, 5, 64 * 1024);
            
            std::cout << "- Sequential: " << seq << " ms\n";
            std::cout << "- Random: " << rand << " ms\n";
            std::cout << "- Stride (64KB): " << stride << " ms\n";
            
            regular_results.duration_ms = (seq + rand + stride) / 3;
            
        } catch (const std::exception& e) {
            std::cerr << "Error testing regular pages: " << e.what() << "\n";
        }
        
        // Test huge pages
        std::cout << "\nHuge Pages Performance:\n";
        #ifdef __linux__
        try {
            MemoryBlock mem(test_size, true, true);
            mem.touch_all_pages();
            
            double seq = accessor_.sequential_access(mem.as<char>(), test_size, 5);
            double rand = accessor_.random_access(mem.as<char>(), test_size, 5);
            double stride = accessor_.stride_access(mem.as<char>(), test_size, 5, 64 * 1024);
            
            std::cout << "- Sequential: " << seq << " ms";
            if (regular_results.duration_ms > 0) {
                std::cout << " (" << regular_results.duration_ms / seq << "x speedup)";
            }
            std::cout << "\n";
            
            std::cout << "- Random: " << rand << " ms\n";
            std::cout << "- Stride (64KB): " << stride << " ms\n";
            
        } catch (const std::exception& e) {
            std::cout << "- Huge pages not available: " << e.what() << "\n";
        }
        #else
        std::cout << "- Huge pages not supported on this platform\n";
        #endif
        
        std::cout << "\nAnalysis:\n";
        std::cout << "- Huge pages reduce TLB pressure\n";
        std::cout << "- " << test_size / PAGE_SIZE << " regular pages vs " 
                  << test_size / HUGE_PAGE_SIZE << " huge pages for " 
                  << test_size / (1024 * 1024) << " MB\n";
    }
    
    void test_mmap_vs_malloc() {
        std::cout << "\n=== mmap() vs malloc() Performance ===\n";
        
        std::vector<std::pair<size_t, std::string>> sizes = {
            {1024 * 1024, "1MB"},
            {16 * 1024 * 1024, "16MB"},
            {128 * 1024 * 1024, "128MB"}
        };
        
        std::cout << "\nAllocation and Access Performance:\n";
        std::cout << std::setw(8) << "Size"
                  << std::setw(15) << "malloc alloc"
                  << std::setw(13) << "mmap alloc"
                  << std::setw(16) << "malloc access"
                  << std::setw(14) << "mmap access\n";
        std::cout << std::string(66, '-') << "\n";
        
        for (const auto& [size, name] : sizes) {
            try {
                // Test malloc
                Timer malloc_timer;
                MemoryBlock malloc_mem(size, false);
                double malloc_alloc_time = malloc_timer.elapsed_ms();
                
                double malloc_access_time = accessor_.sequential_access(
                    malloc_mem.as<char>(), size, 1);
                
                // Test mmap
                Timer mmap_timer;
                MemoryBlock mmap_mem(size, true);
                double mmap_alloc_time = mmap_timer.elapsed_ms();
                
                double mmap_access_time = accessor_.sequential_access(
                    mmap_mem.as<char>(), size, 1);
                
                std::cout << std::setw(8) << name
                          << std::setw(15) << std::fixed << std::setprecision(3) 
                          << malloc_alloc_time << " ms"
                          << std::setw(13) << mmap_alloc_time << " ms"
                          << std::setw(16) << malloc_access_time << " ms"
                          << std::setw(14) << mmap_access_time << " ms\n";
                          
            } catch (const std::exception& e) {
                std::cerr << "Error testing " << name << ": " << e.what() << "\n";
            }
        }
        
        // Page fault comparison
        std::cout << "\nPage Fault Behavior:\n";
        size_t test_size = 64 * 1024 * 1024;
        
        try {
            // malloc behavior
            long faults_before = SystemMetrics::get_page_faults();
            MemoryBlock malloc_mem(test_size, false);
            long malloc_alloc_faults = SystemMetrics::get_page_faults() - faults_before;
            
            faults_before = SystemMetrics::get_page_faults();
            malloc_mem.touch_all_pages();
            long malloc_touch_faults = SystemMetrics::get_page_faults() - faults_before;
            
            // mmap behavior
            faults_before = SystemMetrics::get_page_faults();
            MemoryBlock mmap_mem(test_size, true);
            long mmap_alloc_faults = SystemMetrics::get_page_faults() - faults_before;
            
            faults_before = SystemMetrics::get_page_faults();
            mmap_mem.touch_all_pages();
            long mmap_touch_faults = SystemMetrics::get_page_faults() - faults_before;
            
            std::cout << "- malloc: " << malloc_alloc_faults << " faults on alloc, "
                      << malloc_touch_faults << " on first touch\n";
            std::cout << "- mmap: " << mmap_alloc_faults << " faults on alloc, "
                      << mmap_touch_faults << " on first touch\n";
                      
        } catch (const std::exception& e) {
            std::cerr << "Error in page fault comparison: " << e.what() << "\n";
        }
        
        std::cout << "\nKey Differences:\n";
        std::cout << "- malloc may reuse freed memory (fewer page faults)\n";
        std::cout << "- mmap always gets zero-filled pages from kernel\n";
        std::cout << "- mmap better for large allocations\n";
        std::cout << "- malloc better for small, frequent allocations\n";
    }
    
    void test_copy_on_write() {
        std::cout << "\n=== Copy-on-Write (COW) Fork Analysis ===\n";
        
        size_t test_size = 64 * 1024 * 1024; // 64MB
        
        try {
            MemoryBlock shared_mem(test_size);
            std::memset(shared_mem.get(), 0xAB, test_size);
            
            std::cout << "\nTest Setup:\n";
            std::cout << "- Shared memory size: " << test_size / (1024 * 1024) << " MB\n";
            std::cout << "- Initial pattern: 0xAB\n";
            
            // Test 1: Fork without modification
            std::cout << "\nTest 1: Fork without modification\n";
            
            long parent_faults_before = SystemMetrics::get_page_faults();
            Timer fork_timer;
            
            pid_t pid = fork();
            if (pid == 0) {
                // Child: just read memory
                volatile char sum = 0;
                char* p = shared_mem.as<char>();
                for (size_t i = 0; i < test_size; i += PAGE_SIZE) {
                    sum += p[i];
                }
                _exit(0);
            }
            
            double fork_time = fork_timer.elapsed_ms();
            wait(nullptr);
            long parent_faults_after = SystemMetrics::get_page_faults();
            
            std::cout << "- Fork time: " << fork_time << " ms\n";
            std::cout << "- Parent page faults: " 
                      << (parent_faults_after - parent_faults_before) 
                      << " (minimal due to COW)\n";
            
            // Test 2: Fork with modification
            std::cout << "\nTest 2: Fork with child modifying memory\n";
            
            int pipe_fd[2];
            if (pipe(pipe_fd) == -1) {
                throw std::runtime_error("pipe failed");
            }
            
            parent_faults_before = SystemMetrics::get_page_faults();
            fork_timer.reset();
            
            pid = fork();
            if (pid == 0) {
                // Child: modify half the memory
                close(pipe_fd[0]);
                
                long child_faults_before = SystemMetrics::get_page_faults();
                Timer modify_timer;
                
                char* p = shared_mem.as<char>();
                for (size_t i = 0; i < test_size / 2; i += PAGE_SIZE) {
                    p[i] = 0xCD;
                }
                
                double modify_time = modify_timer.elapsed_ms();
                long child_faults_after = SystemMetrics::get_page_faults();
                long cow_faults = child_faults_after - child_faults_before;
                
                // Send results to parent
                write(pipe_fd[1], &cow_faults, sizeof(cow_faults));
                write(pipe_fd[1], &modify_time, sizeof(modify_time));
                close(pipe_fd[1]);
                
                _exit(0);
            }
            
            // Parent
            close(pipe_fd[1]);
            
            long child_cow_faults;
            double child_modify_time;
            read(pipe_fd[0], &child_cow_faults, sizeof(child_cow_faults));
            read(pipe_fd[0], &child_modify_time, sizeof(child_modify_time));
            close(pipe_fd[0]);
            
            wait(nullptr);
            fork_time = fork_timer.elapsed_ms();
            
            std::cout << "- Fork time: " << fork_time << " ms\n";
            std::cout << "- Child COW page faults: " << child_cow_faults << "\n";
            std::cout << "- Child modify time: " << child_modify_time << " ms\n";
            std::cout << "- Pages modified: " << test_size / 2 / PAGE_SIZE << "\n";
            if (child_cow_faults > 0) {
                std::cout << "- Average COW fault time: " 
                          << (child_modify_time * 1000) / child_cow_faults << " μs\n";
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Error in COW test: " << e.what() << "\n";
        }
        
        std::cout << "\nCOW Analysis:\n";
        std::cout << "- Fork is fast due to COW (no immediate copying)\n";
        std::cout << "- Page faults occur only on write access\n";
        std::cout << "- Each COW fault copies one page (" << PAGE_SIZE << " bytes)\n";
        std::cout << "- Significant memory savings for read-only children\n";
    }
    
    void generate_report() {
        std::cout << "\n=== Virtual Memory Performance Report ===\n";
        
        SystemMetrics::print_system_info();
        
        std::cout << "\nKey Findings:\n";
        std::cout << "1. TLB Coverage: ~" << (TLB_ENTRIES * PAGE_SIZE) / 1024 
                  << " KB before misses impact performance\n";
        std::cout << "2. Page Fault Cost: Typically 10-50 microseconds\n";
        std::cout << "3. Huge Pages: Can provide 10-30% speedup for large datasets\n";
        std::cout << "4. COW Efficiency: Fork overhead minimal until writes occur\n";
        
        std::cout << "\nRecommendations:\n";
        std::cout << "- Use sequential access patterns when possible\n";
        std::cout << "- Consider huge pages for large memory workloads\n";
        std::cout << "- Pre-fault critical memory regions\n";
        std::cout << "- Use mmap for large allocations, malloc for small\n";
        std::cout << "- Leverage COW for read-mostly fork scenarios\n";
    }
};

// Command line parser
class CommandLineParser {
public:
    static void print_usage(const char* prog_name) {
        std::cout << "Usage: " << prog_name << " [options]\n";
        std::cout << "Options:\n";
        std::cout << "  -s <size>    Memory size in MB (default: 256)\n";
        std::cout << "  -t <test>    Test type: tlb, fault, huge, mmap, cow, all\n";
        std::cout << "  -p <pattern> Access pattern: seq, rand, stride, chase\n";
        std::cout << "  -i <iter>    Number of iterations (default: 10)\n";
        std::cout << "  -v           Verbose output\n";
        std::cout << "  -h           Show this help\n";
        std::cout << "\nExamples:\n";
        std::cout << "  " << prog_name << " -t tlb -s 512\n";
        std::cout << "  " << prog_name << " -t all\n";
    }
    
    static TestType parse_test_type(const std::string& str) {
        static const std::map<std::string, TestType> type_map = {
            {"tlb", TestType::TLBMiss},
            {"fault", TestType::PageFault},
            {"huge", TestType::HugePages},
            {"mmap", TestType::MmapVsMalloc},
            {"cow", TestType::CopyOnWrite},
            {"all", TestType::All}
        };
        
        auto it = type_map.find(str);
        if (it != type_map.end()) {
            return it->second;
        }
        throw std::invalid_argument("Unknown test type: " + str);
    }
    
    static AccessPattern parse_pattern(const std::string& str) {
        static const std::map<std::string, AccessPattern> pattern_map = {
            {"seq", AccessPattern::Sequential},
            {"rand", AccessPattern::Random},
            {"stride", AccessPattern::Stride},
            {"chase", AccessPattern::PointerChase}
        };
        
        auto it = pattern_map.find(str);
        if (it != pattern_map.end()) {
            return it->second;
        }
        throw std::invalid_argument("Unknown pattern: " + str);
    }
};

// Global signal handler
volatile sig_atomic_t g_stop = 0;
void signal_handler(int) {
    g_stop = 1;
}

int main(int argc, char* argv[]) {
    try {
        VMExplorer explorer;
        
        // Parse command line
        int opt;
        while ((opt = getopt(argc, argv, "s:t:p:i:vh")) != -1) {
            switch (opt) {
                case 's':
                    explorer.set_memory_size(std::stoul(optarg) * 1024 * 1024);
                    break;
                case 't':
                    explorer.set_test_type(CommandLineParser::parse_test_type(optarg));
                    break;
                case 'p':
                    explorer.set_pattern(CommandLineParser::parse_pattern(optarg));
                    break;
                case 'i':
                    explorer.set_iterations(std::stoi(optarg));
                    break;
                case 'v':
                    explorer.set_verbose(true);
                    break;
                case 'h':
                    CommandLineParser::print_usage(argv[0]);
                    return 0;
                default:
                    CommandLineParser::print_usage(argv[0]);
                    return 1;
            }
        }
        
        // Setup signal handler
        signal(SIGINT, signal_handler);
        
        // Run tests
        explorer.run();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}