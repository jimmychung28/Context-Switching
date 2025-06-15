/*
 * Lock Contention Visualizer
 * Analyzes and visualizes lock contention patterns for different synchronization primitives
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>

#define MAX_THREADS 64
#define DEFAULT_DURATION 10
#define DEFAULT_THREADS 8
#define WORK_ITERATIONS 1000
#define MAX_SAMPLES 10000
#define HISTOGRAM_BUCKETS 50

// Lock types
typedef enum {
    LOCK_MUTEX,
    LOCK_SPINLOCK,
    LOCK_RWLOCK,
    LOCK_ADAPTIVE
} lock_type_t;

// Workload patterns
typedef enum {
    WORKLOAD_BALANCED,
    WORKLOAD_READ_HEAVY,
    WORKLOAD_WRITE_HEAVY,
    WORKLOAD_THUNDERING_HERD,
    WORKLOAD_RANDOM
} workload_pattern_t;

// Custom spinlock implementation
typedef struct {
    atomic_flag flag;
    atomic_int waiters;
} spinlock_t;

// Lock wrapper to support different types
typedef struct {
    lock_type_t type;
    union {
        pthread_mutex_t mutex;
        spinlock_t spinlock;
        pthread_rwlock_t rwlock;
    } lock;
} lock_wrapper_t;

// Thread statistics
typedef struct {
    int thread_id;
    unsigned long acquisitions;
    unsigned long contentions;
    unsigned long read_ops;
    unsigned long write_ops;
    double total_wait_time;
    double total_hold_time;
    double max_wait_time;
    double min_wait_time;
    
    // Contention samples for visualization
    double wait_samples[MAX_SAMPLES];
    int sample_count;
    
    // Per-thread timing
    struct timespec start_time;
    struct timespec end_time;
} thread_stats_t;

// Global configuration
typedef struct {
    int num_threads;
    int duration_seconds;
    lock_type_t lock_type;
    workload_pattern_t workload;
    int read_percentage;  // For read/write workloads
    int work_inside_lock; // Microseconds of work inside critical section
    int work_outside_lock; // Microseconds of work outside critical section
    bool verbose;
    bool realtime_viz;
} config_t;

// Global variables
static volatile int g_stop_all = 0;
static thread_stats_t thread_stats[MAX_THREADS];
static lock_wrapper_t global_lock;
static config_t config;
static int shared_data = 0;

// Atomic counters for global statistics
static atomic_long g_total_acquisitions = 0;
static atomic_long g_total_contentions = 0;
static atomic_int g_current_waiters = 0;
static atomic_int g_max_waiters = 0;

// Function prototypes
void spinlock_init(spinlock_t *lock);
void spinlock_lock(spinlock_t *lock);
void spinlock_unlock(spinlock_t *lock);
int spinlock_trylock(spinlock_t *lock);

// Get current time in seconds
double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Spinlock implementation
void spinlock_init(spinlock_t *lock) {
    atomic_flag_clear(&lock->flag);
    atomic_init(&lock->waiters, 0);
}

void spinlock_lock(spinlock_t *lock) {
    atomic_fetch_add(&lock->waiters, 1);
    while (atomic_flag_test_and_set(&lock->flag)) {
        // Spin with pause instruction (x86) or yield
        #ifdef __x86_64__
            __asm__ __volatile__("pause");
        #else
            sched_yield();
        #endif
    }
    atomic_fetch_sub(&lock->waiters, 1);
}

void spinlock_unlock(spinlock_t *lock) {
    atomic_flag_clear(&lock->flag);
}

int spinlock_trylock(spinlock_t *lock) {
    return !atomic_flag_test_and_set(&lock->flag);
}

// Initialize lock wrapper
void lock_init(lock_wrapper_t *lock, lock_type_t type) {
    lock->type = type;
    switch (type) {
        case LOCK_MUTEX:
            pthread_mutex_init(&lock->lock.mutex, NULL);
            break;
        case LOCK_SPINLOCK:
            spinlock_init(&lock->lock.spinlock);
            break;
        case LOCK_RWLOCK:
            pthread_rwlock_init(&lock->lock.rwlock, NULL);
            break;
        case LOCK_ADAPTIVE:
            // Adaptive mutex not available on macOS, use regular mutex
            pthread_mutex_init(&lock->lock.mutex, NULL);
            break;
    }
}

// Acquire lock with timing
double lock_acquire(lock_wrapper_t *lock, thread_stats_t *stats, bool is_read) {
    double start_time = get_time();
    
    atomic_fetch_add(&g_current_waiters, 1);
    int current_waiters = atomic_load(&g_current_waiters);
    int max_waiters = atomic_load(&g_max_waiters);
    if (current_waiters > max_waiters) {
        atomic_store(&g_max_waiters, current_waiters);
    }
    
    switch (lock->type) {
        case LOCK_MUTEX:
        case LOCK_ADAPTIVE:
            pthread_mutex_lock(&lock->lock.mutex);
            break;
        case LOCK_SPINLOCK:
            spinlock_lock(&lock->lock.spinlock);
            break;
        case LOCK_RWLOCK:
            if (is_read) {
                pthread_rwlock_rdlock(&lock->lock.rwlock);
            } else {
                pthread_rwlock_wrlock(&lock->lock.rwlock);
            }
            break;
    }
    
    atomic_fetch_sub(&g_current_waiters, 1);
    double wait_time = get_time() - start_time;
    
    // Record contention if we had to wait
    if (wait_time > 0.000001) { // 1 microsecond threshold
        stats->contentions++;
        atomic_fetch_add(&g_total_contentions, 1);
    }
    
    return wait_time;
}

// Release lock
void lock_release(lock_wrapper_t *lock, bool is_read) {
    switch (lock->type) {
        case LOCK_MUTEX:
        case LOCK_ADAPTIVE:
            pthread_mutex_unlock(&lock->lock.mutex);
            break;
        case LOCK_SPINLOCK:
            spinlock_unlock(&lock->lock.spinlock);
            break;
        case LOCK_RWLOCK:
            pthread_rwlock_unlock(&lock->lock.rwlock);
            break;
    }
}

// Destroy lock
void lock_destroy(lock_wrapper_t *lock) {
    switch (lock->type) {
        case LOCK_MUTEX:
        case LOCK_ADAPTIVE:
            pthread_mutex_destroy(&lock->lock.mutex);
            break;
        case LOCK_SPINLOCK:
            // No cleanup needed
            break;
        case LOCK_RWLOCK:
            pthread_rwlock_destroy(&lock->lock.rwlock);
            break;
    }
}

// Simulate work
void do_work(int microseconds) {
    if (microseconds > 0) {
        usleep(microseconds);
    }
}

// Critical section work
void critical_section_work(bool is_read) {
    if (is_read) {
        // Read operation
        volatile int temp = shared_data;
        (void)temp; // Suppress unused warning
    } else {
        // Write operation
        shared_data++;
    }
    
    // Simulate work inside lock
    do_work(config.work_inside_lock);
}

// Thread worker function
void* thread_worker(void* arg) {
    thread_stats_t *stats = (thread_stats_t*)arg;
    
    // Initialize stats
    stats->min_wait_time = INFINITY;
    stats->max_wait_time = 0;
    stats->sample_count = 0;
    
    clock_gettime(CLOCK_MONOTONIC, &stats->start_time);
    
    // Thundering herd: all threads start together
    if (config.workload == WORKLOAD_THUNDERING_HERD) {
        static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
        static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
        static atomic_int ready_threads = 0;
        static atomic_bool all_ready = false;
        
        pthread_mutex_lock(&start_mutex);
        atomic_fetch_add(&ready_threads, 1);
        
        if (atomic_load(&ready_threads) == config.num_threads) {
            atomic_store(&all_ready, true);
            pthread_cond_broadcast(&start_cond);
        } else {
            while (!atomic_load(&all_ready)) {
                pthread_cond_wait(&start_cond, &start_mutex);
            }
        }
        pthread_mutex_unlock(&start_mutex);
    }
    
    while (!g_stop_all) {
        // Determine operation type based on workload
        bool is_read = false;
        switch (config.workload) {
            case WORKLOAD_BALANCED:
                is_read = (stats->acquisitions % 2) == 0;
                break;
            case WORKLOAD_READ_HEAVY:
                is_read = (rand() % 100) < config.read_percentage;
                break;
            case WORKLOAD_WRITE_HEAVY:
                is_read = (rand() % 100) < config.read_percentage;
                break;
            case WORKLOAD_THUNDERING_HERD:
                is_read = false; // All writes for maximum contention
                break;
            case WORKLOAD_RANDOM:
                is_read = (rand() % 2) == 0;
                break;
        }
        
        // Acquire lock and measure wait time
        double wait_time = lock_acquire(&global_lock, stats, is_read);
        double hold_start = get_time();
        
        // Critical section
        critical_section_work(is_read);
        
        // Release lock
        double hold_time = get_time() - hold_start;
        lock_release(&global_lock, is_read);
        
        // Update statistics
        stats->acquisitions++;
        atomic_fetch_add(&g_total_acquisitions, 1);
        stats->total_wait_time += wait_time;
        stats->total_hold_time += hold_time;
        
        if (wait_time < stats->min_wait_time) stats->min_wait_time = wait_time;
        if (wait_time > stats->max_wait_time) stats->max_wait_time = wait_time;
        
        // Store sample for visualization
        if (stats->sample_count < MAX_SAMPLES) {
            stats->wait_samples[stats->sample_count++] = wait_time;
        }
        
        if (is_read) {
            stats->read_ops++;
        } else {
            stats->write_ops++;
        }
        
        // Work outside critical section
        do_work(config.work_outside_lock);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &stats->end_time);
    return NULL;
}

// Generate histogram
void print_histogram(double* values, int count, const char* title) {
    if (count == 0) return;
    
    // Find min and max
    double min_val = INFINITY, max_val = 0;
    for (int i = 0; i < count; i++) {
        if (values[i] < min_val) min_val = values[i];
        if (values[i] > max_val) max_val = values[i];
    }
    
    if (max_val == 0) return;
    
    // Create histogram
    int histogram[HISTOGRAM_BUCKETS] = {0};
    double bucket_width = (max_val - min_val) / HISTOGRAM_BUCKETS;
    
    for (int i = 0; i < count; i++) {
        int bucket = (int)((values[i] - min_val) / bucket_width);
        if (bucket >= HISTOGRAM_BUCKETS) bucket = HISTOGRAM_BUCKETS - 1;
        histogram[bucket]++;
    }
    
    // Find max count for scaling
    int max_count = 0;
    for (int i = 0; i < HISTOGRAM_BUCKETS; i++) {
        if (histogram[i] > max_count) max_count = histogram[i];
    }
    
    // Print histogram
    printf("\n%s\n", title);
    printf("Wait Time Distribution (in microseconds):\n");
    
    for (int i = 0; i < HISTOGRAM_BUCKETS; i++) {
        double bucket_start = min_val + i * bucket_width;
        printf("%8.1f-%-8.1f | ", bucket_start * 1e6, (bucket_start + bucket_width) * 1e6);
        
        int bar_length = (histogram[i] * 50) / max_count;
        for (int j = 0; j < bar_length; j++) {
            printf("█");
        }
        printf(" %d\n", histogram[i]);
    }
}

// Real-time visualization
void* realtime_visualizer(void* arg) {
    (void)arg;
    
    while (!g_stop_all) {
        system("clear");
        
        long total_acq = atomic_load(&g_total_acquisitions);
        long total_cont = atomic_load(&g_total_contentions);
        int current_waiters = atomic_load(&g_current_waiters);
        int max_waiters = atomic_load(&g_max_waiters);
        
        printf("=== Lock Contention Real-time Monitor ===\n\n");
        printf("Total Acquisitions: %ld\n", total_acq);
        printf("Total Contentions: %ld (%.1f%%)\n", total_cont, 
               total_acq > 0 ? (total_cont * 100.0 / total_acq) : 0);
        printf("Current Waiters: %d\n", current_waiters);
        printf("Max Waiters: %d\n\n", max_waiters);
        
        // ASCII visualization of current contention
        printf("Contention Level: ");
        int level = (current_waiters * 20) / config.num_threads;
        for (int i = 0; i < 20; i++) {
            if (i < level) printf("█");
            else printf("░");
        }
        printf(" %d/%d\n", current_waiters, config.num_threads);
        
        sleep(1);
    }
    
    return NULL;
}

// Print final results
void print_results() {
    printf("\n=== Lock Contention Analysis Results ===\n\n");
    
    // Lock type info
    const char* lock_names[] = {"Mutex", "Spinlock", "RWLock", "Adaptive Mutex"};
    const char* workload_names[] = {"Balanced", "Read-Heavy", "Write-Heavy", "Thundering Herd", "Random"};
    
    printf("Configuration:\n");
    printf("  Lock Type: %s\n", lock_names[config.lock_type]);
    printf("  Workload: %s\n", workload_names[config.workload]);
    printf("  Threads: %d\n", config.num_threads);
    printf("  Duration: %d seconds\n", config.duration_seconds);
    printf("  Work inside lock: %d μs\n", config.work_inside_lock);
    printf("  Work outside lock: %d μs\n\n", config.work_outside_lock);
    
    // Global statistics
    long total_acq = atomic_load(&g_total_acquisitions);
    long total_cont = atomic_load(&g_total_contentions);
    
    printf("Global Statistics:\n");
    printf("  Total Lock Acquisitions: %ld\n", total_acq);
    printf("  Total Contentions: %ld (%.1f%%)\n", total_cont, 
           total_acq > 0 ? (total_cont * 100.0 / total_acq) : 0);
    printf("  Max Concurrent Waiters: %d\n", atomic_load(&g_max_waiters));
    printf("  Acquisitions/second: %.0f\n\n", total_acq / (double)config.duration_seconds);
    
    // Per-thread statistics
    printf("Per-Thread Statistics:\n");
    printf("Thread | Acquisitions | Contentions | Cont%% | Avg Wait(μs) | Max Wait(μs) | Reads | Writes\n");
    printf("-------|--------------|-------------|-------|--------------|--------------|-------|--------\n");
    
    double total_wait_time = 0;
    double all_wait_samples[MAX_THREADS * MAX_SAMPLES];
    int total_samples = 0;
    
    for (int i = 0; i < config.num_threads; i++) {
        thread_stats_t *stats = &thread_stats[i];
        if (stats->acquisitions > 0) {
            double avg_wait = stats->total_wait_time / stats->acquisitions * 1e6; // Convert to microseconds
            double cont_rate = (stats->contentions * 100.0) / stats->acquisitions;
            
            printf("%6d | %12lu | %11lu | %5.1f | %12.1f | %12.1f | %6lu | %7lu\n",
                   stats->thread_id, stats->acquisitions, stats->contentions,
                   cont_rate, avg_wait, stats->max_wait_time * 1e6,
                   stats->read_ops, stats->write_ops);
            
            total_wait_time += stats->total_wait_time;
            
            // Collect all samples
            for (int j = 0; j < stats->sample_count && total_samples < MAX_THREADS * MAX_SAMPLES; j++) {
                all_wait_samples[total_samples++] = stats->wait_samples[j];
            }
        }
    }
    
    // Contention analysis
    printf("\n=== Contention Analysis ===\n");
    
    if (total_cont > 0) {
        printf("Average contention rate: %.1f%%\n", (total_cont * 100.0) / total_acq);
        printf("Average wait time per contention: %.1f μs\n", 
               (total_wait_time / total_cont) * 1e6);
    }
    
    // Generate wait time histogram
    if (total_samples > 0) {
        print_histogram(all_wait_samples, total_samples, "Wait Time Distribution");
    }
    
    // Lock-specific insights
    printf("\n=== Performance Insights ===\n");
    
    switch (config.lock_type) {
        case LOCK_SPINLOCK:
            if ((total_cont * 100.0 / total_acq) > 30) {
                printf("⚠ High contention detected for spinlock. Consider using mutex instead.\n");
            }
            break;
        case LOCK_RWLOCK:
            {
                long total_reads = 0, total_writes = 0;
                for (int i = 0; i < config.num_threads; i++) {
                    total_reads += thread_stats[i].read_ops;
                    total_writes += thread_stats[i].write_ops;
                }
                double read_ratio = total_reads / (double)(total_reads + total_writes) * 100;
                printf("Read/Write ratio: %.1f%% reads, %.1f%% writes\n", 
                       read_ratio, 100 - read_ratio);
                if (read_ratio < 70) {
                    printf("⚠ Low read ratio for RWLock. Consider using regular mutex.\n");
                }
            }
            break;
        default:
            break;
    }
}

// Print usage
void print_usage(char* prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -t <threads>    Number of threads (default: %d)\n", DEFAULT_THREADS);
    printf("  -d <seconds>    Duration of test (default: %d)\n", DEFAULT_DURATION);
    printf("  -l <type>       Lock type: mutex, spin, rwlock, adaptive (default: mutex)\n");
    printf("  -w <pattern>    Workload: balanced, read-heavy, write-heavy, thunder, random\n");
    printf("  -r <percent>    Read percentage for read/write workloads (default: 80)\n");
    printf("  -i <microsec>   Work time inside lock (default: 10)\n");
    printf("  -o <microsec>   Work time outside lock (default: 100)\n");
    printf("  -v              Verbose output\n");
    printf("  -z              Enable real-time visualization\n");
    printf("  -h              Show this help\n");
    printf("\nExamples:\n");
    printf("  %s -t 16 -l spin -w thunder -z\n", prog_name);
    printf("  %s -t 8 -l rwlock -w read-heavy -r 90\n", prog_name);
}

// Signal handler
void signal_handler(int sig) {
    g_stop_all = 1;
}

int main(int argc, char* argv[]) {
    // Initialize configuration
    config.num_threads = DEFAULT_THREADS;
    config.duration_seconds = DEFAULT_DURATION;
    config.lock_type = LOCK_MUTEX;
    config.workload = WORKLOAD_BALANCED;
    config.read_percentage = 80;
    config.work_inside_lock = 10;
    config.work_outside_lock = 100;
    config.verbose = false;
    config.realtime_viz = false;
    
    // Parse command line arguments
    int opt;
    while ((opt = getopt(argc, argv, "t:d:l:w:r:i:o:vzh")) != -1) {
        switch (opt) {
            case 't':
                config.num_threads = atoi(optarg);
                if (config.num_threads > MAX_THREADS) {
                    fprintf(stderr, "Error: Maximum %d threads supported\n", MAX_THREADS);
                    return 1;
                }
                break;
            case 'd':
                config.duration_seconds = atoi(optarg);
                break;
            case 'l':
                if (strcmp(optarg, "mutex") == 0) config.lock_type = LOCK_MUTEX;
                else if (strcmp(optarg, "spin") == 0) config.lock_type = LOCK_SPINLOCK;
                else if (strcmp(optarg, "rwlock") == 0) config.lock_type = LOCK_RWLOCK;
                else if (strcmp(optarg, "adaptive") == 0) config.lock_type = LOCK_ADAPTIVE;
                else {
                    fprintf(stderr, "Unknown lock type: %s\n", optarg);
                    return 1;
                }
                break;
            case 'w':
                if (strcmp(optarg, "balanced") == 0) config.workload = WORKLOAD_BALANCED;
                else if (strcmp(optarg, "read-heavy") == 0) {
                    config.workload = WORKLOAD_READ_HEAVY;
                    config.read_percentage = 80;
                }
                else if (strcmp(optarg, "write-heavy") == 0) {
                    config.workload = WORKLOAD_WRITE_HEAVY;
                    config.read_percentage = 20;
                }
                else if (strcmp(optarg, "thunder") == 0) config.workload = WORKLOAD_THUNDERING_HERD;
                else if (strcmp(optarg, "random") == 0) config.workload = WORKLOAD_RANDOM;
                else {
                    fprintf(stderr, "Unknown workload: %s\n", optarg);
                    return 1;
                }
                break;
            case 'r':
                config.read_percentage = atoi(optarg);
                break;
            case 'i':
                config.work_inside_lock = atoi(optarg);
                break;
            case 'o':
                config.work_outside_lock = atoi(optarg);
                break;
            case 'v':
                config.verbose = true;
                break;
            case 'z':
                config.realtime_viz = true;
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
    signal(SIGTERM, signal_handler);
    
    // Initialize lock
    lock_init(&global_lock, config.lock_type);
    
    // Initialize threads
    pthread_t threads[MAX_THREADS];
    pthread_t viz_thread;
    
    printf("Starting Lock Contention Visualizer\n");
    printf("Configuration: %d threads, %s lock, %s workload\n",
           config.num_threads,
           config.lock_type == LOCK_MUTEX ? "mutex" :
           config.lock_type == LOCK_SPINLOCK ? "spinlock" :
           config.lock_type == LOCK_RWLOCK ? "rwlock" : "adaptive",
           config.workload == WORKLOAD_BALANCED ? "balanced" :
           config.workload == WORKLOAD_READ_HEAVY ? "read-heavy" :
           config.workload == WORKLOAD_WRITE_HEAVY ? "write-heavy" :
           config.workload == WORKLOAD_THUNDERING_HERD ? "thundering herd" : "random");
    
    // Start visualization thread if requested
    if (config.realtime_viz) {
        pthread_create(&viz_thread, NULL, realtime_visualizer, NULL);
    }
    
    // Start worker threads
    for (int i = 0; i < config.num_threads; i++) {
        thread_stats[i].thread_id = i;
        if (pthread_create(&threads[i], NULL, thread_worker, &thread_stats[i]) != 0) {
            perror("pthread_create");
            g_stop_all = 1;
            break;
        }
    }
    
    // Run for specified duration
    if (!g_stop_all) {
        sleep(config.duration_seconds);
        g_stop_all = 1;
    }
    
    // Wait for all threads
    for (int i = 0; i < config.num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    if (config.realtime_viz) {
        pthread_join(viz_thread, NULL);
    }
    
    // Print results
    print_results();
    
    // Cleanup
    lock_destroy(&global_lock);
    
    return 0;
}