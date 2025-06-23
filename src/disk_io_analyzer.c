#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdint.h>
#include <math.h>

#ifdef __APPLE__
#include <sys/disk.h>
#include <sys/mount.h>

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

#else
#include <sys/statvfs.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#define KB (1024)
#define MB (1024 * 1024)
#define GB (1024 * 1024 * 1024)
#define DEFAULT_FILE_SIZE (100 * MB)
#define DEFAULT_TEST_DURATION 10
#define MAX_THREADS 64
#define MAX_FILENAME 256
#define HISTOGRAM_BUCKETS 50

typedef enum {
    IO_PATTERN_SEQUENTIAL_READ,
    IO_PATTERN_SEQUENTIAL_WRITE,
    IO_PATTERN_RANDOM_READ,
    IO_PATTERN_RANDOM_WRITE,
    IO_PATTERN_MIXED
} IOPattern;

typedef enum {
    IO_MODE_BUFFERED,
    IO_MODE_DIRECT,
    IO_MODE_SYNC
} IOMode;

typedef enum {
    TEST_PATTERN_ANALYSIS,
    TEST_BLOCK_SIZE,
    TEST_SYNC_OVERHEAD,
    TEST_IO_MODES,
    TEST_QUEUE_DEPTH,
    TEST_ALL
} TestType;

typedef struct {
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t read_operations;
    uint64_t write_operations;
    double total_time;
    double min_latency;
    double max_latency;
    double total_latency;
    double sync_time;
    int thread_id;
    IOPattern pattern;
    IOMode mode;
    size_t block_size;
    
    // Latency histogram
    uint64_t latency_histogram[HISTOGRAM_BUCKETS];
    double histogram_max_latency;
} IOStatistics;

typedef struct {
    int thread_id;
    char* filename;
    size_t file_size;
    size_t block_size;
    IOPattern pattern;
    IOMode mode;
    int duration_seconds;
    int queue_depth;
    IOStatistics* stats;
    pthread_barrier_t* barrier;
    volatile int* stop_flag;
} ThreadArgs;

static double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void add_latency_to_histogram(IOStatistics* stats, double latency) {
    if (stats->histogram_max_latency == 0.0) {
        stats->histogram_max_latency = latency * 2; // Initial estimate
    }
    
    if (latency > stats->histogram_max_latency) {
        stats->histogram_max_latency = latency * 1.5; // Expand if needed
    }
    
    int bucket = (int)((latency / stats->histogram_max_latency) * HISTOGRAM_BUCKETS);
    if (bucket >= HISTOGRAM_BUCKETS) bucket = HISTOGRAM_BUCKETS - 1;
    if (bucket < 0) bucket = 0;
    
    stats->latency_histogram[bucket]++;
}

static int create_test_file(const char* filename, size_t size) {
    int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Failed to create test file");
        return -1;
    }
    
    // Write zeros to create file of specified size
    char* buffer = calloc(1, MB);
    if (!buffer) {
        close(fd);
        return -1;
    }
    
    size_t remaining = size;
    while (remaining > 0) {
        size_t write_size = (remaining > MB) ? MB : remaining;
        ssize_t written = write(fd, buffer, write_size);
        if (written < 0) {
            perror("Failed to write to test file");
            free(buffer);
            close(fd);
            return -1;
        }
        remaining -= written;
    }
    
    free(buffer);
    close(fd);
    sync(); // Ensure data is written to disk
    return 0;
}

static void* sequential_io_test(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    IOStatistics* stats = args->stats;
    
    // Prepare filename for this thread
    char thread_filename[MAX_FILENAME];
    snprintf(thread_filename, sizeof(thread_filename), "%s.%d", 
             args->filename, args->thread_id);
    
    // Open flags based on mode
    int flags = 0;
    switch (args->mode) {
        case IO_MODE_BUFFERED:
            flags = (args->pattern == IO_PATTERN_SEQUENTIAL_READ || 
                    args->pattern == IO_PATTERN_RANDOM_READ) ? O_RDONLY : O_RDWR;
            break;
        case IO_MODE_DIRECT:
#ifdef O_DIRECT
            flags = O_DIRECT | ((args->pattern == IO_PATTERN_SEQUENTIAL_READ || 
                               args->pattern == IO_PATTERN_RANDOM_READ) ? O_RDONLY : O_RDWR);
#else
            flags = (args->pattern == IO_PATTERN_SEQUENTIAL_READ || 
                    args->pattern == IO_PATTERN_RANDOM_READ) ? O_RDONLY : O_RDWR;
#endif
            break;
        case IO_MODE_SYNC:
            flags = O_SYNC | ((args->pattern == IO_PATTERN_SEQUENTIAL_READ || 
                             args->pattern == IO_PATTERN_RANDOM_READ) ? O_RDONLY : O_RDWR);
            break;
    }
    
    int fd = open(thread_filename, flags);
    if (fd < 0) {
        perror("Failed to open test file");
        return NULL;
    }
    
    // Allocate aligned buffer for direct I/O
    void* buffer;
    if (posix_memalign(&buffer, 4096, args->block_size) != 0) {
        perror("Failed to allocate aligned buffer");
        close(fd);
        return NULL;
    }
    
    // Initialize statistics
    stats->thread_id = args->thread_id;
    stats->pattern = args->pattern;
    stats->mode = args->mode;
    stats->block_size = args->block_size;
    stats->min_latency = INFINITY;
    stats->max_latency = 0.0;
    
    // Wait for all threads to be ready
    pthread_barrier_wait(args->barrier);
    
    double start_time = get_time();
    uint64_t operations = 0;
    size_t file_offset = 0;
    
    while (!*args->stop_flag) {
        double op_start = get_time();
        ssize_t result = 0;
        
        // Perform I/O operation based on pattern
        switch (args->pattern) {
            case IO_PATTERN_SEQUENTIAL_READ:
                result = pread(fd, buffer, args->block_size, file_offset);
                if (result > 0) {
                    stats->bytes_read += result;
                    stats->read_operations++;
                }
                file_offset += args->block_size;
                if (file_offset >= args->file_size) file_offset = 0;
                break;
                
            case IO_PATTERN_SEQUENTIAL_WRITE:
                // Fill buffer with some data
                memset(buffer, (int)(operations & 0xFF), args->block_size);
                result = pwrite(fd, buffer, args->block_size, file_offset);
                if (result > 0) {
                    stats->bytes_written += result;
                    stats->write_operations++;
                }
                file_offset += args->block_size;
                if (file_offset >= args->file_size) file_offset = 0;
                break;
                
            case IO_PATTERN_RANDOM_READ:
                file_offset = (rand() % (args->file_size / args->block_size)) * args->block_size;
                result = pread(fd, buffer, args->block_size, file_offset);
                if (result > 0) {
                    stats->bytes_read += result;
                    stats->read_operations++;
                }
                break;
                
            case IO_PATTERN_RANDOM_WRITE:
                file_offset = (rand() % (args->file_size / args->block_size)) * args->block_size;
                memset(buffer, (int)(operations & 0xFF), args->block_size);
                result = pwrite(fd, buffer, args->block_size, file_offset);
                if (result > 0) {
                    stats->bytes_written += result;
                    stats->write_operations++;
                }
                break;
                
            case IO_PATTERN_MIXED:
                if (operations % 3 == 0) {
                    // Write operation
                    file_offset = (rand() % (args->file_size / args->block_size)) * args->block_size;
                    memset(buffer, (int)(operations & 0xFF), args->block_size);
                    result = pwrite(fd, buffer, args->block_size, file_offset);
                    if (result > 0) {
                        stats->bytes_written += result;
                        stats->write_operations++;
                    }
                } else {
                    // Read operation
                    file_offset = (rand() % (args->file_size / args->block_size)) * args->block_size;
                    result = pread(fd, buffer, args->block_size, file_offset);
                    if (result > 0) {
                        stats->bytes_read += result;
                        stats->read_operations++;
                    }
                }
                break;
        }
        
        double op_end = get_time();
        double latency = op_end - op_start;
        
        if (result > 0) {
            stats->total_latency += latency;
            if (latency < stats->min_latency) stats->min_latency = latency;
            if (latency > stats->max_latency) stats->max_latency = latency;
            add_latency_to_histogram(stats, latency);
            operations++;
        }
    }
    
    stats->total_time = get_time() - start_time;
    
    free(buffer);
    close(fd);
    return NULL;
}

static void* sync_overhead_test(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    IOStatistics* stats = args->stats;
    
    char thread_filename[MAX_FILENAME];
    snprintf(thread_filename, sizeof(thread_filename), "%s.%d", 
             args->filename, args->thread_id);
    
    int fd = open(thread_filename, O_RDWR);
    if (fd < 0) {
        perror("Failed to open test file for sync test");
        return NULL;
    }
    
    void* buffer;
    if (posix_memalign(&buffer, 4096, args->block_size) != 0) {
        perror("Failed to allocate aligned buffer");
        close(fd);
        return NULL;
    }
    
    stats->thread_id = args->thread_id;
    stats->pattern = IO_PATTERN_SEQUENTIAL_WRITE;
    stats->block_size = args->block_size;
    stats->min_latency = INFINITY;
    
    pthread_barrier_wait(args->barrier);
    
    double start_time = get_time();
    size_t offset = 0;
    
    while (!*args->stop_flag) {
        // Write some data
        memset(buffer, rand() & 0xFF, args->block_size);
        ssize_t written = pwrite(fd, buffer, args->block_size, offset);
        
        if (written > 0) {
            stats->bytes_written += written;
            stats->write_operations++;
            
            // Measure sync overhead
            double sync_start = get_time();
            fsync(fd);
            double sync_end = get_time();
            
            double sync_latency = sync_end - sync_start;
            stats->sync_time += sync_latency;
            stats->total_latency += sync_latency;
            
            if (sync_latency < stats->min_latency) stats->min_latency = sync_latency;
            if (sync_latency > stats->max_latency) stats->max_latency = sync_latency;
            add_latency_to_histogram(stats, sync_latency);
        }
        
        offset += args->block_size;
        if (offset >= args->file_size) offset = 0;
    }
    
    stats->total_time = get_time() - start_time;
    
    free(buffer);
    close(fd);
    return NULL;
}

static void print_io_statistics(IOStatistics* stats, int num_threads, const char* test_name) {
    printf("\n=== %s Results ===\n", test_name);
    
    uint64_t total_read_bytes = 0, total_write_bytes = 0;
    uint64_t total_read_ops = 0, total_write_ops = 0;
    double total_time = 0.0, total_latency = 0.0, total_sync_time = 0.0;
    double min_latency = INFINITY, max_latency = 0.0;
    
    for (int i = 0; i < num_threads; i++) {
        total_read_bytes += stats[i].bytes_read;
        total_write_bytes += stats[i].bytes_written;
        total_read_ops += stats[i].read_operations;
        total_write_ops += stats[i].write_operations;
        total_latency += stats[i].total_latency;
        total_sync_time += stats[i].sync_time;
        
        if (stats[i].total_time > total_time) total_time = stats[i].total_time;
        if (stats[i].min_latency < min_latency) min_latency = stats[i].min_latency;
        if (stats[i].max_latency > max_latency) max_latency = stats[i].max_latency;
    }
    
    uint64_t total_ops = total_read_ops + total_write_ops;
    uint64_t total_bytes = total_read_bytes + total_write_bytes;
    
    printf("Duration: %.2f seconds\n", total_time);
    printf("Total operations: %llu (reads: %llu, writes: %llu)\n", 
           (unsigned long long)total_ops, (unsigned long long)total_read_ops, (unsigned long long)total_write_ops);
    printf("Total data: %.2f MB (read: %.2f MB, written: %.2f MB)\n",
           total_bytes / (double)MB, total_read_bytes / (double)MB, 
           total_write_bytes / (double)MB);
    
    if (total_time > 0) {
        printf("Throughput: %.2f MB/s\n", total_bytes / (double)MB / total_time);
        printf("IOPS: %.0f (read: %.0f, write: %.0f)\n",
               total_ops / total_time, total_read_ops / total_time, 
               total_write_ops / total_time);
    }
    
    if (total_ops > 0) {
        printf("Average latency: %.3f ms\n", (total_latency / total_ops) * 1000);
        printf("Min latency: %.3f ms\n", min_latency * 1000);
        printf("Max latency: %.3f ms\n", max_latency * 1000);
    }
    
    if (total_sync_time > 0) {
        printf("Total sync time: %.2f seconds (%.1f%% of total time)\n",
               total_sync_time, (total_sync_time / total_time) * 100);
        printf("Average sync latency: %.3f ms\n", 
               (total_sync_time / total_write_ops) * 1000);
    }
    
    printf("Block size: %zu bytes\n", stats[0].block_size);
    printf("Threads: %d\n", num_threads);
}

static void pattern_analysis_test(const char* filename, size_t file_size, 
                                int num_threads, int duration) {
    printf("\n=== I/O Pattern Analysis ===\n");
    
    IOPattern patterns[] = {
        IO_PATTERN_SEQUENTIAL_READ,
        IO_PATTERN_SEQUENTIAL_WRITE,
        IO_PATTERN_RANDOM_READ,
        IO_PATTERN_RANDOM_WRITE,
        IO_PATTERN_MIXED
    };
    
    const char* pattern_names[] = {
        "Sequential Read",
        "Sequential Write", 
        "Random Read",
        "Random Write",
        "Mixed (33% write, 67% read)"
    };
    
    size_t block_size = 64 * KB;
    
    for (int p = 0; p < 5; p++) {
        IOStatistics* stats = calloc(num_threads, sizeof(IOStatistics));
        ThreadArgs* args = calloc(num_threads, sizeof(ThreadArgs));
        pthread_t* threads = calloc(num_threads, sizeof(pthread_t));
        pthread_barrier_t barrier;
        volatile int stop_flag = 0;
        
        pthread_barrier_init(&barrier, NULL, num_threads);
        
        // Create test files for each thread
        for (int i = 0; i < num_threads; i++) {
            char thread_filename[MAX_FILENAME];
            snprintf(thread_filename, sizeof(thread_filename), "%s.%d", filename, i);
            
            if (create_test_file(thread_filename, file_size) < 0) {
                fprintf(stderr, "Failed to create test file %s\n", thread_filename);
                goto cleanup;
            }
            
            args[i].thread_id = i;
            args[i].filename = (char*)filename;
            args[i].file_size = file_size;
            args[i].block_size = block_size;
            args[i].pattern = patterns[p];
            args[i].mode = IO_MODE_BUFFERED;
            args[i].duration_seconds = duration;
            args[i].stats = &stats[i];
            args[i].barrier = &barrier;
            args[i].stop_flag = &stop_flag;
            
            pthread_create(&threads[i], NULL, sequential_io_test, &args[i]);
        }
        
        sleep(duration);
        stop_flag = 1;
        
        for (int i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
        }
        
        print_io_statistics(stats, num_threads, pattern_names[p]);
        
cleanup:
        // Cleanup test files
        for (int i = 0; i < num_threads; i++) {
            char thread_filename[MAX_FILENAME];
            snprintf(thread_filename, sizeof(thread_filename), "%s.%d", filename, i);
            unlink(thread_filename);
        }
        
        pthread_barrier_destroy(&barrier);
        free(stats);
        free(args);
        free(threads);
    }
}

static void block_size_analysis_test(const char* filename, size_t file_size, 
                                   int num_threads, int duration) {
    printf("\n=== Block Size Analysis ===\n");
    
    size_t block_sizes[] = {4*KB, 8*KB, 16*KB, 32*KB, 64*KB, 128*KB, 256*KB, 512*KB, 1*MB};
    int num_sizes = sizeof(block_sizes) / sizeof(block_sizes[0]);
    
    printf("%-12s %-15s %-15s %-15s %-15s\n", 
           "Block Size", "Read MB/s", "Write MB/s", "Read IOPS", "Write IOPS");
    printf("------------ --------------- --------------- --------------- ---------------\n");
    
    for (int s = 0; s < num_sizes; s++) {
        IOStatistics* read_stats = calloc(num_threads, sizeof(IOStatistics));
        IOStatistics* write_stats = calloc(num_threads, sizeof(IOStatistics));
        
        // Test sequential read
        {
            ThreadArgs* args = calloc(num_threads, sizeof(ThreadArgs));
            pthread_t* threads = calloc(num_threads, sizeof(pthread_t));
            pthread_barrier_t barrier;
            volatile int stop_flag = 0;
            
            pthread_barrier_init(&barrier, NULL, num_threads);
            
            for (int i = 0; i < num_threads; i++) {
                char thread_filename[MAX_FILENAME];
                snprintf(thread_filename, sizeof(thread_filename), "%s.%d", filename, i);
                create_test_file(thread_filename, file_size);
                
                args[i].thread_id = i;
                args[i].filename = (char*)filename;
                args[i].file_size = file_size;
                args[i].block_size = block_sizes[s];
                args[i].pattern = IO_PATTERN_SEQUENTIAL_READ;
                args[i].mode = IO_MODE_BUFFERED;
                args[i].stats = &read_stats[i];
                args[i].barrier = &barrier;
                args[i].stop_flag = &stop_flag;
                
                pthread_create(&threads[i], NULL, sequential_io_test, &args[i]);
            }
            
            sleep(duration / 2);
            stop_flag = 1;
            
            for (int i = 0; i < num_threads; i++) {
                pthread_join(threads[i], NULL);
            }
            
            pthread_barrier_destroy(&barrier);
            free(args);
            free(threads);
        }
        
        // Test sequential write
        {
            ThreadArgs* args = calloc(num_threads, sizeof(ThreadArgs));
            pthread_t* threads = calloc(num_threads, sizeof(pthread_t));
            pthread_barrier_t barrier;
            volatile int stop_flag = 0;
            
            pthread_barrier_init(&barrier, NULL, num_threads);
            
            for (int i = 0; i < num_threads; i++) {
                args[i].thread_id = i;
                args[i].filename = (char*)filename;
                args[i].file_size = file_size;
                args[i].block_size = block_sizes[s];
                args[i].pattern = IO_PATTERN_SEQUENTIAL_WRITE;
                args[i].mode = IO_MODE_BUFFERED;
                args[i].stats = &write_stats[i];
                args[i].barrier = &barrier;
                args[i].stop_flag = &stop_flag;
                
                pthread_create(&threads[i], NULL, sequential_io_test, &args[i]);
            }
            
            sleep(duration / 2);
            stop_flag = 1;
            
            for (int i = 0; i < num_threads; i++) {
                pthread_join(threads[i], NULL);
            }
            
            pthread_barrier_destroy(&barrier);
            free(args);
            free(threads);
        }
        
        // Calculate and print results
        uint64_t total_read_bytes = 0, total_write_bytes = 0;
        uint64_t total_read_ops = 0, total_write_ops = 0;
        double read_time = 0, write_time = 0;
        
        for (int i = 0; i < num_threads; i++) {
            total_read_bytes += read_stats[i].bytes_read;
            total_write_bytes += write_stats[i].bytes_written;
            total_read_ops += read_stats[i].read_operations;
            total_write_ops += write_stats[i].write_operations;
            if (read_stats[i].total_time > read_time) read_time = read_stats[i].total_time;
            if (write_stats[i].total_time > write_time) write_time = write_stats[i].total_time;
        }
        
        double read_mbps = read_time > 0 ? (total_read_bytes / (double)MB) / read_time : 0;
        double write_mbps = write_time > 0 ? (total_write_bytes / (double)MB) / write_time : 0;
        double read_iops = read_time > 0 ? total_read_ops / read_time : 0;
        double write_iops = write_time > 0 ? total_write_ops / write_time : 0;
        
        printf("%-12zu %-15.1f %-15.1f %-15.0f %-15.0f\n",
               block_sizes[s], read_mbps, write_mbps, read_iops, write_iops);
        
        // Cleanup
        for (int i = 0; i < num_threads; i++) {
            char thread_filename[MAX_FILENAME];
            snprintf(thread_filename, sizeof(thread_filename), "%s.%d", filename, i);
            unlink(thread_filename);
        }
        
        free(read_stats);
        free(write_stats);
    }
}

static void sync_overhead_analysis_test(const char* filename, size_t file_size, 
                                      int num_threads, int duration) {
    printf("\n=== Sync Overhead Analysis ===\n");
    
    IOStatistics* stats = calloc(num_threads, sizeof(IOStatistics));
    ThreadArgs* args = calloc(num_threads, sizeof(ThreadArgs));
    pthread_t* threads = calloc(num_threads, sizeof(pthread_t));
    pthread_barrier_t barrier;
    volatile int stop_flag = 0;
    
    pthread_barrier_init(&barrier, NULL, num_threads);
    
    for (int i = 0; i < num_threads; i++) {
        char thread_filename[MAX_FILENAME];
        snprintf(thread_filename, sizeof(thread_filename), "%s.%d", filename, i);
        create_test_file(thread_filename, file_size);
        
        args[i].thread_id = i;
        args[i].filename = (char*)filename;
        args[i].file_size = file_size;
        args[i].block_size = 64 * KB;
        args[i].stats = &stats[i];
        args[i].barrier = &barrier;
        args[i].stop_flag = &stop_flag;
        
        pthread_create(&threads[i], NULL, sync_overhead_test, &args[i]);
    }
    
    sleep(duration);
    stop_flag = 1;
    
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    print_io_statistics(stats, num_threads, "fsync() Overhead");
    
    // Cleanup
    for (int i = 0; i < num_threads; i++) {
        char thread_filename[MAX_FILENAME];
        snprintf(thread_filename, sizeof(thread_filename), "%s.%d", filename, i);
        unlink(thread_filename);
    }
    
    pthread_barrier_destroy(&barrier);
    free(stats);
    free(args);
    free(threads);
}

static void io_modes_comparison_test(const char* filename, size_t file_size, 
                                   int num_threads, int duration) {
    printf("\n=== I/O Modes Comparison ===\n");
    
    IOMode modes[] = {IO_MODE_BUFFERED, IO_MODE_DIRECT, IO_MODE_SYNC};
    const char* mode_names[] = {"Buffered I/O", "Direct I/O", "Synchronous I/O"};
    
    printf("%-20s %-15s %-15s %-15s\n", "Mode", "Read MB/s", "Write MB/s", "Avg Latency (ms)");
    printf("-------------------- --------------- --------------- ---------------\n");
    
    for (int m = 0; m < 3; m++) {
        IOStatistics* stats = calloc(num_threads, sizeof(IOStatistics));
        ThreadArgs* args = calloc(num_threads, sizeof(ThreadArgs));
        pthread_t* threads = calloc(num_threads, sizeof(pthread_t));
        pthread_barrier_t barrier;
        volatile int stop_flag = 0;
        
        pthread_barrier_init(&barrier, NULL, num_threads);
        
        for (int i = 0; i < num_threads; i++) {
            char thread_filename[MAX_FILENAME];
            snprintf(thread_filename, sizeof(thread_filename), "%s.%d", filename, i);
            create_test_file(thread_filename, file_size);
            
            args[i].thread_id = i;
            args[i].filename = (char*)filename;
            args[i].file_size = file_size;
            args[i].block_size = 64 * KB;
            args[i].pattern = IO_PATTERN_MIXED;
            args[i].mode = modes[m];
            args[i].stats = &stats[i];
            args[i].barrier = &barrier;
            args[i].stop_flag = &stop_flag;
            
            pthread_create(&threads[i], NULL, sequential_io_test, &args[i]);
        }
        
        sleep(duration);
        stop_flag = 1;
        
        for (int i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
        }
        
        // Calculate results
        uint64_t total_read_bytes = 0, total_write_bytes = 0, total_ops = 0;
        double total_time = 0, total_latency = 0;
        
        for (int i = 0; i < num_threads; i++) {
            total_read_bytes += stats[i].bytes_read;
            total_write_bytes += stats[i].bytes_written;
            total_ops += stats[i].read_operations + stats[i].write_operations;
            total_latency += stats[i].total_latency;
            if (stats[i].total_time > total_time) total_time = stats[i].total_time;
        }
        
        double read_mbps = total_time > 0 ? (total_read_bytes / (double)MB) / total_time : 0;
        double write_mbps = total_time > 0 ? (total_write_bytes / (double)MB) / total_time : 0;
        double avg_latency = total_ops > 0 ? (total_latency / total_ops) * 1000 : 0;
        
        printf("%-20s %-15.1f %-15.1f %-15.3f\n",
               mode_names[m], read_mbps, write_mbps, avg_latency);
        
        // Cleanup
        for (int i = 0; i < num_threads; i++) {
            char thread_filename[MAX_FILENAME];
            snprintf(thread_filename, sizeof(thread_filename), "%s.%d", filename, i);
            unlink(thread_filename);
        }
        
        pthread_barrier_destroy(&barrier);
        free(stats);
        free(args);
        free(threads);
    }
}

static void print_usage(const char* prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -f <filename>  Test file path (default: ./diskio_test)\n");
    printf("  -s <size>      File size in MB (default: 100)\n");
    printf("  -t <type>      Test type: pattern, block, sync, modes, all (default: all)\n");
    printf("  -T <threads>   Number of threads (default: 4)\n");
    printf("  -d <duration>  Test duration in seconds (default: 10)\n");
    printf("  -h             Show this help message\n");
    printf("\nAnalyzes disk I/O performance characteristics:\n");
    printf("  - Sequential vs random access patterns\n");
    printf("  - Block size optimization\n");
    printf("  - fsync/fdatasync overhead\n");
    printf("  - Buffered vs direct vs synchronous I/O\n");
    printf("  - Multi-threaded I/O scaling\n");
}

int main(int argc, char* argv[]) {
    const char* filename = "./diskio_test";
    size_t file_size_mb = 100;
    TestType test_type = TEST_ALL;
    int num_threads = 4;
    int duration = DEFAULT_TEST_DURATION;
    
    int opt;
    while ((opt = getopt(argc, argv, "f:s:t:T:d:h")) != -1) {
        switch (opt) {
            case 'f':
                filename = optarg;
                break;
            case 's':
                file_size_mb = atoi(optarg);
                if (file_size_mb <= 0) {
                    fprintf(stderr, "Invalid file size: %s\n", optarg);
                    return 1;
                }
                break;
            case 't':
                if (strcmp(optarg, "pattern") == 0) test_type = TEST_PATTERN_ANALYSIS;
                else if (strcmp(optarg, "block") == 0) test_type = TEST_BLOCK_SIZE;
                else if (strcmp(optarg, "sync") == 0) test_type = TEST_SYNC_OVERHEAD;
                else if (strcmp(optarg, "modes") == 0) test_type = TEST_IO_MODES;
                else if (strcmp(optarg, "all") == 0) test_type = TEST_ALL;
                else {
                    fprintf(stderr, "Unknown test type: %s\n", optarg);
                    return 1;
                }
                break;
            case 'T':
                num_threads = atoi(optarg);
                if (num_threads <= 0 || num_threads > MAX_THREADS) {
                    fprintf(stderr, "Invalid number of threads: %s\n", optarg);
                    return 1;
                }
                break;
            case 'd':
                duration = atoi(optarg);
                if (duration <= 0) {
                    fprintf(stderr, "Invalid duration: %s\n", optarg);
                    return 1;
                }
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }
    
    size_t file_size = file_size_mb * MB;
    
    printf("Disk I/O Performance Analyzer\n");
    printf("=============================\n");
    printf("Test file: %s\n", filename);
    printf("File size: %zu MB\n", file_size_mb);
    printf("Threads: %d\n", num_threads);
    printf("Duration: %d seconds\n", duration);
    printf("Test type: ");
    
    switch (test_type) {
        case TEST_PATTERN_ANALYSIS: printf("Pattern Analysis\n"); break;
        case TEST_BLOCK_SIZE: printf("Block Size Analysis\n"); break;
        case TEST_SYNC_OVERHEAD: printf("Sync Overhead Analysis\n"); break;
        case TEST_IO_MODES: printf("I/O Modes Comparison\n"); break;
        case TEST_ALL: printf("All Tests\n"); break;
        default: printf("Unknown\n"); break;
    }
    
    // Run selected tests
    switch (test_type) {
        case TEST_PATTERN_ANALYSIS:
            pattern_analysis_test(filename, file_size, num_threads, duration);
            break;
        case TEST_BLOCK_SIZE:
            block_size_analysis_test(filename, file_size, num_threads, duration);
            break;
        case TEST_SYNC_OVERHEAD:
            sync_overhead_analysis_test(filename, file_size, num_threads, duration);
            break;
        case TEST_IO_MODES:
            io_modes_comparison_test(filename, file_size, num_threads, duration);
            break;
        case TEST_ALL:
            pattern_analysis_test(filename, file_size, num_threads, duration);
            block_size_analysis_test(filename, file_size, num_threads, duration);
            sync_overhead_analysis_test(filename, file_size, num_threads, duration);
            io_modes_comparison_test(filename, file_size, num_threads, duration);
            break;
    }
    
    return 0;
}