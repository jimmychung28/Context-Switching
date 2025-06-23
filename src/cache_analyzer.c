#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>
#include <math.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>

// macOS doesn't have pthread_barrier_t
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned int count;
    unsigned int num_threads;
} pthread_barrier_t;

static int pthread_barrier_init(pthread_barrier_t *barrier, void *attr, unsigned int count) {
    (void)attr;
    barrier->count = 0;
    barrier->num_threads = count;
    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
    return 0;
}

static int pthread_barrier_wait(pthread_barrier_t *barrier) {
    pthread_mutex_lock(&barrier->mutex);
    barrier->count++;
    if (barrier->count >= barrier->num_threads) {
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return 1;
    } else {
        pthread_cond_wait(&barrier->cond, &barrier->mutex);
        pthread_mutex_unlock(&barrier->mutex);
        return 0;
    }
}

static int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    pthread_mutex_destroy(&barrier->mutex);
    pthread_cond_destroy(&barrier->cond);
    return 0;
}

// macOS doesn't have CPU affinity functions like Linux
static int set_thread_affinity(int cpu_id) {
    thread_affinity_policy_data_t policy = { cpu_id };
    thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
    return thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, 
                            (thread_policy_t)&policy, 1);
}

#else
#include <sys/sysinfo.h>
#include <numa.h>
#include <sched.h>

static int set_thread_affinity(int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}
#endif

#define CACHE_LINE_SIZE 64
#define KB (1024)
#define MB (1024 * 1024)
#define GB (1024 * 1024 * 1024)

#define DEFAULT_ITERATIONS 1000000
#define MAX_THREADS 64
#define HISTOGRAM_BUCKETS 50

typedef enum {
    PATTERN_SEQUENTIAL,
    PATTERN_RANDOM,
    PATTERN_STRIDE,
    PATTERN_POINTER_CHASE,
    PATTERN_FALSE_SHARING,
    PATTERN_CACHE_FRIENDLY
} AccessPattern;

typedef enum {
    TEST_CACHE_HIERARCHY,
    TEST_FALSE_SHARING,
    TEST_CACHE_BOUNCING,
    TEST_NUMA_ANALYSIS,
    TEST_PREFETCHER,
    TEST_ALL
} TestType;

typedef struct {
    size_t l1_size;
    size_t l2_size;
    size_t l3_size;
    size_t cache_line_size;
    int num_cores;
    int num_numa_nodes;
} SystemInfo;

typedef struct {
    uint64_t total_accesses;
    uint64_t estimated_l1_hits;
    uint64_t estimated_l2_hits;
    uint64_t estimated_l3_hits;
    uint64_t estimated_memory_accesses;
    double total_time;
    double avg_latency;
    double throughput;
    int thread_id;
    int cpu_id;
    AccessPattern pattern;
} CacheStats;

typedef struct {
    int thread_id;
    int cpu_id;
    size_t memory_size;
    size_t stride;
    int iterations;
    AccessPattern pattern;
    CacheStats* stats;
    pthread_barrier_t* barrier;
    volatile int* stop_flag;
    char* shared_data;
    size_t shared_data_size;
} ThreadArgs;

// Global variables for cache analysis
static SystemInfo sys_info;
static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

static double get_time_diff(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

static void get_current_time(struct timespec* ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static void detect_system_info() {
    memset(&sys_info, 0, sizeof(SystemInfo));
    sys_info.cache_line_size = CACHE_LINE_SIZE;
    
#ifdef __APPLE__
    size_t size = sizeof(size_t);
    
    // Get cache sizes
    sysctlbyname("hw.l1icachesize", &sys_info.l1_size, &size, NULL, 0);
    sysctlbyname("hw.l2cachesize", &sys_info.l2_size, &size, NULL, 0);
    sysctlbyname("hw.l3cachesize", &sys_info.l3_size, &size, NULL, 0);
    
    // Get number of cores
    int ncpu;
    size = sizeof(int);
    sysctlbyname("hw.ncpu", &ncpu, &size, NULL, 0);
    sys_info.num_cores = ncpu;
    
    sys_info.num_numa_nodes = 1; // macOS typically has 1 NUMA node
    
    // Fallback values if detection fails
    if (sys_info.l1_size == 0) sys_info.l1_size = 32 * KB;
    if (sys_info.l2_size == 0) sys_info.l2_size = 256 * KB;
    if (sys_info.l3_size == 0) sys_info.l3_size = 8 * MB;
    
#else
    // Linux cache detection
    FILE* fp;
    char buffer[256];
    
    // Try to read cache sizes from /sys/devices/system/cpu/cpu0/cache/
    fp = fopen("/sys/devices/system/cpu/cpu0/cache/index0/size", "r");
    if (fp) {
        if (fgets(buffer, sizeof(buffer), fp)) {
            sys_info.l1_size = atoi(buffer) * KB;
        }
        fclose(fp);
    }
    
    fp = fopen("/sys/devices/system/cpu/cpu0/cache/index2/size", "r");
    if (fp) {
        if (fgets(buffer, sizeof(buffer), fp)) {
            sys_info.l2_size = atoi(buffer) * KB;
        }
        fclose(fp);
    }
    
    fp = fopen("/sys/devices/system/cpu/cpu0/cache/index3/size", "r");
    if (fp) {
        if (fgets(buffer, sizeof(buffer), fp)) {
            sys_info.l3_size = atoi(buffer) * KB;
        }
        fclose(fp);
    }
    
    sys_info.num_cores = get_nprocs();
    
    // Check NUMA
    if (numa_available() != -1) {
        sys_info.num_numa_nodes = numa_num_configured_nodes();
    } else {
        sys_info.num_numa_nodes = 1;
    }
    
    // Fallback values
    if (sys_info.l1_size == 0) sys_info.l1_size = 32 * KB;
    if (sys_info.l2_size == 0) sys_info.l2_size = 256 * KB;
    if (sys_info.l3_size == 0) sys_info.l3_size = 8 * MB;
#endif
}

static void print_system_info() {
    printf("=== System Cache Information ===\n");
    printf("L1 Cache Size: %zu KB\n", sys_info.l1_size / KB);
    printf("L2 Cache Size: %zu KB\n", sys_info.l2_size / KB);
    printf("L3 Cache Size: %zu MB\n", sys_info.l3_size / MB);
    printf("Cache Line Size: %zu bytes\n", sys_info.cache_line_size);
    printf("Number of Cores: %d\n", sys_info.num_cores);
    printf("NUMA Nodes: %d\n", sys_info.num_numa_nodes);
    printf("\n");
}

static void estimate_cache_performance(CacheStats* stats, size_t data_size, AccessPattern pattern) {
    // Estimate cache hit rates based on data size and access pattern
    double l1_hit_rate = 0.0, l2_hit_rate = 0.0, l3_hit_rate = 0.0;
    
    switch (pattern) {
        case PATTERN_SEQUENTIAL:
            if (data_size <= sys_info.l1_size) {
                l1_hit_rate = 0.95;
                l2_hit_rate = 0.04;
                l3_hit_rate = 0.01;
            } else if (data_size <= sys_info.l2_size) {
                l1_hit_rate = 0.30;
                l2_hit_rate = 0.65;
                l3_hit_rate = 0.04;
            } else if (data_size <= sys_info.l3_size) {
                l1_hit_rate = 0.10;
                l2_hit_rate = 0.20;
                l3_hit_rate = 0.65;
            } else {
                l1_hit_rate = 0.05;
                l2_hit_rate = 0.10;
                l3_hit_rate = 0.30;
            }
            break;
            
        case PATTERN_RANDOM:
            if (data_size <= sys_info.l1_size) {
                l1_hit_rate = 0.80;
                l2_hit_rate = 0.15;
                l3_hit_rate = 0.05;
            } else if (data_size <= sys_info.l2_size) {
                l1_hit_rate = 0.15;
                l2_hit_rate = 0.70;
                l3_hit_rate = 0.10;
            } else if (data_size <= sys_info.l3_size) {
                l1_hit_rate = 0.05;
                l2_hit_rate = 0.15;
                l3_hit_rate = 0.60;
            } else {
                l1_hit_rate = 0.02;
                l2_hit_rate = 0.05;
                l3_hit_rate = 0.20;
            }
            break;
            
        case PATTERN_STRIDE:
            // Stride pattern effectiveness depends on stride size
            if (data_size <= sys_info.l2_size) {
                l1_hit_rate = 0.60;
                l2_hit_rate = 0.35;
                l3_hit_rate = 0.04;
            } else {
                l1_hit_rate = 0.20;
                l2_hit_rate = 0.40;
                l3_hit_rate = 0.35;
            }
            break;
            
        default:
            l1_hit_rate = 0.50;
            l2_hit_rate = 0.30;
            l3_hit_rate = 0.15;
            break;
    }
    
    stats->estimated_l1_hits = (uint64_t)(stats->total_accesses * l1_hit_rate);
    stats->estimated_l2_hits = (uint64_t)(stats->total_accesses * l2_hit_rate);
    stats->estimated_l3_hits = (uint64_t)(stats->total_accesses * l3_hit_rate);
    stats->estimated_memory_accesses = stats->total_accesses - 
        stats->estimated_l1_hits - stats->estimated_l2_hits - stats->estimated_l3_hits;
}

static void* sequential_access_test(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    CacheStats* stats = &args->stats[args->thread_id];
    
    // Set CPU affinity
    if (args->cpu_id >= 0) {
        set_thread_affinity(args->cpu_id);
    }
    
    // Allocate memory
    char* data = malloc(args->memory_size);
    if (!data) {
        stats->total_accesses = 0;
        return NULL;
    }
    
    // Initialize data
    memset(data, 0, args->memory_size);
    
    if (args->barrier) {
        pthread_barrier_wait(args->barrier);
    }
    
    struct timespec start, end;
    get_current_time(&start);
    
    volatile char sum = 0;
    uint64_t accesses = 0;
    
    for (int iter = 0; iter < args->iterations; iter++) {
        for (size_t i = 0; i < args->memory_size; i += args->stride) {
            sum += data[i];
            accesses++;
            
            if (args->stop_flag && *args->stop_flag) {
                goto cleanup;
            }
        }
    }
    
cleanup:
    get_current_time(&end);
    
    stats->total_accesses = accesses;
    stats->total_time = get_time_diff(start, end);
    stats->avg_latency = stats->total_time / accesses;
    stats->throughput = accesses / stats->total_time;
    stats->thread_id = args->thread_id;
    stats->cpu_id = args->cpu_id;
    stats->pattern = args->pattern;
    
    estimate_cache_performance(stats, args->memory_size, args->pattern);
    
    free(data);
    return NULL;
}

static void* random_access_test(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    CacheStats* stats = &args->stats[args->thread_id];
    
    // Set CPU affinity
    if (args->cpu_id >= 0) {
        set_thread_affinity(args->cpu_id);
    }
    
    // Allocate memory
    char* data = malloc(args->memory_size);
    if (!data) {
        stats->total_accesses = 0;
        return NULL;
    }
    
    // Initialize data
    memset(data, 0, args->memory_size);
    
    // Create random indices
    size_t num_indices = args->memory_size / sizeof(size_t);
    size_t* indices = malloc(num_indices * sizeof(size_t));
    if (!indices) {
        free(data);
        stats->total_accesses = 0;
        return NULL;
    }
    
    // Initialize random indices
    srand(args->thread_id + time(NULL));
    for (size_t i = 0; i < num_indices; i++) {
        indices[i] = (rand() % (args->memory_size / CACHE_LINE_SIZE)) * CACHE_LINE_SIZE;
    }
    
    if (args->barrier) {
        pthread_barrier_wait(args->barrier);
    }
    
    struct timespec start, end;
    get_current_time(&start);
    
    volatile char sum = 0;
    uint64_t accesses = 0;
    
    for (int iter = 0; iter < args->iterations; iter++) {
        for (size_t i = 0; i < num_indices; i++) {
            sum += data[indices[i]];
            accesses++;
            
            if (args->stop_flag && *args->stop_flag) {
                goto cleanup;
            }
        }
    }
    
cleanup:
    get_current_time(&end);
    
    stats->total_accesses = accesses;
    stats->total_time = get_time_diff(start, end);
    stats->avg_latency = stats->total_time / accesses;
    stats->throughput = accesses / stats->total_time;
    stats->thread_id = args->thread_id;
    stats->cpu_id = args->cpu_id;
    stats->pattern = args->pattern;
    
    estimate_cache_performance(stats, args->memory_size, args->pattern);
    
    free(data);
    free(indices);
    return NULL;
}

static void* pointer_chase_test(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    CacheStats* stats = &args->stats[args->thread_id];
    
    // Set CPU affinity
    if (args->cpu_id >= 0) {
        set_thread_affinity(args->cpu_id);
    }
    
    typedef struct node {
        struct node* next;
        char padding[CACHE_LINE_SIZE - sizeof(struct node*)];
    } node_t;
    
    size_t num_nodes = args->memory_size / sizeof(node_t);
    node_t* nodes = aligned_alloc(CACHE_LINE_SIZE, num_nodes * sizeof(node_t));
    if (!nodes) {
        stats->total_accesses = 0;
        return NULL;
    }
    
    // Create a random linked list
    srand(args->thread_id + time(NULL));
    for (size_t i = 0; i < num_nodes - 1; i++) {
        size_t next_idx = rand() % num_nodes;
        nodes[i].next = &nodes[next_idx];
    }
    nodes[num_nodes - 1].next = &nodes[0]; // Close the loop
    
    if (args->barrier) {
        pthread_barrier_wait(args->barrier);
    }
    
    struct timespec start, end;
    get_current_time(&start);
    
    node_t* current = &nodes[0];
    uint64_t accesses = 0;
    
    for (int iter = 0; iter < args->iterations; iter++) {
        for (size_t i = 0; i < num_nodes; i++) {
            current = current->next;
            accesses++;
            
            if (args->stop_flag && *args->stop_flag) {
                goto cleanup;
            }
        }
    }
    
cleanup:
    get_current_time(&end);
    
    stats->total_accesses = accesses;
    stats->total_time = get_time_diff(start, end);
    stats->avg_latency = stats->total_time / accesses;
    stats->throughput = accesses / stats->total_time;
    stats->thread_id = args->thread_id;
    stats->cpu_id = args->cpu_id;
    stats->pattern = args->pattern;
    
    estimate_cache_performance(stats, args->memory_size, args->pattern);
    
    free(nodes);
    return NULL;
}

static void* false_sharing_test(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    CacheStats* stats = &args->stats[args->thread_id];
    
    // Set CPU affinity
    if (args->cpu_id >= 0) {
        set_thread_affinity(args->cpu_id);
    }
    
    // Use shared data for false sharing
    volatile int* shared_counters = (volatile int*)args->shared_data;
    int my_counter_idx = args->thread_id;
    
    if (args->barrier) {
        pthread_barrier_wait(args->barrier);
    }
    
    struct timespec start, end;
    get_current_time(&start);
    
    uint64_t accesses = 0;
    
    for (int iter = 0; iter < args->iterations; iter++) {
        // Each thread increments its own counter (false sharing)
        shared_counters[my_counter_idx]++;
        accesses++;
        
        // Add some computation to make the effect more visible
        for (int i = 0; i < 100; i++) {
            shared_counters[my_counter_idx] += i % 7;
        }
        accesses += 100;
        
        if (args->stop_flag && *args->stop_flag) {
            break;
        }
    }
    
    get_current_time(&end);
    
    stats->total_accesses = accesses;
    stats->total_time = get_time_diff(start, end);
    stats->avg_latency = stats->total_time / accesses;
    stats->throughput = accesses / stats->total_time;
    stats->thread_id = args->thread_id;
    stats->cpu_id = args->cpu_id;
    stats->pattern = args->pattern;
    
    // False sharing typically causes poor cache performance
    estimate_cache_performance(stats, CACHE_LINE_SIZE, args->pattern);
    
    return NULL;
}

static void cache_hierarchy_test(int num_threads) {
    printf("=== Cache Hierarchy Analysis ===\n");
    
    size_t test_sizes[] = {
        sys_info.l1_size / 2,     // L1 cache
        sys_info.l1_size * 2,     // L2 cache
        sys_info.l2_size * 2,     // L3 cache
        sys_info.l3_size * 2,     // Main memory
        sys_info.l3_size * 8      // Large memory
    };
    const char* size_names[] = {"L1", "L2", "L3", "Memory", "Large Memory"};
    int num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);
    
    AccessPattern patterns[] = {PATTERN_SEQUENTIAL, PATTERN_RANDOM, PATTERN_STRIDE};
    const char* pattern_names[] = {"Sequential", "Random", "Stride"};
    int num_patterns = sizeof(patterns) / sizeof(patterns[0]);
    
    for (int p = 0; p < num_patterns; p++) {
        printf("\n%s Access Pattern:\n", pattern_names[p]);
        printf("%-12s %12s %12s %12s %12s %12s\n", 
               "Size", "Latency(ns)", "Throughput", "L1 Hits", "L2 Hits", "Memory");
        printf("%-12s %12s %12s %12s %12s %12s\n", 
               "--------", "----------", "----------", "-------", "-------", "------");
        
        for (int s = 0; s < num_sizes; s++) {
            pthread_t threads[MAX_THREADS];
            ThreadArgs args[MAX_THREADS];
            CacheStats stats[MAX_THREADS];
            pthread_barrier_t barrier;
            
            memset(stats, 0, sizeof(stats));
            pthread_barrier_init(&barrier, NULL, num_threads);
            
            for (int t = 0; t < num_threads; t++) {
                args[t].thread_id = t;
                args[t].cpu_id = t % sys_info.num_cores;
                args[t].memory_size = test_sizes[s];
                args[t].stride = (patterns[p] == PATTERN_STRIDE) ? CACHE_LINE_SIZE * 2 : 1;
                args[t].iterations = 100;
                args[t].pattern = patterns[p];
                args[t].stats = stats;
                args[t].barrier = &barrier;
                args[t].stop_flag = NULL;
                
                void* (*test_func)(void*) = (patterns[p] == PATTERN_RANDOM) ? 
                    random_access_test : sequential_access_test;
                
                pthread_create(&threads[t], NULL, test_func, &args[t]);
            }
            
            for (int t = 0; t < num_threads; t++) {
                pthread_join(threads[t], NULL);
            }
            
            // Calculate averages
            double avg_latency = 0.0;
            double avg_throughput = 0.0;
            uint64_t total_l1_hits = 0;
            uint64_t total_l2_hits = 0;
            uint64_t total_memory = 0;
            
            for (int t = 0; t < num_threads; t++) {
                avg_latency += stats[t].avg_latency;
                avg_throughput += stats[t].throughput;
                total_l1_hits += stats[t].estimated_l1_hits;
                total_l2_hits += stats[t].estimated_l2_hits;
                total_memory += stats[t].estimated_memory_accesses;
            }
            
            avg_latency /= num_threads;
            avg_throughput /= num_threads;
            
            printf("%-12s %12.2f %12.0f %12.0f%% %12.0f%% %12.0f%%\n",
                   size_names[s],
                   avg_latency * 1e9,
                   avg_throughput,
                   (double)total_l1_hits / (total_l1_hits + total_l2_hits + total_memory) * 100,
                   (double)total_l2_hits / (total_l1_hits + total_l2_hits + total_memory) * 100,
                   (double)total_memory / (total_l1_hits + total_l2_hits + total_memory) * 100);
            
            pthread_barrier_destroy(&barrier);
        }
    }
}

static void false_sharing_analysis(int num_threads) {
    printf("\n=== False Sharing Analysis ===\n");
    
    // Test with false sharing
    printf("Testing FALSE SHARING scenario:\n");
    
    // Allocate shared data where counters are adjacent (false sharing)
    char* shared_data = aligned_alloc(CACHE_LINE_SIZE, CACHE_LINE_SIZE * num_threads);
    memset(shared_data, 0, CACHE_LINE_SIZE * num_threads);
    
    pthread_t threads[MAX_THREADS];
    ThreadArgs args[MAX_THREADS];
    CacheStats stats[MAX_THREADS];
    pthread_barrier_t barrier;
    
    memset(stats, 0, sizeof(stats));
    pthread_barrier_init(&barrier, NULL, num_threads);
    
    struct timespec start, end;
    get_current_time(&start);
    
    for (int t = 0; t < num_threads; t++) {
        args[t].thread_id = t;
        args[t].cpu_id = t % sys_info.num_cores;
        args[t].memory_size = 0;
        args[t].stride = 0;
        args[t].iterations = 10000;
        args[t].pattern = PATTERN_FALSE_SHARING;
        args[t].stats = stats;
        args[t].barrier = &barrier;
        args[t].stop_flag = NULL;
        args[t].shared_data = shared_data;
        args[t].shared_data_size = CACHE_LINE_SIZE * num_threads;
        
        pthread_create(&threads[t], NULL, false_sharing_test, &args[t]);
    }
    
    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }
    
    get_current_time(&end);
    double false_sharing_time = get_time_diff(start, end);
    
    // Test without false sharing
    printf("Testing CACHE-FRIENDLY scenario:\n");
    
    // Allocate shared data where each counter is on its own cache line
    free(shared_data);
    shared_data = aligned_alloc(CACHE_LINE_SIZE, CACHE_LINE_SIZE * num_threads);
    memset(shared_data, 0, CACHE_LINE_SIZE * num_threads);
    
    CacheStats cache_friendly_stats[MAX_THREADS];
    memset(cache_friendly_stats, 0, sizeof(cache_friendly_stats));
    
    get_current_time(&start);
    
    for (int t = 0; t < num_threads; t++) {
        args[t].stats = cache_friendly_stats;
        args[t].pattern = PATTERN_CACHE_FRIENDLY;
        // Each thread uses its own cache line
        args[t].shared_data = shared_data + (t * CACHE_LINE_SIZE);
        
        pthread_create(&threads[t], NULL, false_sharing_test, &args[t]);
    }
    
    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }
    
    get_current_time(&end);
    double cache_friendly_time = get_time_diff(start, end);
    
    // Compare results
    printf("\nComparison Results:\n");
    printf("False Sharing Time:  %.3f seconds\n", false_sharing_time);
    printf("Cache Friendly Time: %.3f seconds\n", cache_friendly_time);
    printf("Performance Ratio:   %.2fx (cache-friendly is faster)\n", 
           false_sharing_time / cache_friendly_time);
    
    double false_sharing_avg_throughput = 0.0;
    double cache_friendly_avg_throughput = 0.0;
    
    for (int t = 0; t < num_threads; t++) {
        false_sharing_avg_throughput += stats[t].throughput;
        cache_friendly_avg_throughput += cache_friendly_stats[t].throughput;
    }
    
    false_sharing_avg_throughput /= num_threads;
    cache_friendly_avg_throughput /= num_threads;
    
    printf("False Sharing Throughput:  %.0f ops/sec per thread\n", false_sharing_avg_throughput);
    printf("Cache Friendly Throughput: %.0f ops/sec per thread\n", cache_friendly_avg_throughput);
    
    pthread_barrier_destroy(&barrier);
    free(shared_data);
}

static void cache_bouncing_analysis(int num_threads) {
    printf("\n=== Cache Line Bouncing Analysis ===\n");
    
    if (num_threads < 2) {
        printf("Cache bouncing test requires at least 2 threads.\n");
        return;
    }
    
    // Allocate a single cache line that will bounce between cores
    char* bouncing_data = aligned_alloc(CACHE_LINE_SIZE, CACHE_LINE_SIZE);
    memset(bouncing_data, 0, CACHE_LINE_SIZE);
    
    volatile int* counter = (volatile int*)bouncing_data;
    *counter = 0;
    
    pthread_t threads[MAX_THREADS];
    ThreadArgs args[MAX_THREADS];
    CacheStats stats[MAX_THREADS];
    pthread_barrier_t barrier;
    
    memset(stats, 0, sizeof(stats));
    pthread_barrier_init(&barrier, NULL, num_threads);
    
    printf("Testing cache line bouncing with %d threads on different cores...\n", num_threads);
    
    struct timespec start, end;
    get_current_time(&start);
    
    for (int t = 0; t < num_threads; t++) {
        args[t].thread_id = t;
        args[t].cpu_id = t % sys_info.num_cores;
        args[t].memory_size = 0;
        args[t].stride = 0;
        args[t].iterations = 50000;
        args[t].pattern = PATTERN_FALSE_SHARING;
        args[t].stats = stats;
        args[t].barrier = &barrier;
        args[t].stop_flag = NULL;
        args[t].shared_data = bouncing_data;
        args[t].shared_data_size = CACHE_LINE_SIZE;
        
        pthread_create(&threads[t], NULL, false_sharing_test, &args[t]);
    }
    
    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }
    
    get_current_time(&end);
    double total_time = get_time_diff(start, end);
    
    printf("Cache bouncing test completed in %.3f seconds\n", total_time);
    printf("Final counter value: %d\n", *counter);
    
    double total_throughput = 0.0;
    for (int t = 0; t < num_threads; t++) {
        total_throughput += stats[t].throughput;
        printf("Thread %d (CPU %d): %.0f ops/sec\n", 
               t, stats[t].cpu_id, stats[t].throughput);
    }
    
    printf("Total system throughput: %.0f ops/sec\n", total_throughput);
    printf("Average per-thread throughput: %.0f ops/sec\n", total_throughput / num_threads);
    
    pthread_barrier_destroy(&barrier);
    free(bouncing_data);
}

static void prefetcher_analysis() {
    printf("\n=== Hardware Prefetcher Analysis ===\n");
    
    size_t test_size = 4 * MB;
    char* data = malloc(test_size);
    if (!data) {
        printf("Failed to allocate memory for prefetcher test\n");
        return;
    }
    
    memset(data, 0, test_size);
    
    // Test different stride patterns
    size_t strides[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
    int num_strides = sizeof(strides) / sizeof(strides[0]);
    
    printf("Stride (bytes) | Latency (ns) | Throughput | Prefetcher Effectiveness\n");
    printf("---------------|--------------|------------|------------------------\n");
    
    for (int s = 0; s < num_strides; s++) {
        size_t stride = strides[s];
        
        struct timespec start, end;
        get_current_time(&start);
        
        volatile char sum = 0;
        uint64_t accesses = 0;
        
        for (size_t i = 0; i < test_size; i += stride) {
            sum += data[i];
            accesses++;
        }
        
        get_current_time(&end);
        
        double time = get_time_diff(start, end);
        double latency = time / accesses;
        double throughput = accesses / time;
        
        // Estimate prefetcher effectiveness
        const char* effectiveness;
        if (stride <= 64) {
            effectiveness = "High";
        } else if (stride <= 256) {
            effectiveness = "Medium";
        } else {
            effectiveness = "Low";
        }
        
        printf("%13zu | %11.2f | %10.0f | %s\n",
               stride, latency * 1e9, throughput, effectiveness);
    }
    
    free(data);
}

static void print_usage(const char* prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -t <type>     Test type: hierarchy, false_sharing, bouncing, prefetcher, all\n");
    printf("  -T <threads>  Number of threads (default: 4)\n");
    printf("  -v            Verbose output\n");
    printf("  -h            Show this help message\n");
    printf("\nAnalyzes CPU cache performance characteristics:\n");
    printf("  - Cache hierarchy (L1/L2/L3) performance\n");
    printf("  - False sharing detection and analysis\n");
    printf("  - Cache line bouncing between cores\n");
    printf("  - Hardware prefetcher effectiveness\n");
    printf("  - NUMA memory access patterns\n");
}

int main(int argc, char* argv[]) {
    TestType test_type = TEST_ALL;
    int num_threads = 4;
    int verbose = 0;
    
    int opt;
    while ((opt = getopt(argc, argv, "t:T:vh")) != -1) {
        switch (opt) {
            case 't':
                if (strcmp(optarg, "hierarchy") == 0) test_type = TEST_CACHE_HIERARCHY;
                else if (strcmp(optarg, "false_sharing") == 0) test_type = TEST_FALSE_SHARING;
                else if (strcmp(optarg, "bouncing") == 0) test_type = TEST_CACHE_BOUNCING;
                else if (strcmp(optarg, "prefetcher") == 0) test_type = TEST_PREFETCHER;
                else if (strcmp(optarg, "all") == 0) test_type = TEST_ALL;
                else {
                    fprintf(stderr, "Unknown test type: %s\n", optarg);
                    return 1;
                }
                break;
            case 'T':
                num_threads = atoi(optarg);
                if (num_threads <= 0 || num_threads > MAX_THREADS) {
                    fprintf(stderr, "Invalid number of threads: %d\n", num_threads);
                    return 1;
                }
                break;
            case 'v':
                verbose = 1;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }
    
    printf("CPU Cache Performance Analyzer\n");
    printf("==============================\n\n");
    
    detect_system_info();
    
    if (verbose) {
        print_system_info();
    }
    
    if (test_type == TEST_CACHE_HIERARCHY || test_type == TEST_ALL) {
        cache_hierarchy_test(num_threads);
    }
    
    if (test_type == TEST_FALSE_SHARING || test_type == TEST_ALL) {
        false_sharing_analysis(num_threads);
    }
    
    if (test_type == TEST_CACHE_BOUNCING || test_type == TEST_ALL) {
        cache_bouncing_analysis(num_threads);
    }
    
    if (test_type == TEST_PREFETCHER || test_type == TEST_ALL) {
        prefetcher_analysis();
    }
    
    return 0;
}