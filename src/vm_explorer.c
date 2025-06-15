/*
 * Virtual Memory Subsystem Explorer
 * Analyzes VM performance: TLB misses, page faults, huge pages, mmap vs malloc, COW
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

// Constants
#define PAGE_SIZE 4096
#define HUGE_PAGE_SIZE (2 * 1024 * 1024)  // 2MB
#define CACHE_LINE_SIZE 64
#define MAX_SAMPLES 1000
#define DEFAULT_MEMORY_SIZE (256 * 1024 * 1024)  // 256MB
#define TLB_ENTRIES 1536  // Typical TLB size

// Test types
typedef enum {
    TEST_TLB_MISS,
    TEST_PAGE_FAULT,
    TEST_HUGE_PAGES,
    TEST_MMAP_MALLOC,
    TEST_COW_FORK,
    TEST_ALL
} test_type_t;

// Access patterns
typedef enum {
    PATTERN_SEQUENTIAL,
    PATTERN_RANDOM,
    PATTERN_STRIDE,
    PATTERN_POINTER_CHASE
} access_pattern_t;

// Test results
typedef struct {
    double duration;
    long page_faults;
    long tlb_misses_estimate;
    double throughput_mbps;
    double latency_ns;
    long memory_used;
} test_result_t;

// Configuration
typedef struct {
    size_t memory_size;
    test_type_t test_type;
    access_pattern_t pattern;
    int iterations;
    int stride_size;
    bool verbose;
    bool use_huge_pages;
} config_t;

// Global configuration
static config_t config;
static volatile int g_stop = 0;

// Signal handler
void signal_handler(int sig) {
    g_stop = 1;
}

// Get current time in seconds
double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Get page faults for current process
long get_page_faults() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_majflt + usage.ru_minflt;
    }
    return 0;
}

// Allocate memory with specified method
void* allocate_memory(size_t size, bool use_mmap, bool use_huge_pages) {
    void* ptr = NULL;
    
    if (use_mmap) {
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (use_huge_pages) {
            #ifdef __linux__
                #ifdef MAP_HUGETLB
                flags |= MAP_HUGETLB;
                #endif
            #else
                // Huge pages not supported on macOS
                printf("Note: Huge pages not available on macOS\n");
                use_huge_pages = false;
            #endif
        }
        
        ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (ptr == MAP_FAILED) {
            if (use_huge_pages && errno == ENOMEM) {
                // Fallback to regular pages
                printf("Note: Huge pages not available, using regular pages\n");
                ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, 
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            }
            if (ptr == MAP_FAILED) {
                perror("mmap");
                return NULL;
            }
        }
    } else {
        // Use malloc
        if (posix_memalign(&ptr, PAGE_SIZE, size) != 0) {
            perror("posix_memalign");
            return NULL;
        }
    }
    
    return ptr;
}

// Free allocated memory
void free_memory(void* ptr, size_t size, bool use_mmap) {
    if (use_mmap) {
        munmap(ptr, size);
    } else {
        free(ptr);
    }
}

// Touch all pages to force allocation
void touch_pages(void* ptr, size_t size) {
    char* p = (char*)ptr;
    for (size_t i = 0; i < size; i += PAGE_SIZE) {
        p[i] = 1;
    }
}

// Sequential access pattern
double test_sequential_access(void* ptr, size_t size, int iterations) {
    volatile char* p = (volatile char*)ptr;
    double start = get_time();
    
    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < size; i += CACHE_LINE_SIZE) {
            p[i]++;
        }
    }
    
    return get_time() - start;
}

// Random access pattern
double test_random_access(void* ptr, size_t size, int iterations) {
    volatile char* p = (volatile char*)ptr;
    size_t num_accesses = size / CACHE_LINE_SIZE;
    
    // Create random access pattern
    size_t* indices = malloc(num_accesses * sizeof(size_t));
    for (size_t i = 0; i < num_accesses; i++) {
        indices[i] = (rand() % (size / CACHE_LINE_SIZE)) * CACHE_LINE_SIZE;
    }
    
    double start = get_time();
    
    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < num_accesses; i++) {
            p[indices[i]]++;
        }
    }
    
    double duration = get_time() - start;
    free(indices);
    return duration;
}

// Stride access pattern
double test_stride_access(void* ptr, size_t size, int iterations, int stride) {
    volatile char* p = (volatile char*)ptr;
    double start = get_time();
    
    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < size; i += stride) {
            p[i]++;
        }
    }
    
    return get_time() - start;
}

// Pointer chase pattern (linked list traversal)
double test_pointer_chase(void* ptr, size_t size, int iterations) {
    struct node {
        struct node* next;
        char padding[CACHE_LINE_SIZE - sizeof(struct node*)];
    };
    
    struct node* nodes = (struct node*)ptr;
    size_t num_nodes = size / sizeof(struct node);
    
    // Create random linked list
    for (size_t i = 0; i < num_nodes - 1; i++) {
        nodes[i].next = &nodes[i + 1];
    }
    nodes[num_nodes - 1].next = &nodes[0];
    
    // Shuffle the list
    for (size_t i = 0; i < num_nodes - 1; i++) {
        size_t j = i + rand() % (num_nodes - i);
        struct node* temp = nodes[i].next;
        nodes[i].next = nodes[j].next;
        nodes[j].next = temp;
    }
    
    double start = get_time();
    
    struct node* current = &nodes[0];
    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < num_nodes; i++) {
            current = current->next;
        }
    }
    
    return get_time() - start;
}

// Test TLB misses with different access patterns
void test_tlb_misses() {
    printf("\n=== TLB Miss Analysis ===\n");
    
    size_t sizes[] = {
        4 * 1024,              // 4KB - fits in TLB
        64 * 1024,             // 64KB - partial TLB coverage
        1024 * 1024,           // 1MB - exceeds TLB
        16 * 1024 * 1024,      // 16MB - way beyond TLB
        config.memory_size     // User specified size
    };
    
    const char* size_names[] = {"4KB", "64KB", "1MB", "16MB", "Custom"};
    
    printf("\nTesting different memory sizes and access patterns:\n");
    printf("Size     | Sequential(ms) | Random(ms) | Stride(ms) | Est. TLB Misses\n");
    printf("---------|----------------|------------|------------|----------------\n");
    
    for (int i = 0; i < 5; i++) {
        void* ptr = allocate_memory(sizes[i], true, false);
        if (!ptr) continue;
        
        touch_pages(ptr, sizes[i]);
        
        // Test different patterns
        double seq_time = test_sequential_access(ptr, sizes[i], 10) * 1000;
        double rand_time = test_random_access(ptr, sizes[i], 10) * 1000;
        double stride_time = test_stride_access(ptr, sizes[i], 10, PAGE_SIZE) * 1000;
        
        // Estimate TLB misses based on performance degradation
        long tlb_pages = TLB_ENTRIES * PAGE_SIZE;
        long est_misses = (sizes[i] > tlb_pages) ? 
            (sizes[i] - tlb_pages) / PAGE_SIZE : 0;
        
        printf("%-8s | %14.2f | %10.2f | %10.2f | %15ld\n",
               size_names[i], seq_time, rand_time, stride_time, est_misses);
        
        free_memory(ptr, sizes[i], true);
    }
    
    printf("\nTLB Coverage Analysis:\n");
    printf("- TLB can typically hold %d pages (%ld KB)\n", 
           TLB_ENTRIES, (TLB_ENTRIES * PAGE_SIZE) / 1024);
    printf("- Sequential access minimizes TLB misses\n");
    printf("- Random access maximizes TLB misses\n");
    printf("- Performance degradation indicates TLB pressure\n");
}

// Test page fault behavior
void test_page_faults() {
    printf("\n=== Page Fault Analysis ===\n");
    
    size_t test_size = config.memory_size;
    
    // Test 1: Demand paging
    printf("\nTest 1: Demand Paging (first touch)\n");
    void* ptr = allocate_memory(test_size, true, false);
    if (!ptr) return;
    
    long faults_before = get_page_faults();
    double start = get_time();
    
    // Touch pages one by one
    char* p = (char*)ptr;
    for (size_t i = 0; i < test_size; i += PAGE_SIZE) {
        p[i] = 1;
    }
    
    double duration = get_time() - start;
    long faults_after = get_page_faults();
    long new_faults = faults_after - faults_before;
    
    printf("- Memory size: %zu MB\n", test_size / (1024 * 1024));
    printf("- Page faults: %ld\n", new_faults);
    printf("- Time taken: %.3f ms\n", duration * 1000);
    printf("- Average time per fault: %.3f μs\n", 
           (duration * 1e6) / new_faults);
    
    free_memory(ptr, test_size, true);
    
    // Test 2: Pre-faulted memory
    printf("\nTest 2: Pre-faulted Memory Access\n");
    ptr = allocate_memory(test_size, true, false);
    if (!ptr) return;
    
    // Pre-fault all pages
    touch_pages(ptr, test_size);
    
    faults_before = get_page_faults();
    start = get_time();
    
    // Access again - should be no page faults
    p = (char*)ptr;
    for (size_t i = 0; i < test_size; i += PAGE_SIZE) {
        p[i]++;
    }
    
    duration = get_time() - start;
    faults_after = get_page_faults();
    new_faults = faults_after - faults_before;
    
    printf("- Page faults on second access: %ld\n", new_faults);
    printf("- Time taken: %.3f ms (much faster)\n", duration * 1000);
    
    free_memory(ptr, test_size, true);
    
    // Test 3: madvise effects
    #ifdef MADV_WILLNEED
    printf("\nTest 3: madvise() Effects\n");
    ptr = allocate_memory(test_size, true, false);
    if (!ptr) return;
    
    // Advise kernel we'll need the memory
    madvise(ptr, test_size, MADV_WILLNEED);
    
    faults_before = get_page_faults();
    start = get_time();
    
    p = (char*)ptr;
    for (size_t i = 0; i < test_size; i += PAGE_SIZE) {
        p[i] = 1;
    }
    
    duration = get_time() - start;
    faults_after = get_page_faults();
    new_faults = faults_after - faults_before;
    
    printf("- Page faults with MADV_WILLNEED: %ld\n", new_faults);
    printf("- Time taken: %.3f ms\n", duration * 1000);
    
    free_memory(ptr, test_size, true);
    #endif
}

// Test huge pages vs regular pages
void test_huge_pages() {
    printf("\n=== Huge Pages vs Regular Pages ===\n");
    
    size_t test_size = 256 * 1024 * 1024; // 256MB
    if (test_size % HUGE_PAGE_SIZE != 0) {
        test_size = (test_size / HUGE_PAGE_SIZE + 1) * HUGE_PAGE_SIZE;
    }
    
    printf("\nTest size: %zu MB\n", test_size / (1024 * 1024));
    printf("Regular page size: %d KB\n", PAGE_SIZE / 1024);
    printf("Huge page size: %d MB\n", HUGE_PAGE_SIZE / (1024 * 1024));
    
    // Test regular pages
    printf("\nRegular Pages Performance:\n");
    void* regular_ptr = allocate_memory(test_size, true, false);
    if (!regular_ptr) return;
    
    touch_pages(regular_ptr, test_size);
    
    double reg_seq = test_sequential_access(regular_ptr, test_size, 5);
    double reg_rand = test_random_access(regular_ptr, test_size, 5);
    double reg_stride = test_stride_access(regular_ptr, test_size, 5, 64 * 1024);
    
    printf("- Sequential: %.3f ms\n", reg_seq * 1000);
    printf("- Random: %.3f ms\n", reg_rand * 1000);
    printf("- Stride (64KB): %.3f ms\n", reg_stride * 1000);
    
    free_memory(regular_ptr, test_size, true);
    
    // Test huge pages
    printf("\nHuge Pages Performance:\n");
    void* huge_ptr = allocate_memory(test_size, true, true);
    if (!huge_ptr) {
        printf("- Huge pages not available on this system\n");
        return;
    }
    
    touch_pages(huge_ptr, test_size);
    
    double huge_seq = test_sequential_access(huge_ptr, test_size, 5);
    double huge_rand = test_random_access(huge_ptr, test_size, 5);
    double huge_stride = test_stride_access(huge_ptr, test_size, 5, 64 * 1024);
    
    printf("- Sequential: %.3f ms (%.1fx speedup)\n", 
           huge_seq * 1000, reg_seq / huge_seq);
    printf("- Random: %.3f ms (%.1fx speedup)\n", 
           huge_rand * 1000, reg_rand / huge_rand);
    printf("- Stride (64KB): %.3f ms (%.1fx speedup)\n", 
           huge_stride * 1000, reg_stride / huge_stride);
    
    free_memory(huge_ptr, test_size, true);
    
    printf("\nAnalysis:\n");
    printf("- Huge pages reduce TLB pressure\n");
    printf("- %d regular pages vs %d huge pages for %zu MB\n",
           (int)(test_size / PAGE_SIZE), 
           (int)(test_size / HUGE_PAGE_SIZE),
           test_size / (1024 * 1024));
    printf("- Benefits increase with working set size\n");
}

// Test mmap vs malloc performance
void test_mmap_vs_malloc() {
    printf("\n=== mmap() vs malloc() Performance ===\n");
    
    size_t sizes[] = {
        1024 * 1024,        // 1MB
        16 * 1024 * 1024,   // 16MB
        128 * 1024 * 1024,  // 128MB
    };
    const char* size_names[] = {"1MB", "16MB", "128MB"};
    
    printf("\nAllocation and Access Performance:\n");
    printf("Size  | malloc alloc | mmap alloc | malloc access | mmap access\n");
    printf("------|--------------|------------|---------------|-------------\n");
    
    for (int i = 0; i < 3; i++) {
        size_t size = sizes[i];
        
        // Test malloc
        double malloc_alloc_start = get_time();
        void* malloc_ptr = allocate_memory(size, false, false);
        double malloc_alloc_time = get_time() - malloc_alloc_start;
        
        if (!malloc_ptr) continue;
        
        double malloc_access_time = test_sequential_access(malloc_ptr, size, 1);
        
        // Test mmap
        double mmap_alloc_start = get_time();
        void* mmap_ptr = allocate_memory(size, true, false);
        double mmap_alloc_time = get_time() - mmap_alloc_start;
        
        if (!mmap_ptr) {
            free_memory(malloc_ptr, size, false);
            continue;
        }
        
        double mmap_access_time = test_sequential_access(mmap_ptr, size, 1);
        
        printf("%-5s | %10.3f ms | %8.3f ms | %11.3f ms | %9.3f ms\n",
               size_names[i],
               malloc_alloc_time * 1000,
               mmap_alloc_time * 1000,
               malloc_access_time * 1000,
               mmap_access_time * 1000);
        
        free_memory(malloc_ptr, size, false);
        free_memory(mmap_ptr, size, true);
    }
    
    // Test page fault behavior
    printf("\nPage Fault Behavior:\n");
    size_t test_size = 64 * 1024 * 1024; // 64MB
    
    // malloc behavior
    long faults_before = get_page_faults();
    void* malloc_ptr = allocate_memory(test_size, false, false);
    long malloc_alloc_faults = get_page_faults() - faults_before;
    
    faults_before = get_page_faults();
    touch_pages(malloc_ptr, test_size);
    long malloc_touch_faults = get_page_faults() - faults_before;
    
    free_memory(malloc_ptr, test_size, false);
    
    // mmap behavior
    faults_before = get_page_faults();
    void* mmap_ptr = allocate_memory(test_size, true, false);
    long mmap_alloc_faults = get_page_faults() - faults_before;
    
    faults_before = get_page_faults();
    touch_pages(mmap_ptr, test_size);
    long mmap_touch_faults = get_page_faults() - faults_before;
    
    free_memory(mmap_ptr, test_size, true);
    
    printf("- malloc: %ld faults on alloc, %ld on first touch\n",
           malloc_alloc_faults, malloc_touch_faults);
    printf("- mmap: %ld faults on alloc, %ld on first touch\n",
           mmap_alloc_faults, mmap_touch_faults);
    printf("\nKey Differences:\n");
    printf("- malloc may reuse freed memory (fewer page faults)\n");
    printf("- mmap always gets zero-filled pages from kernel\n");
    printf("- mmap better for large allocations\n");
    printf("- malloc better for small, frequent allocations\n");
}

// Test copy-on-write fork behavior
void test_cow_fork() {
    printf("\n=== Copy-on-Write (COW) Fork Analysis ===\n");
    
    size_t test_size = 64 * 1024 * 1024; // 64MB
    void* shared_mem = allocate_memory(test_size, true, false);
    if (!shared_mem) return;
    
    // Initialize memory with pattern
    memset(shared_mem, 0xAB, test_size);
    
    printf("\nTest Setup:\n");
    printf("- Shared memory size: %zu MB\n", test_size / (1024 * 1024));
    printf("- Initial pattern: 0xAB\n");
    
    // Test 1: Fork without modification
    printf("\nTest 1: Fork without modification\n");
    
    long parent_faults_before = get_page_faults();
    double fork_start = get_time();
    
    pid_t pid = fork();
    if (pid == 0) {
        // Child: just read memory
        volatile char sum = 0;
        char* p = (char*)shared_mem;
        for (size_t i = 0; i < test_size; i += PAGE_SIZE) {
            sum += p[i];
        }
        exit(0);
    }
    
    double fork_time = get_time() - fork_start;
    wait(NULL);
    long parent_faults_after = get_page_faults();
    
    printf("- Fork time: %.3f ms\n", fork_time * 1000);
    printf("- Parent page faults: %ld (minimal due to COW)\n",
           parent_faults_after - parent_faults_before);
    
    // Test 2: Fork with modification
    printf("\nTest 2: Fork with child modifying memory\n");
    
    int pipe_fd[2];
    pipe(pipe_fd);
    
    parent_faults_before = get_page_faults();
    fork_start = get_time();
    
    pid = fork();
    if (pid == 0) {
        // Child: modify half the memory
        close(pipe_fd[0]);
        
        long child_faults_before = get_page_faults();
        double modify_start = get_time();
        
        char* p = (char*)shared_mem;
        for (size_t i = 0; i < test_size / 2; i += PAGE_SIZE) {
            p[i] = 0xCD;
        }
        
        double modify_time = get_time() - modify_start;
        long child_faults_after = get_page_faults();
        long cow_faults = child_faults_after - child_faults_before;
        
        // Send results to parent
        write(pipe_fd[1], &cow_faults, sizeof(cow_faults));
        write(pipe_fd[1], &modify_time, sizeof(modify_time));
        close(pipe_fd[1]);
        
        exit(0);
    }
    
    // Parent
    close(pipe_fd[1]);
    
    long child_cow_faults;
    double child_modify_time;
    read(pipe_fd[0], &child_cow_faults, sizeof(child_cow_faults));
    read(pipe_fd[0], &child_modify_time, sizeof(child_modify_time));
    close(pipe_fd[0]);
    
    wait(NULL);
    fork_time = get_time() - fork_start;
    
    printf("- Fork time: %.3f ms\n", fork_time * 1000);
    printf("- Child COW page faults: %ld\n", child_cow_faults);
    printf("- Child modify time: %.3f ms\n", child_modify_time * 1000);
    printf("- Pages modified: %zu\n", test_size / 2 / PAGE_SIZE);
    printf("- Average COW fault time: %.3f μs\n",
           (child_modify_time * 1e6) / child_cow_faults);
    
    // Test 3: Both parent and child modify
    printf("\nTest 3: Both parent and child modifying\n");
    
    pipe(pipe_fd);
    parent_faults_before = get_page_faults();
    
    pid = fork();
    if (pid == 0) {
        // Child: modify second half
        close(pipe_fd[0]);
        
        char* p = (char*)shared_mem;
        for (size_t i = test_size / 2; i < test_size; i += PAGE_SIZE) {
            p[i] = 0xEF;
        }
        
        write(pipe_fd[1], "done", 4);
        close(pipe_fd[1]);
        exit(0);
    }
    
    // Parent: modify first half
    close(pipe_fd[1]);
    
    char* p = (char*)shared_mem;
    for (size_t i = 0; i < test_size / 2; i += PAGE_SIZE) {
        p[i] = 0x12;
    }
    
    char buf[4];
    read(pipe_fd[0], buf, 4);
    close(pipe_fd[0]);
    
    wait(NULL);
    parent_faults_after = get_page_faults();
    
    printf("- Parent COW faults: %ld\n", 
           parent_faults_after - parent_faults_before);
    printf("- Total pages COW'd: ~%zu (entire allocation)\n",
           test_size / PAGE_SIZE);
    
    free_memory(shared_mem, test_size, true);
    
    printf("\nCOW Analysis:\n");
    printf("- Fork is fast due to COW (no immediate copying)\n");
    printf("- Page faults occur only on write access\n");
    printf("- Each COW fault copies one page (4KB)\n");
    printf("- Significant memory savings for read-only children\n");
}

// Generate comprehensive report
void generate_report() {
    printf("\n=== Virtual Memory Performance Report ===\n");
    printf("\nSystem Information:\n");
    printf("- Page size: %d bytes\n", PAGE_SIZE);
    printf("- Estimated TLB entries: %d\n", TLB_ENTRIES);
    
    long total_ram = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    printf("- Total RAM: %ld MB\n", total_ram / (1024 * 1024));
    
    printf("\nKey Findings:\n");
    printf("1. TLB Coverage: ~%ld KB before misses impact performance\n",
           (TLB_ENTRIES * PAGE_SIZE) / 1024);
    printf("2. Page Fault Cost: Typically 10-50 microseconds\n");
    printf("3. Huge Pages: Can provide 10-30%% speedup for large datasets\n");
    printf("4. COW Efficiency: Fork overhead minimal until writes occur\n");
    
    printf("\nRecommendations:\n");
    printf("- Use sequential access patterns when possible\n");
    printf("- Consider huge pages for large memory workloads\n");
    printf("- Pre-fault critical memory regions\n");
    printf("- Use mmap for large allocations, malloc for small\n");
    printf("- Leverage COW for read-mostly fork scenarios\n");
}

// Print usage
void print_usage(char* prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -s <size>    Memory size in MB (default: 256)\n");
    printf("  -t <test>    Test type: tlb, fault, huge, mmap, cow, all\n");
    printf("  -p <pattern> Access pattern: seq, rand, stride, chase\n");
    printf("  -i <iter>    Number of iterations (default: 10)\n");
    printf("  -v           Verbose output\n");
    printf("  -h           Show this help\n");
    printf("\nExamples:\n");
    printf("  %s -t tlb -s 512\n", prog_name);
    printf("  %s -t all\n", prog_name);
}

int main(int argc, char* argv[]) {
    // Initialize configuration
    config.memory_size = DEFAULT_MEMORY_SIZE;
    config.test_type = TEST_ALL;
    config.pattern = PATTERN_SEQUENTIAL;
    config.iterations = 10;
    config.stride_size = PAGE_SIZE;
    config.verbose = false;
    config.use_huge_pages = false;
    
    // Parse command line
    int opt;
    while ((opt = getopt(argc, argv, "s:t:p:i:vh")) != -1) {
        switch (opt) {
            case 's':
                config.memory_size = atol(optarg) * 1024 * 1024;
                break;
            case 't':
                if (strcmp(optarg, "tlb") == 0) config.test_type = TEST_TLB_MISS;
                else if (strcmp(optarg, "fault") == 0) config.test_type = TEST_PAGE_FAULT;
                else if (strcmp(optarg, "huge") == 0) config.test_type = TEST_HUGE_PAGES;
                else if (strcmp(optarg, "mmap") == 0) config.test_type = TEST_MMAP_MALLOC;
                else if (strcmp(optarg, "cow") == 0) config.test_type = TEST_COW_FORK;
                else if (strcmp(optarg, "all") == 0) config.test_type = TEST_ALL;
                else {
                    fprintf(stderr, "Unknown test type: %s\n", optarg);
                    return 1;
                }
                break;
            case 'p':
                if (strcmp(optarg, "seq") == 0) config.pattern = PATTERN_SEQUENTIAL;
                else if (strcmp(optarg, "rand") == 0) config.pattern = PATTERN_RANDOM;
                else if (strcmp(optarg, "stride") == 0) config.pattern = PATTERN_STRIDE;
                else if (strcmp(optarg, "chase") == 0) config.pattern = PATTERN_POINTER_CHASE;
                else {
                    fprintf(stderr, "Unknown pattern: %s\n", optarg);
                    return 1;
                }
                break;
            case 'i':
                config.iterations = atoi(optarg);
                break;
            case 'v':
                config.verbose = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    
    printf("Virtual Memory Subsystem Explorer\n");
    printf("=================================\n");
    
    // Seed random number generator
    srand(time(NULL));
    
    // Run tests
    switch (config.test_type) {
        case TEST_TLB_MISS:
            test_tlb_misses();
            break;
        case TEST_PAGE_FAULT:
            test_page_faults();
            break;
        case TEST_HUGE_PAGES:
            test_huge_pages();
            break;
        case TEST_MMAP_MALLOC:
            test_mmap_vs_malloc();
            break;
        case TEST_COW_FORK:
            test_cow_fork();
            break;
        case TEST_ALL:
            test_tlb_misses();
            test_page_faults();
            test_huge_pages();
            test_mmap_vs_malloc();
            test_cow_fork();
            generate_report();
            break;
    }
    
    printf("\n");
    return 0;
}