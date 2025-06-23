#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <math.h>
#include <assert.h>
#include <signal.h>
#include <stdbool.h>

#ifdef __APPLE__
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
#endif

#define MAX_ALLOCATIONS 100000
#define MAX_THREADS 32
#define DEFAULT_ITERATIONS 1000
#define DEFAULT_THREADS 4
#define HISTOGRAM_BUCKETS 50
#define CACHE_LINE_SIZE 64
#define PAGE_SIZE 4096

typedef enum {
    PATTERN_SEQUENTIAL,
    PATTERN_RANDOM,
    PATTERN_EXPONENTIAL,
    PATTERN_BIMODAL,
    PATTERN_REALISTIC
} AllocationPattern;

typedef enum {
    TEST_SPEED,
    TEST_FRAGMENTATION,
    TEST_SCALABILITY,
    TEST_ALL
} TestType;

typedef struct {
    size_t size;
    void* ptr;
    struct timespec alloc_time;
    struct timespec free_time;
    int thread_id;
} AllocationRecord;

typedef struct {
    size_t total_allocated;
    size_t peak_allocated;
    size_t current_allocated;
    size_t allocation_count;
    size_t free_count;
    double total_alloc_time;
    double total_free_time;
    double min_alloc_time;
    double max_alloc_time;
    double min_free_time;
    double max_free_time;
    size_t failed_allocations;
    size_t fragmentation_score;
} AllocatorStats;

typedef struct {
    int thread_id;
    int num_iterations;
    AllocationPattern pattern;
    size_t min_size;
    size_t max_size;
    AllocatorStats* stats;
    pthread_barrier_t* barrier;
    volatile bool* stop_flag;
} ThreadArgs;

struct {
    AllocatorStats global_stats;
    AllocatorStats thread_stats[MAX_THREADS];
    AllocationRecord* records;
    size_t record_count;
    pthread_mutex_t stats_mutex;
    double size_histogram[HISTOGRAM_BUCKETS];
    double time_histogram[HISTOGRAM_BUCKETS];
    size_t histogram_max_size;
} benchmark_data;

static double get_time_diff(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

static void get_current_time(struct timespec* ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static size_t get_allocation_size(AllocationPattern pattern, size_t min_size, size_t max_size, int iteration) {
    size_t range = max_size - min_size;
    
    switch (pattern) {
        case PATTERN_SEQUENTIAL:
            return min_size + (iteration % range);
            
        case PATTERN_RANDOM:
            return min_size + (rand() % range);
            
        case PATTERN_EXPONENTIAL: {
            double r = (double)rand() / RAND_MAX;
            return min_size + (size_t)(range * (1.0 - exp(-3.0 * r)) / (1.0 - exp(-3.0)));
        }
            
        case PATTERN_BIMODAL:
            if (rand() % 100 < 80) {
                return min_size + (rand() % (range / 10));
            } else {
                return max_size - (rand() % (range / 10));
            }
            
        case PATTERN_REALISTIC: {
            int choice = rand() % 100;
            if (choice < 40) return 16 + (rand() % 48);
            if (choice < 70) return 64 + (rand() % 192);
            if (choice < 90) return 256 + (rand() % 768);
            if (choice < 98) return 1024 + (rand() % 3072);
            return 4096 + (rand() % 12288);
        }
    }
    
    return min_size;
}

static void update_stats(AllocatorStats* stats, double alloc_time, double free_time, size_t size, bool success) {
    pthread_mutex_lock(&benchmark_data.stats_mutex);
    
    if (success) {
        stats->total_allocated += size;
        stats->current_allocated += size;
        if (stats->current_allocated > stats->peak_allocated) {
            stats->peak_allocated = stats->current_allocated;
        }
        stats->allocation_count++;
        stats->total_alloc_time += alloc_time;
        
        if (alloc_time < stats->min_alloc_time || stats->min_alloc_time == 0) {
            stats->min_alloc_time = alloc_time;
        }
        if (alloc_time > stats->max_alloc_time) {
            stats->max_alloc_time = alloc_time;
        }
    } else {
        stats->failed_allocations++;
    }
    
    if (free_time > 0) {
        stats->current_allocated -= size;
        stats->free_count++;
        stats->total_free_time += free_time;
        
        if (free_time < stats->min_free_time || stats->min_free_time == 0) {
            stats->min_free_time = free_time;
        }
        if (free_time > stats->max_free_time) {
            stats->max_free_time = free_time;
        }
    }
    
    pthread_mutex_unlock(&benchmark_data.stats_mutex);
}

static void update_histogram(size_t size, double time) {
    pthread_mutex_lock(&benchmark_data.stats_mutex);
    
    int size_bucket = (size * HISTOGRAM_BUCKETS) / benchmark_data.histogram_max_size;
    if (size_bucket >= HISTOGRAM_BUCKETS) size_bucket = HISTOGRAM_BUCKETS - 1;
    benchmark_data.size_histogram[size_bucket]++;
    
    int time_bucket = (int)(time * 1e6 / 10.0);
    if (time_bucket >= HISTOGRAM_BUCKETS) time_bucket = HISTOGRAM_BUCKETS - 1;
    if (time_bucket < 0) time_bucket = 0;
    benchmark_data.time_histogram[time_bucket]++;
    
    pthread_mutex_unlock(&benchmark_data.stats_mutex);
}

static void* allocation_speed_test(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    AllocationRecord* local_records = malloc(args->num_iterations * sizeof(AllocationRecord));
    
    if (args->barrier) {
        pthread_barrier_wait(args->barrier);
    }
    
    for (int i = 0; i < args->num_iterations && !*args->stop_flag; i++) {
        size_t size = get_allocation_size(args->pattern, args->min_size, args->max_size, i);
        struct timespec start, end;
        
        get_current_time(&start);
        void* ptr = malloc(size);
        get_current_time(&end);
        
        double alloc_time = get_time_diff(start, end);
        
        if (ptr) {
            memset(ptr, 0xAA, size);
            
            local_records[i].size = size;
            local_records[i].ptr = ptr;
            local_records[i].alloc_time = start;
            local_records[i].thread_id = args->thread_id;
            
            update_stats(&args->stats[args->thread_id], alloc_time, 0, size, true);
            update_histogram(size, alloc_time);
            
            if (i % 2 == 0 && i > 0) {
                int free_idx = rand() % i;
                if (local_records[free_idx].ptr) {
                    get_current_time(&start);
                    free(local_records[free_idx].ptr);
                    get_current_time(&end);
                    
                    double free_time = get_time_diff(start, end);
                    update_stats(&args->stats[args->thread_id], 0, free_time, 
                               local_records[free_idx].size, true);
                    update_histogram(local_records[free_idx].size, free_time);
                    
                    local_records[free_idx].ptr = NULL;
                    local_records[free_idx].free_time = end;
                }
            }
        } else {
            update_stats(&args->stats[args->thread_id], alloc_time, 0, size, false);
        }
    }
    
    for (int i = 0; i < args->num_iterations; i++) {
        if (local_records[i].ptr) {
            struct timespec start, end;
            get_current_time(&start);
            free(local_records[i].ptr);
            get_current_time(&end);
            
            double free_time = get_time_diff(start, end);
            update_stats(&args->stats[args->thread_id], 0, free_time, 
                       local_records[i].size, true);
        }
    }
    
    free(local_records);
    return NULL;
}

static double calculate_fragmentation() {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    
    size_t rss = usage.ru_maxrss;
    #ifdef __APPLE__
    rss = rss / 1024;
    #endif
    
    size_t heap_used = benchmark_data.global_stats.current_allocated;
    
    if (heap_used == 0) return 0.0;
    
    double fragmentation = ((double)(rss * 1024) / heap_used) - 1.0;
    return fragmentation * 100.0;
}

static void fragmentation_test(size_t min_size, size_t max_size) {
    printf("\n=== Fragmentation Test ===\n");
    
    const int num_allocations = 10000;
    void** ptrs = calloc(num_allocations, sizeof(void*));
    size_t* sizes = calloc(num_allocations, sizeof(size_t));
    
    for (int round = 0; round < 5; round++) {
        printf("\nRound %d:\n", round + 1);
        
        for (int i = 0; i < num_allocations; i++) {
            sizes[i] = get_allocation_size(PATTERN_RANDOM, min_size, max_size, i);
            ptrs[i] = malloc(sizes[i]);
            if (ptrs[i]) {
                memset(ptrs[i], 0xBB, sizes[i]);
                benchmark_data.global_stats.current_allocated += sizes[i];
                benchmark_data.global_stats.total_allocated += sizes[i];
            }
        }
        
        double frag_before = calculate_fragmentation();
        printf("  Fragmentation before freeing: %.2f%%\n", frag_before);
        
        if (round % 2 == 0) {
            for (int i = 0; i < num_allocations; i += 2) {
                if (ptrs[i]) {
                    free(ptrs[i]);
                    benchmark_data.global_stats.current_allocated -= sizes[i];
                    ptrs[i] = NULL;
                }
            }
        } else {
            for (int i = num_allocations - 1; i >= 0; i--) {
                if (rand() % 3 == 0 && ptrs[i]) {
                    free(ptrs[i]);
                    benchmark_data.global_stats.current_allocated -= sizes[i];
                    ptrs[i] = NULL;
                }
            }
        }
        
        double frag_after = calculate_fragmentation();
        printf("  Fragmentation after partial free: %.2f%%\n", frag_after);
        
        size_t holes = 0;
        size_t total_hole_size = 0;
        for (int i = 0; i < num_allocations; i++) {
            if (!ptrs[i] && sizes[i] > 0) {
                holes++;
                total_hole_size += sizes[i];
            }
        }
        printf("  Memory holes: %zu, Total hole size: %.2f MB\n", 
               holes, total_hole_size / (1024.0 * 1024.0));
    }
    
    for (int i = 0; i < num_allocations; i++) {
        if (ptrs[i]) {
            free(ptrs[i]);
        }
    }
    
    free(ptrs);
    free(sizes);
}

static void scalability_test(AllocationPattern pattern, size_t min_size, size_t max_size) {
    printf("\n=== Scalability Test ===\n");
    printf("Testing with pattern: ");
    switch (pattern) {
        case PATTERN_SEQUENTIAL: printf("Sequential\n"); break;
        case PATTERN_RANDOM: printf("Random\n"); break;
        case PATTERN_EXPONENTIAL: printf("Exponential\n"); break;
        case PATTERN_BIMODAL: printf("Bimodal\n"); break;
        case PATTERN_REALISTIC: printf("Realistic\n"); break;
    }
    
    int thread_counts[] = {1, 2, 4, 8, 16};
    int num_tests = sizeof(thread_counts) / sizeof(thread_counts[0]);
    
    for (int t = 0; t < num_tests; t++) {
        int num_threads = thread_counts[t];
        if (num_threads > MAX_THREADS) continue;
        
        printf("\n%d Thread(s):\n", num_threads);
        
        memset(&benchmark_data.global_stats, 0, sizeof(AllocatorStats));
        memset(benchmark_data.thread_stats, 0, sizeof(benchmark_data.thread_stats));
        
        pthread_t threads[MAX_THREADS];
        ThreadArgs args[MAX_THREADS];
        pthread_barrier_t barrier;
        volatile bool stop_flag = false;
        
        pthread_barrier_init(&barrier, NULL, num_threads);
        
        struct timespec start, end;
        get_current_time(&start);
        
        for (int i = 0; i < num_threads; i++) {
            args[i].thread_id = i;
            args[i].num_iterations = DEFAULT_ITERATIONS / num_threads;
            args[i].pattern = pattern;
            args[i].min_size = min_size;
            args[i].max_size = max_size;
            args[i].stats = benchmark_data.thread_stats;
            args[i].barrier = &barrier;
            args[i].stop_flag = &stop_flag;
            
            pthread_create(&threads[i], NULL, allocation_speed_test, &args[i]);
        }
        
        for (int i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
        }
        
        get_current_time(&end);
        double total_time = get_time_diff(start, end);
        
        size_t total_ops = 0;
        double total_alloc_time = 0;
        double total_free_time = 0;
        
        for (int i = 0; i < num_threads; i++) {
            total_ops += benchmark_data.thread_stats[i].allocation_count;
            total_ops += benchmark_data.thread_stats[i].free_count;
            total_alloc_time += benchmark_data.thread_stats[i].total_alloc_time;
            total_free_time += benchmark_data.thread_stats[i].total_free_time;
            
            benchmark_data.global_stats.total_allocated += benchmark_data.thread_stats[i].total_allocated;
            benchmark_data.global_stats.allocation_count += benchmark_data.thread_stats[i].allocation_count;
            benchmark_data.global_stats.free_count += benchmark_data.thread_stats[i].free_count;
        }
        
        printf("  Total operations: %zu\n", total_ops);
        printf("  Operations/second: %.0f\n", total_ops / total_time);
        printf("  Avg allocation time: %.2f μs\n", 
               (total_alloc_time / benchmark_data.global_stats.allocation_count) * 1e6);
        printf("  Avg free time: %.2f μs\n", 
               (total_free_time / benchmark_data.global_stats.free_count) * 1e6);
        printf("  Throughput: %.2f MB/s\n", 
               (benchmark_data.global_stats.total_allocated / (1024.0 * 1024.0)) / total_time);
        
        double speedup = 1.0;
        if (t > 0) {
            speedup = (double)total_ops / (thread_counts[0] * (total_ops / num_threads));
            printf("  Speedup: %.2fx\n", speedup);
            printf("  Efficiency: %.2f%%\n", (speedup / num_threads) * 100);
        }
        
        pthread_barrier_destroy(&barrier);
    }
}

static void print_histogram(const char* title, double* histogram, int buckets, bool is_size) {
    printf("\n%s:\n", title);
    
    double max_count = 0;
    for (int i = 0; i < buckets; i++) {
        if (histogram[i] > max_count) max_count = histogram[i];
    }
    
    if (max_count == 0) return;
    
    for (int i = 0; i < buckets; i++) {
        if (histogram[i] == 0) continue;
        
        if (is_size) {
            size_t bucket_size = (benchmark_data.histogram_max_size * i) / buckets;
            printf("%6zu KB: ", bucket_size / 1024);
        } else {
            printf("%3d-%3d μs: ", i * 10, (i + 1) * 10);
        }
        
        int bar_len = (int)((histogram[i] / max_count) * 50);
        for (int j = 0; j < bar_len; j++) printf("█");
        printf(" %.0f\n", histogram[i]);
    }
}

static void print_summary() {
    printf("\n=== Summary Statistics ===\n");
    
    printf("\nMemory Usage:\n");
    printf("  Total allocated: %.2f MB\n", 
           benchmark_data.global_stats.total_allocated / (1024.0 * 1024.0));
    printf("  Peak allocated: %.2f MB\n", 
           benchmark_data.global_stats.peak_allocated / (1024.0 * 1024.0));
    printf("  Total allocations: %zu\n", benchmark_data.global_stats.allocation_count);
    printf("  Failed allocations: %zu\n", benchmark_data.global_stats.failed_allocations);
    
    printf("\nPerformance:\n");
    printf("  Average allocation time: %.2f μs\n", 
           (benchmark_data.global_stats.total_alloc_time / benchmark_data.global_stats.allocation_count) * 1e6);
    printf("  Min allocation time: %.2f μs\n", benchmark_data.global_stats.min_alloc_time * 1e6);
    printf("  Max allocation time: %.2f μs\n", benchmark_data.global_stats.max_alloc_time * 1e6);
    
    if (benchmark_data.global_stats.free_count > 0) {
        printf("  Average free time: %.2f μs\n", 
               (benchmark_data.global_stats.total_free_time / benchmark_data.global_stats.free_count) * 1e6);
        printf("  Min free time: %.2f μs\n", benchmark_data.global_stats.min_free_time * 1e6);
        printf("  Max free time: %.2f μs\n", benchmark_data.global_stats.max_free_time * 1e6);
    }
    
    print_histogram("Size Distribution", benchmark_data.size_histogram, HISTOGRAM_BUCKETS, true);
    print_histogram("Allocation Time Distribution", benchmark_data.time_histogram, HISTOGRAM_BUCKETS, false);
}

static void print_usage(const char* prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -t <type>     Test type: speed, frag, scale, all (default: all)\n");
    printf("  -p <pattern>  Allocation pattern: seq, rand, exp, bimodal, real (default: real)\n");
    printf("  -s <size>     Min allocation size in bytes (default: 8)\n");
    printf("  -S <size>     Max allocation size in bytes (default: 8192)\n");
    printf("  -n <count>    Number of iterations (default: 1000)\n");
    printf("  -T <threads>  Number of threads for scalability test (default: 4)\n");
    printf("  -v            Verbose output\n");
    printf("  -h            Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s -t speed -p rand -n 10000\n", prog_name);
    printf("  %s -t scale -T 16\n", prog_name);
    printf("  %s -t frag -s 64 -S 4096\n", prog_name);
}

int main(int argc, char* argv[]) {
    TestType test_type = TEST_ALL;
    AllocationPattern pattern = PATTERN_REALISTIC;
    size_t min_size = 8;
    size_t max_size = 8192;
    int num_iterations = DEFAULT_ITERATIONS;
    int num_threads = DEFAULT_THREADS;
    
    int opt;
    while ((opt = getopt(argc, argv, "t:p:s:S:n:T:vh")) != -1) {
        switch (opt) {
            case 't':
                if (strcmp(optarg, "speed") == 0) test_type = TEST_SPEED;
                else if (strcmp(optarg, "frag") == 0) test_type = TEST_FRAGMENTATION;
                else if (strcmp(optarg, "scale") == 0) test_type = TEST_SCALABILITY;
                else if (strcmp(optarg, "all") == 0) test_type = TEST_ALL;
                break;
                
            case 'p':
                if (strcmp(optarg, "seq") == 0) pattern = PATTERN_SEQUENTIAL;
                else if (strcmp(optarg, "rand") == 0) pattern = PATTERN_RANDOM;
                else if (strcmp(optarg, "exp") == 0) pattern = PATTERN_EXPONENTIAL;
                else if (strcmp(optarg, "bimodal") == 0) pattern = PATTERN_BIMODAL;
                else if (strcmp(optarg, "real") == 0) pattern = PATTERN_REALISTIC;
                break;
                
            case 's':
                min_size = atoi(optarg);
                break;
                
            case 'S':
                max_size = atoi(optarg);
                break;
                
            case 'n':
                num_iterations = atoi(optarg);
                break;
                
            case 'T':
                num_threads = atoi(optarg);
                if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;
                break;
                
            case 'v':
                printf("Verbose mode enabled\n");
                break;
                
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }
    
    printf("Memory Allocator Benchmarker\n");
    printf("============================\n");
    
    pthread_mutex_init(&benchmark_data.stats_mutex, NULL);
    memset(&benchmark_data, 0, sizeof(benchmark_data));
    benchmark_data.histogram_max_size = max_size;
    
    if (test_type == TEST_SPEED || test_type == TEST_ALL) {
        printf("\n=== Speed Test ===\n");
        
        ThreadArgs args = {
            .thread_id = 0,
            .num_iterations = num_iterations,
            .pattern = pattern,
            .min_size = min_size,
            .max_size = max_size,
            .stats = benchmark_data.thread_stats,
            .barrier = NULL,
            .stop_flag = &(volatile bool){false}
        };
        
        struct timespec start, end;
        get_current_time(&start);
        
        allocation_speed_test(&args);
        
        get_current_time(&end);
        double total_time = get_time_diff(start, end);
        
        printf("Completed %d iterations in %.3f seconds\n", num_iterations, total_time);
        printf("Operations per second: %.0f\n", 
               (args.stats[0].allocation_count + args.stats[0].free_count) / total_time);
        
        benchmark_data.global_stats = args.stats[0];
    }
    
    if (test_type == TEST_FRAGMENTATION || test_type == TEST_ALL) {
        memset(&benchmark_data.global_stats, 0, sizeof(AllocatorStats));
        fragmentation_test(min_size, max_size);
    }
    
    if (test_type == TEST_SCALABILITY || test_type == TEST_ALL) {
        scalability_test(pattern, min_size, max_size);
    }
    
    if (test_type == TEST_SPEED || test_type == TEST_ALL) {
        print_summary();
    }
    
    pthread_mutex_destroy(&benchmark_data.stats_mutex);
    
    return 0;
}