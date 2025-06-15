/*
 * Scheduler Fairness Analyzer
 * Tests how the OS scheduler distributes CPU time among different workloads
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>

#define MAX_THREADS 32
#define DEFAULT_DURATION 10
#define ITERATIONS_PER_YIELD 1000000
#define IO_BUFFER_SIZE 4096

typedef enum {
    WORKLOAD_CPU,
    WORKLOAD_IO,
    WORKLOAD_MIXED
} workload_type_t;

typedef struct {
    int thread_id;
    workload_type_t type;
    int nice_value;
    pthread_t pthread_id;
    
    // Performance metrics
    unsigned long iterations;
    double cpu_time_used;
    struct timespec start_time;
    struct timespec end_time;
    
    // For IO workload
    int pipe_read_fd;
    int pipe_write_fd;
    
    volatile int should_stop;
} thread_info_t;

typedef struct {
    int num_threads;
    int duration_seconds;
    int cpu_threads;
    int io_threads;
    int mixed_threads;
    int enable_nice;
    int verbose;
} config_t;

static volatile int g_stop_all = 0;
static thread_info_t threads[MAX_THREADS];
static config_t config;

// Signal handler for clean shutdown
void signal_handler(int sig) {
    g_stop_all = 1;
}

// Get CPU time used by current thread
double get_thread_cpu_time() {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == -1) {
        return 0.0;
    }
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// CPU-intensive workload
void cpu_intensive_work() {
    double x = 1.1;
    for (int i = 0; i < ITERATIONS_PER_YIELD; i++) {
        x = sin(x) * cos(x) + tan(x);
        x = sqrt(fabs(x)) + x * x;
    }
}

// IO-intensive workload
void io_intensive_work(int read_fd, int write_fd) {
    char buffer[IO_BUFFER_SIZE];
    memset(buffer, 'X', sizeof(buffer));
    
    // Write then read to simulate IO
    if (write(write_fd, buffer, sizeof(buffer)) < 0) {
        if (errno != EPIPE) perror("write");
    }
    
    // Non-blocking read
    ssize_t bytes = read(read_fd, buffer, sizeof(buffer));
    if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("read");
    }
    
    // Small sleep to simulate waiting for IO
    usleep(1000); // 1ms
}

// Thread worker function
void* thread_worker(void* arg) {
    thread_info_t* info = (thread_info_t*)arg;
    
    // Set nice value if requested
    if (config.enable_nice && info->nice_value != 0) {
        if (setpriority(PRIO_PROCESS, 0, info->nice_value) == -1) {
            perror("setpriority");
        }
    }
    
    // Record start time
    clock_gettime(CLOCK_MONOTONIC, &info->start_time);
    double start_cpu_time = get_thread_cpu_time();
    
    // Main work loop
    while (!g_stop_all && !info->should_stop) {
        switch (info->type) {
            case WORKLOAD_CPU:
                cpu_intensive_work();
                break;
                
            case WORKLOAD_IO:
                io_intensive_work(info->pipe_read_fd, info->pipe_write_fd);
                break;
                
            case WORKLOAD_MIXED:
                if (info->iterations % 3 == 0) {
                    io_intensive_work(info->pipe_read_fd, info->pipe_write_fd);
                } else {
                    cpu_intensive_work();
                }
                break;
        }
        
        info->iterations++;
        
        // Yield occasionally to allow scheduler to work
        if (info->iterations % 100 == 0) {
            sched_yield();
        }
    }
    
    // Record end time
    clock_gettime(CLOCK_MONOTONIC, &info->end_time);
    info->cpu_time_used = get_thread_cpu_time() - start_cpu_time;
    
    return NULL;
}

// Initialize thread
int init_thread(thread_info_t* thread, int id, workload_type_t type, int nice) {
    memset(thread, 0, sizeof(thread_info_t));
    thread->thread_id = id;
    thread->type = type;
    thread->nice_value = nice;
    
    // Create pipes for IO operations
    if (type == WORKLOAD_IO || type == WORKLOAD_MIXED) {
        int pipefd[2];
        if (pipe(pipefd) == -1) {
            perror("pipe");
            return -1;
        }
        thread->pipe_read_fd = pipefd[0];
        thread->pipe_write_fd = pipefd[1];
        
        // Make read end non-blocking
        int flags = fcntl(thread->pipe_read_fd, F_GETFL, 0);
        fcntl(thread->pipe_read_fd, F_SETFL, flags | O_NONBLOCK);
    }
    
    return 0;
}

// Cleanup thread resources
void cleanup_thread(thread_info_t* thread) {
    if (thread->pipe_read_fd > 0) close(thread->pipe_read_fd);
    if (thread->pipe_write_fd > 0) close(thread->pipe_write_fd);
}

// Calculate statistics
void calculate_stats(double* values, int count, double* mean, double* stddev, double* min, double* max) {
    *mean = 0;
    *min = values[0];
    *max = values[0];
    
    for (int i = 0; i < count; i++) {
        *mean += values[i];
        if (values[i] < *min) *min = values[i];
        if (values[i] > *max) *max = values[i];
    }
    *mean /= count;
    
    *stddev = 0;
    for (int i = 0; i < count; i++) {
        double diff = values[i] - *mean;
        *stddev += diff * diff;
    }
    *stddev = sqrt(*stddev / count);
}

// Print results
void print_results() {
    printf("\n=== Scheduler Fairness Analysis Results ===\n\n");
    
    double total_cpu_time = 0;
    double cpu_times[MAX_THREADS];
    int cpu_count = 0, io_count = 0, mixed_count = 0;
    
    // Collect data by workload type
    double cpu_workload_times[MAX_THREADS], io_workload_times[MAX_THREADS], mixed_workload_times[MAX_THREADS];
    
    for (int i = 0; i < config.num_threads; i++) {
        if (threads[i].cpu_time_used > 0) {
            total_cpu_time += threads[i].cpu_time_used;
            cpu_times[i] = threads[i].cpu_time_used;
            
            switch (threads[i].type) {
                case WORKLOAD_CPU:
                    cpu_workload_times[cpu_count++] = threads[i].cpu_time_used;
                    break;
                case WORKLOAD_IO:
                    io_workload_times[io_count++] = threads[i].cpu_time_used;
                    break;
                case WORKLOAD_MIXED:
                    mixed_workload_times[mixed_count++] = threads[i].cpu_time_used;
                    break;
            }
        }
    }
    
    // Per-thread results
    printf("Thread ID | Type  | Nice | Iterations | CPU Time | CPU %% | Wall Time\n");
    printf("----------|-------|------|------------|----------|-------|----------\n");
    
    for (int i = 0; i < config.num_threads; i++) {
        if (threads[i].cpu_time_used > 0) {
            double wall_time = (threads[i].end_time.tv_sec - threads[i].start_time.tv_sec) +
                              (threads[i].end_time.tv_nsec - threads[i].start_time.tv_nsec) / 1e9;
            double cpu_percent = (threads[i].cpu_time_used / total_cpu_time) * 100;
            
            const char* type_str = threads[i].type == WORKLOAD_CPU ? "CPU" : 
                                  threads[i].type == WORKLOAD_IO ? "IO" : "MIXED";
            
            printf("%9d | %5s | %4d | %10lu | %8.3f | %5.1f | %9.3f\n",
                   threads[i].thread_id, type_str, threads[i].nice_value,
                   threads[i].iterations, threads[i].cpu_time_used, 
                   cpu_percent, wall_time);
        }
    }
    
    // Summary statistics by workload type
    printf("\n=== Workload Type Statistics ===\n\n");
    
    if (cpu_count > 0) {
        double mean, stddev, min, max;
        calculate_stats(cpu_workload_times, cpu_count, &mean, &stddev, &min, &max);
        printf("CPU-intensive threads (%d threads):\n", cpu_count);
        printf("  Mean CPU time: %.3f sec (±%.3f)\n", mean, stddev);
        printf("  Min/Max: %.3f / %.3f sec\n", min, max);
        printf("  Total CPU %%: %.1f%%\n\n", (mean * cpu_count / total_cpu_time) * 100);
    }
    
    if (io_count > 0) {
        double mean, stddev, min, max;
        calculate_stats(io_workload_times, io_count, &mean, &stddev, &min, &max);
        printf("IO-intensive threads (%d threads):\n", io_count);
        printf("  Mean CPU time: %.3f sec (±%.3f)\n", mean, stddev);
        printf("  Min/Max: %.3f / %.3f sec\n", min, max);
        printf("  Total CPU %%: %.1f%%\n\n", (mean * io_count / total_cpu_time) * 100);
    }
    
    if (mixed_count > 0) {
        double mean, stddev, min, max;
        calculate_stats(mixed_workload_times, mixed_count, &mean, &stddev, &min, &max);
        printf("Mixed workload threads (%d threads):\n", mixed_count);
        printf("  Mean CPU time: %.3f sec (±%.3f)\n", mean, stddev);
        printf("  Min/Max: %.3f / %.3f sec\n", min, max);
        printf("  Total CPU %%: %.1f%%\n\n", (mean * mixed_count / total_cpu_time) * 100);
    }
    
    // Fairness index (Jain's fairness index)
    double sum_squares = 0, sum = 0;
    for (int i = 0; i < config.num_threads; i++) {
        if (threads[i].cpu_time_used > 0) {
            sum += threads[i].cpu_time_used;
            sum_squares += threads[i].cpu_time_used * threads[i].cpu_time_used;
        }
    }
    double fairness = (sum * sum) / (config.num_threads * sum_squares);
    
    printf("=== Fairness Metrics ===\n");
    printf("Jain's Fairness Index: %.3f (1.0 = perfect fairness)\n", fairness);
    printf("Total CPU time used: %.3f seconds\n", total_cpu_time);
    printf("CPU utilization: %.1f%%\n", (total_cpu_time / (config.duration_seconds * sysconf(_SC_NPROCESSORS_ONLN))) * 100);
}

// Print usage
void print_usage(char* prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -d <seconds>    Duration of test (default: %d)\n", DEFAULT_DURATION);
    printf("  -c <count>      Number of CPU-intensive threads\n");
    printf("  -i <count>      Number of IO-intensive threads\n");
    printf("  -m <count>      Number of mixed workload threads\n");
    printf("  -n              Enable nice values (CPU: -5, IO: 5, Mixed: 0)\n");
    printf("  -v              Verbose output\n");
    printf("  -h              Show this help\n");
    printf("\nExample: %s -d 30 -c 4 -i 4 -m 2 -n\n", prog_name);
}

int main(int argc, char* argv[]) {
    // Set defaults
    config.duration_seconds = DEFAULT_DURATION;
    config.cpu_threads = 2;
    config.io_threads = 2;
    config.mixed_threads = 1;
    config.enable_nice = 0;
    config.verbose = 0;
    
    // Parse arguments
    int opt;
    while ((opt = getopt(argc, argv, "d:c:i:m:nvh")) != -1) {
        switch (opt) {
            case 'd':
                config.duration_seconds = atoi(optarg);
                break;
            case 'c':
                config.cpu_threads = atoi(optarg);
                break;
            case 'i':
                config.io_threads = atoi(optarg);
                break;
            case 'm':
                config.mixed_threads = atoi(optarg);
                break;
            case 'n':
                config.enable_nice = 1;
                break;
            case 'v':
                config.verbose = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    config.num_threads = config.cpu_threads + config.io_threads + config.mixed_threads;
    if (config.num_threads > MAX_THREADS) {
        fprintf(stderr, "Error: Too many threads (max %d)\n", MAX_THREADS);
        return 1;
    }
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("Starting Scheduler Fairness Analyzer\n");
    printf("Duration: %d seconds\n", config.duration_seconds);
    printf("Threads: %d CPU, %d IO, %d Mixed\n", config.cpu_threads, config.io_threads, config.mixed_threads);
    if (config.enable_nice) {
        printf("Nice values enabled (CPU: -5, IO: 5, Mixed: 0)\n");
    }
    printf("\n");
    
    // Initialize threads
    int thread_idx = 0;
    
    // CPU-intensive threads
    for (int i = 0; i < config.cpu_threads; i++) {
        init_thread(&threads[thread_idx], thread_idx, WORKLOAD_CPU, config.enable_nice ? -5 : 0);
        thread_idx++;
    }
    
    // IO-intensive threads
    for (int i = 0; i < config.io_threads; i++) {
        init_thread(&threads[thread_idx], thread_idx, WORKLOAD_IO, config.enable_nice ? 5 : 0);
        thread_idx++;
    }
    
    // Mixed workload threads
    for (int i = 0; i < config.mixed_threads; i++) {
        init_thread(&threads[thread_idx], thread_idx, WORKLOAD_MIXED, 0);
        thread_idx++;
    }
    
    // Start all threads
    for (int i = 0; i < config.num_threads; i++) {
        if (pthread_create(&threads[i].pthread_id, NULL, thread_worker, &threads[i]) != 0) {
            perror("pthread_create");
            g_stop_all = 1;
            break;
        }
    }
    
    // Run for specified duration
    if (!g_stop_all) {
        printf("Running test...\n");
        sleep(config.duration_seconds);
        g_stop_all = 1;
    }
    
    // Wait for all threads to complete
    printf("Stopping threads...\n");
    for (int i = 0; i < config.num_threads; i++) {
        if (threads[i].pthread_id) {
            pthread_join(threads[i].pthread_id, NULL);
        }
    }
    
    // Print results
    print_results();
    
    // Cleanup
    for (int i = 0; i < config.num_threads; i++) {
        cleanup_thread(&threads[i]);
    }
    
    return 0;
}