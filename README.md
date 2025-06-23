# Operating System Performance Analysis Tools

A comprehensive suite of tools for analyzing and understanding operating system performance characteristics including context switching, scheduling, lock contention, virtual memory behavior, memory allocation performance, CPU cache behavior, disk I/O performance, and network I/O performance.

## Overview

This repository contains sixteen specialized tools for exploring different aspects of operating system performance:

1. **Context Switch Measurement (C)** - Measures the overhead of process context switching
2. **Context Switch Measurement (C++)** - Enhanced version with multiple measurement methods
3. **Scheduler Fairness Analyzer (C)** - Tests how the OS scheduler distributes CPU time
4. **Scheduler Fairness Analyzer (C++)** - Advanced scheduler analysis with templates and STL
5. **Lock Contention Visualizer (C)** - Analyzes synchronization primitive performance
6. **Lock Contention Visualizer (C++)** - Modern lock analysis with custom lock types and real-time visualization
7. **Virtual Memory Explorer (C)** - Deep dive into VM subsystem behavior
8. **Virtual Memory Explorer (C++)** - Modern C++ implementation with enhanced features
9. **Memory Allocator Benchmarker (C)** - Comprehensive malloc/free performance analysis
10. **Memory Allocator Benchmarker (C++)** - Advanced allocator testing with STL and custom allocators
11. **CPU Cache Performance Analyzer (C)** - Comprehensive CPU cache hierarchy and performance analysis
12. **CPU Cache Performance Analyzer (C++)** - Template-based cache analysis with modern C++ features
13. **Disk I/O Performance Analyzer (C)** - Comprehensive disk I/O performance analysis and optimization
14. **Disk I/O Performance Analyzer (C++)** - Template-based I/O analysis with modern C++ async features
15. **Network I/O Benchmarker (C)** - Comprehensive network performance analysis and protocol comparison
16. **Network I/O Benchmarker (C++)** - Modern network I/O testing with async patterns and advanced statistics

## Building All Tools

```bash
# Build all tools at once
make all

# Or build individually:
gcc -o context_switch "src/Cost of Context Switching.c" -lrt
gcc -o context_switch_macos src/context_switch_macos.c         # macOS version
gcc -o scheduler_analyzer src/scheduler_analyzer.c -lpthread -lm
gcc -o lock_visualizer src/lock_contention_visualizer.c -lpthread -lm
gcc -o vm_explorer src/vm_explorer.c -lm
g++ -std=c++17 -O2 -o vm_explorer_cpp src/vm_explorer.cpp -pthread
gcc -o memory_allocator src/memory_allocator_benchmarker.c -lpthread -lm
g++ -std=c++17 -O2 -o memory_allocator_cpp src/memory_allocator_benchmarker.cpp -pthread
g++ -std=c++17 -O2 -o context_switch_cpp src/context_switch_cpp.cpp -pthread
g++ -std=c++17 -O2 -o scheduler_analyzer_cpp src/scheduler_analyzer_cpp.cpp -pthread  
g++ -std=c++17 -O2 -o lock_visualizer_cpp src/lock_visualizer_cpp.cpp -pthread
gcc -o cache_analyzer src/cache_analyzer.c -lpthread -lm
g++ -std=c++17 -O2 -o cache_analyzer_cpp src/cache_analyzer_cpp.cpp -pthread
gcc -o disk_io_analyzer src/disk_io_analyzer.c -lpthread -lm
g++ -std=c++17 -O2 -o disk_io_analyzer_cpp src/disk_io_analyzer_cpp.cpp -pthread
gcc -o network_io_benchmarker src/network_io_benchmarker.c -lpthread -lm
g++ -std=c++17 -O2 -o network_io_benchmarker_cpp src/network_io_benchmarker_cpp.cpp -pthread
```

## Tool Descriptions

### 1. Context Switch Measurement

Measures the cost of context switching between processes using pipe-based IPC.

**Usage:**
```bash
# Linux with CPU affinity
./context_switch <parent-cpu> <child-cpu>

# macOS version
./context_switch_macos
```

**Key Features:**
- Measures real-world context switch overhead
- Supports CPU affinity on Linux
- Uses high-resolution timers
- Performs 1000 iterations for accuracy

**Example Output:**
```
Average context switching time: 3.3 microseconds
```

### 2. Scheduler Fairness Analyzer

Tests scheduler behavior under different workloads and thread priorities.

**Usage:**
```bash
./scheduler_analyzer [options]
  -t <threads>    Number of threads (default: 8)
  -d <seconds>    Duration of test (default: 10)
  -c <count>      Number of CPU-intensive threads
  -i <count>      Number of IO-intensive threads
  -m <count>      Number of mixed workload threads
  -n              Enable nice values
```

**Example:**
```bash
# Test with different workload types and priorities
./scheduler_analyzer -d 30 -c 4 -i 4 -m 2 -n
```

**Key Metrics:**
- CPU time distribution per thread
- Contention rates
- Jain's Fairness Index
- Read/write operation counts

### 3. Lock Contention Visualizer

Analyzes lock performance and contention patterns for different synchronization primitives.

**Usage:**
```bash
./lock_visualizer [options]
  -t <threads>    Number of threads (default: 8)
  -l <type>       Lock type: mutex, spin, rwlock, adaptive
  -w <pattern>    Workload: balanced, read-heavy, write-heavy, thunder, random
  -z              Enable real-time visualization
```

**Example:**
```bash
# Thundering herd test with real-time visualization
./lock_visualizer -t 16 -l spin -w thunder -z
```

**Key Features:**
- Multiple lock types (mutex, spinlock, rwlock)
- Various workload patterns
- Real-time contention monitoring
- Wait time distribution histograms
- Performance recommendations

### 4. Virtual Memory Explorer

Comprehensive analysis of virtual memory subsystem performance.

**Usage:**
```bash
./vm_explorer [options]
  -s <size>    Memory size in MB (default: 256)
  -t <test>    Test type: tlb, fault, huge, mmap, cow, all
  -p <pattern> Access pattern: seq, rand, stride, chase
```

**Test Types:**
- **TLB Miss Analysis** - Impact of Translation Lookaside Buffer misses
- **Page Fault Measurement** - Cost of demand paging
- **Huge Pages Testing** - 2MB vs 4KB page performance
- **mmap vs malloc** - Memory allocation strategies
- **Copy-on-Write** - Fork efficiency analysis

**Example:**
```bash
# Run all VM tests
./vm_explorer -t all

# Specific TLB test with large memory
./vm_explorer -t tlb -s 512
```

### 5. Virtual Memory Explorer (C++ Version)

Modern C++ implementation with enhanced safety and features.

**Additional Features:**
- RAII memory management
- Exception handling
- Template-based access patterns
- STL containers and algorithms
- Type-safe operations

**Usage:** Same as C version
```bash
./vm_explorer_cpp -t all -s 256
```

### 6. Memory Allocator Benchmarker (C)

Comprehensive analysis of dynamic memory allocation performance.

**Usage:**
```bash
./memory_allocator [options]
  -t <type>     Test type: speed, frag, scale, all
  -p <pattern>  Allocation pattern: seq, rand, exp, bimodal, real
  -s <size>     Min allocation size (default: 8)
  -S <size>     Max allocation size (default: 8192)
  -n <count>    Number of iterations (default: 1000)
```

**Test Types:**
- **Speed Test** - Measures malloc/free performance
- **Fragmentation Analysis** - Tracks memory fragmentation over time
- **Scalability Test** - Multi-threaded allocation performance
- **Pattern Analysis** - Different allocation size distributions

**Key Features:**
- Real-time performance statistics
- Memory fragmentation scoring
- Thread scalability analysis
- Allocation/free time histograms
- Peak memory tracking

**Example:**
```bash
# Run all tests with realistic pattern
./memory_allocator -t all

# Test fragmentation with specific sizes
./memory_allocator -t frag -s 64 -S 4096

# Scalability test with random pattern
./memory_allocator -t scale -p rand
```

### 7. Memory Allocator Benchmarker (C++ Version)

Advanced C++ implementation with additional allocator testing capabilities.

**Usage:**
```bash
./memory_allocator_cpp [options]
  -t <type>     Test type: speed, frag, scale, container, custom, all
  -p <pattern>  Allocation pattern: seq, rand, exp, bimodal, real
  -s <size>     Min allocation size (default: 8)
  -S <size>     Max allocation size (default: 8192)
  -n <count>    Number of iterations (default: 1000)
```

**Additional Features:**
- **STL Container Tests** - Benchmarks vector, map, unordered_map allocations
- **Custom Allocator Tests** - Compares standard, aligned, and PMR allocators
- **Smart Pointer Usage** - RAII-based memory management
- **Percentile Statistics** - 50th, 90th, 99th percentile measurements

**Example:**
```bash
# Test STL container allocations
./memory_allocator_cpp -t container

# Compare custom allocators
./memory_allocator_cpp -t custom

# Full benchmark suite
./memory_allocator_cpp -t all
```

### 8. Context Switch Measurement (C++ Version)

Enhanced C++ implementation with multiple measurement methods and advanced statistics.

**Usage:**
```bash
./context_switch_cpp [options]
  -n <count>    Number of iterations (default: 1000)
  -v            Verbose output with detailed statistics
  -h            Show this help message
```

**Measurement Methods:**
- **Pipe-based IPC** - Traditional process context switch measurement
- **Shared Memory** - Low-overhead synchronization method
- **Thread Switching** - Measures thread context switch overhead

**Enhanced Features:**
- RAII-based resource management
- Statistical analysis with percentiles
- Multiple measurement methods comparison
- Histogram visualization of timing distribution

**Example:**
```bash
# Quick measurement with default settings
./context_switch_cpp

# Detailed analysis with verbose output
./context_switch_cpp -n 5000 -v
```

### 9. Scheduler Fairness Analyzer (C++ Version)

Advanced scheduler analysis using modern C++ features and comprehensive workload types.

**Usage:**
```bash
./scheduler_analyzer_cpp [options]
  -d <seconds>  Duration of test (default: 10)
  -t <threads>  Number of threads (default: 4)
  -n            Enable nice values for priority testing
  -v            Verbose output with detailed statistics
  -h            Show this help message
```

**Workload Types:**
- **CPU-intensive** - Mathematical computations
- **I/O-intensive** - Sleep-based I/O simulation
- **Mixed** - Combination of CPU and I/O work
- **Memory-intensive** - Large memory operations with random access

**Enhanced Features:**
- Template-based workload generators
- STL containers for statistics
- Smart pointer memory management
- Advanced fairness metrics (Jain's Index, Coefficient of Variation)

**Example:**
```bash
# Standard fairness test
./scheduler_analyzer_cpp -d 10 -t 8

# Priority testing with nice values
./scheduler_analyzer_cpp -d 15 -t 12 -n -v
```

### 10. Lock Contention Visualizer (C++ Version)

Modern lock analysis with custom lock implementations and real-time monitoring.

**Usage:**
```bash
./lock_visualizer_cpp [options]
  -t <threads>  Number of threads (default: 8)
  -d <seconds>  Duration of test (default: 10)
  -l <type>     Lock type: mutex, recursive, shared, spin, adaptive
  -w <pattern>  Workload: balanced, read, write, thunder, random, bursty
  -z            Enable real-time visualization
  -h            Show this help message
```

**Lock Types:**
- **std::mutex** - Standard mutex
- **std::recursive_mutex** - Recursive locking
- **std::shared_mutex** - Reader-writer lock
- **SpinLock** - Custom spinlock implementation
- **AdaptiveMutex** - Hybrid spin-then-block mutex

**Workload Patterns:**
- **Balanced** - 50% read, 50% write operations
- **Read-heavy** - 80% read, 20% write operations
- **Write-heavy** - 20% read, 80% write operations
- **Thundering herd** - High contention scenario
- **Random** - Variable access patterns
- **Bursty** - Periodic high activity

**Enhanced Features:**
- Real-time contention monitoring
- Custom lock type implementations
- Advanced statistics with percentiles
- Performance recommendations
- Type-safe template-based design

**Example:**
```bash
# Test shared mutex with read-heavy workload
./lock_visualizer_cpp -l shared -w read -z

# High contention spinlock test
./lock_visualizer_cpp -l spin -w thunder -t 16

# Adaptive mutex comparison
./lock_visualizer_cpp -l adaptive -d 20 -v
```

### 11. CPU Cache Performance Analyzer (C)

Comprehensive analysis of CPU cache hierarchy performance and cache-related bottlenecks.

**Usage:**
```bash
./cache_analyzer [options]
  -t <type>     Test type: hierarchy, false_sharing, bouncing, prefetcher, all
  -T <threads>  Number of threads (default: 4)
  -v            Verbose output with system information
  -h            Show this help message
```

**Test Types:**
- **Cache Hierarchy Analysis** - Tests L1, L2, L3 cache performance with different access patterns
- **False Sharing Detection** - Compares cache-friendly vs false sharing scenarios
- **Cache Line Bouncing** - Analyzes cache line bouncing between CPU cores
- **Hardware Prefetcher Analysis** - Tests prefetcher effectiveness with different stride patterns

**Key Features:**
- Multiple access patterns (sequential, random, stride, pointer chase)
- Cross-platform support (Linux/macOS)
- Thread CPU affinity control
- Detailed performance metrics with cache hit rate estimates
- Real-time contention analysis

**Example:**
```bash
# Run all cache tests with 8 threads
./cache_analyzer -t all -T 8

# Test cache hierarchy with verbose output
./cache_analyzer -t hierarchy -v

# False sharing analysis with 4 threads
./cache_analyzer -t false_sharing -T 4
```

### 12. CPU Cache Performance Analyzer (C++ Version)

Modern C++ implementation with template-based access patterns and enhanced safety features.

**Usage:**
```bash
./cache_analyzer_cpp [options]
  -t <type>     Test type: hierarchy, false_sharing, bouncing, prefetcher, all
  -T <threads>  Number of threads (default: 4)
  -v            Verbose output with detailed system information
  -h            Show this help message
```

**Enhanced Features:**
- **Template-based Access Patterns** - Type-safe, compile-time optimized access patterns
- **RAII Memory Management** - Automatic resource cleanup and exception safety
- **Aligned Memory Allocators** - Cache-line aligned memory allocation for optimal performance
- **STL Integration** - Modern C++ containers and algorithms
- **Advanced Statistics** - Comprehensive performance metrics and analysis

**Access Pattern Types:**
- **Sequential** - Linear memory access for maximum cache efficiency
- **Random** - Random access patterns to stress cache hierarchy
- **Stride** - Configurable stride patterns to test prefetcher limits
- **Pointer Chase** - Linked list traversal to defeat prefetchers
- **False Sharing** - Demonstrates false sharing performance impact
- **Cache Friendly** - Optimized access patterns for comparison

**Example:**
```bash
# Complete cache analysis suite
./cache_analyzer_cpp -t all -v

# Cache hierarchy test with detailed metrics
./cache_analyzer_cpp -t hierarchy -T 8 -v

# Hardware prefetcher effectiveness analysis
./cache_analyzer_cpp -t prefetcher
```

**Key Insights:**
- **L1 Cache** - Typically 32KB, ~1 cycle access time
- **L2 Cache** - Usually 256KB-1MB, ~3-10 cycles
- **L3 Cache** - Often 8-32MB, ~10-50 cycles
- **Memory Access** - 100-300 cycles, major performance impact
- **False Sharing** - Can reduce performance by 2-10x
- **Cache Line Size** - 64 bytes on most modern CPUs
- **Prefetcher Effectiveness** - Works well for strides ≤64 bytes

### 13. Disk I/O Performance Analyzer (C)

Comprehensive analysis of disk I/O performance characteristics and optimization opportunities.

**Usage:**
```bash
./disk_io_analyzer [options]
  -f <filename>  Test file path (default: ./diskio_test)
  -s <size>      File size in MB (default: 100)
  -t <type>      Test type: pattern, block, sync, modes, all (default: all)
  -T <threads>   Number of threads (default: 4)
  -d <duration>  Test duration in seconds (default: 10)
  -h             Show this help message
```

**Test Types:**
- **Pattern Analysis** - Sequential vs random read/write performance comparison
- **Block Size Analysis** - Optimal block size determination for different workloads
- **Sync Overhead Analysis** - fsync() and fdatasync() performance impact measurement
- **I/O Mode Comparison** - Buffered vs Direct vs Synchronous I/O performance
- **Multi-threading Scaling** - I/O performance scaling with thread count

**Key Features:**
- Cross-platform compatibility (Linux/macOS)
- Multiple access patterns (sequential, random, mixed)
- Various I/O modes (buffered, direct, synchronous)
- Thread synchronization with barriers
- Detailed latency histogram analysis
- Real-time throughput and IOPS measurement

**Example:**
```bash
# Run comprehensive I/O analysis
./disk_io_analyzer -t all -s 500 -T 8

# Test different block sizes for optimization
./disk_io_analyzer -t block -s 200 -d 15

# Measure sync overhead impact
./disk_io_analyzer -t sync -T 2 -d 10
```

### 14. Disk I/O Performance Analyzer (C++ Version)

Modern C++ implementation with template-based I/O patterns and advanced async capabilities.

**Usage:**
```bash
./disk_io_analyzer_cpp [options]
  -f <filename>  Test file path (default: ./diskio_test)
  -s <size>      File size in MB (default: 100)
  -t <type>      Test type: pattern, block, sync, modes, all (default: all)
  -T <threads>   Number of threads (default: 4)
  -d <duration>  Test duration in seconds (default: 10)
  -v             Verbose output with detailed statistics
  -h             Show this help message
```

**Enhanced Features:**
- **Template-based I/O Patterns** - Compile-time optimized I/O pattern execution
- **RAII Resource Management** - Automatic cleanup and exception safety
- **Aligned Memory Allocators** - Optimal memory alignment for direct I/O
- **Modern C++ Concurrency** - std::barrier, std::future, and std::async
- **Advanced Statistics** - Percentile analysis and detailed latency metrics
- **Type-safe Operations** - Template-based pattern selection and execution

**I/O Pattern Types:**
- **SequentialRead** - Linear read access for maximum throughput
- **SequentialWrite** - Linear write access with data verification
- **RandomRead** - Random access patterns to test seek performance
- **RandomWrite** - Random write operations with cache bypass
- **Mixed** - Realistic workload simulation (67% read, 33% write)

**I/O Mode Analysis:**
- **Buffered I/O** - Standard OS buffer cache utilization
- **Direct I/O** - Bypass OS cache for raw device performance
- **Synchronous I/O** - Force immediate disk writes for durability testing

**Example:**
```bash
# Complete I/O performance suite with verbose output
./disk_io_analyzer_cpp -t all -v -s 1000

# Pattern analysis with large file
./disk_io_analyzer_cpp -t pattern -s 2000 -T 8 -v

# Block size optimization for SSD/NVMe
./disk_io_analyzer_cpp -t block -T 16 -d 20
```

**Key Performance Insights:**
- **Sequential I/O** - Typically 10-50x faster than random I/O
- **Block Size Impact** - Larger blocks reduce syscall overhead but increase latency
- **Direct I/O** - 20-50% faster for large sequential operations
- **Sync Overhead** - Can reduce throughput by 10-100x depending on device
- **Thread Scaling** - Optimal thread count depends on storage type (SSD vs HDD)
- **Latency Distribution** - 99th percentile often 10-100x higher than average

### 15. Network I/O Benchmarker (C)

Comprehensive analysis of network I/O performance characteristics and protocol comparison.

**Usage:**
```bash
./network_io_benchmarker [options]
  -m <mode>      Mode: client or server (default: run both)
  -a <address>   Server address (default: 127.0.0.1)
  -p <port>      Port number (default: 12345)
  -P <protocol>  Protocol: tcp or udp (default: tcp)
  -t <type>      Test type: throughput, latency, buffer, multiplexer, all
  -s <size>      Message size in bytes (default: 1024)
  -n <count>     Number of messages (default: 10000)
  -d <duration>  Test duration in seconds (default: 10)
  -T <threads>   Number of threads (default: 1)
  -h             Show this help message
```

**Test Types:**
- **Throughput Test** - TCP vs UDP maximum throughput measurement
- **Latency Test** - Round-trip latency analysis with percentile distribution
- **Buffer Size Optimization** - Socket buffer size impact on performance
- **I/O Multiplexer Comparison** - select/poll/epoll/kqueue performance comparison
- **Connection Scaling** - Multi-connection performance analysis

**Key Features:**
- Cross-platform I/O multiplexing (select/poll/epoll/kqueue)
- TCP and UDP protocol support
- Socket buffer size optimization testing
- Real-time latency histogram analysis
- Connection scaling and load testing
- Multi-threaded client/server architecture

**I/O Multiplexers:**
- **select()** - Cross-platform, traditional multiplexing
- **poll()** - Improved scalability over select
- **epoll** (Linux) - High-performance event notification
- **kqueue** (macOS/BSD) - Efficient kernel event queue

**Example:**
```bash
# Run comprehensive network analysis
./network_io_benchmarker -t all -P tcp

# Test UDP vs TCP throughput
./network_io_benchmarker -t throughput -P udp -s 8192 -d 15

# Compare I/O multiplexers
./network_io_benchmarker -t multiplexer -P tcp

# Socket buffer optimization
./network_io_benchmarker -t buffer -P tcp
```

### 16. Network I/O Benchmarker (C++ Version)

Modern C++ implementation with async I/O patterns and advanced statistical analysis.

**Usage:**
```bash
./network_io_benchmarker_cpp [options]
  -a <address>   Server address (default: 127.0.0.1)
  -p <port>      Port number (default: 12345)
  -P <protocol>  Protocol: tcp or udp (default: tcp)
  -t <type>      Test type: throughput, latency, buffer, multiplexer, all
  -s <size>      Message size in bytes (default: 1024)
  -n <count>     Number of messages (default: 10000)
  -d <duration>  Test duration in seconds (default: 10)
  -T <threads>   Number of threads (default: 1)
  -v             Verbose output with percentiles
  -h             Show this help message
```

**Enhanced Features:**
- **RAII Socket Management** - Automatic resource cleanup and exception safety
- **Template-based Protocol Selection** - Compile-time optimized networking code
- **Modern C++ Concurrency** - std::future, std::async, and threading primitives
- **Advanced Statistics** - Comprehensive percentile analysis and latency distribution
- **Exception-safe Networking** - Robust error handling and resource management
- **Type-safe Operations** - Template-based socket and protocol management

**Network Analysis Types:**
- **TCP Performance** - Connection-oriented reliable delivery testing
- **UDP Performance** - Connectionless low-overhead performance measurement
- **Protocol Comparison** - Direct TCP vs UDP performance analysis
- **Buffer Size Impact** - Send/receive buffer optimization analysis
- **Multiplexer Efficiency** - Event-driven I/O performance comparison

**Advanced Features:**
- **Async I/O Patterns** - Modern C++ asynchronous networking
- **Connection Management** - Scalable connection handling with RAII
- **Statistical Analysis** - Detailed latency percentiles (50th, 75th, 90th, 95th, 99th, 99.9th)
- **Real-time Monitoring** - Live performance metrics and statistics
- **Cross-platform Networking** - Unified API across Linux/macOS/BSD

**Example:**
```bash
# Complete network performance suite with verbose statistics
./network_io_benchmarker_cpp -t all -v

# High-performance TCP throughput test
./network_io_benchmarker_cpp -t throughput -P tcp -s 65536 -v

# Detailed latency analysis with percentiles
./network_io_benchmarker_cpp -t latency -P tcp -s 64 -v

# Socket buffer size optimization for high throughput
./network_io_benchmarker_cpp -t buffer -P tcp -v
```

**Key Performance Insights:**
- **TCP vs UDP** - TCP typically 10-20% slower due to reliability overhead
- **Buffer Size Impact** - Larger buffers improve throughput but increase latency
- **I/O Multiplexer Performance** - epoll/kqueue 2-10x faster than select for many connections
- **Connection Scaling** - Performance degrades significantly beyond optimal connection count
- **Message Size Effect** - Larger messages improve efficiency but increase memory usage
- **Protocol Overhead** - TCP header (20+ bytes) vs UDP header (8 bytes) impact

## Performance Insights

### Context Switching
- Typical cost: 2-5 microseconds per switch
- Higher on same CPU due to cache effects
- Critical for understanding scheduling overhead

### Scheduler Fairness
- CPU-bound threads get ~33% each with 3 threads
- I/O-bound threads use <1% CPU time
- Nice values effectively prioritize threads
- Fairness index >0.9 indicates good distribution

### Lock Contention
- Mutex: Good general-purpose, 20-40% contention typical
- Spinlock: Fast but wastes CPU, avoid >30% contention
- RWLock: Best for >70% read workloads
- Thundering herd creates ~36% contention

### Virtual Memory
- TLB covers ~6MB before performance impact
- Page faults cost 1-50 microseconds each
- Huge pages provide 10-30% speedup for large data
- COW makes fork() nearly free until writes occur

### Memory Allocation
- malloc/free typically 0.1-1 microsecond per operation
- Fragmentation can increase RSS by 20-50%
- Multi-threaded scaling depends on allocator design
- Custom allocators can provide 2-5x speedup for specific patterns
- STL containers benefit from reserve() and PMR allocators

### CPU Cache Performance
- L1 cache hits: ~1 cycle latency, highest performance
- L2 cache hits: ~3-10 cycles, good performance
- L3 cache hits: ~10-50 cycles, moderate performance
- Memory access: ~100-300 cycles, major bottleneck
- Cache line size: 64 bytes on most modern processors
- False sharing can reduce performance by 2-10x
- Sequential access patterns optimize prefetcher effectiveness
- Random access patterns stress cache hierarchy most
- Cache-friendly data structures critical for performance

### Disk I/O Performance
- Sequential I/O: 100-3000 MB/s typical throughput (SSD/NVMe)
- Random I/O: 10-50x slower than sequential, especially on HDDs
- Block size impact: 4KB-1MB optimal depending on workload
- Direct I/O: 20-50% faster for large sequential operations
- Sync overhead: fsync() can reduce throughput by 10-100x
- Queue depth: Higher queue depth improves SSD performance
- Thread scaling: 2-16 threads optimal for most storage devices
- Latency distribution: 99th percentile often 10-100x average

### Network I/O Performance
- TCP throughput: 100-10000 MB/s typical on modern networks
- UDP throughput: 10-20% higher than TCP due to lower overhead
- Latency: LAN ~0.1-1ms, WAN ~10-200ms, varies by distance
- Buffer size impact: Larger buffers improve throughput, increase latency
- I/O multiplexer scaling: epoll/kqueue handle 10K+ connections efficiently
- Connection overhead: TCP setup/teardown costs 2-3 RTTs
- Protocol overhead: TCP 20+ bytes vs UDP 8 bytes per packet
- Congestion control: TCP adapts to network conditions automatically

## System Requirements

- POSIX-compliant OS (Linux, macOS, BSD)
- GCC or Clang compiler
- C99 and C++17 support
- pthread library
- 64-bit architecture recommended

## Use Cases

1. **Performance Tuning**
   - Identify scheduling bottlenecks
   - Optimize lock strategies
   - Reduce page faults
   - Minimize context switches
   - Optimize I/O patterns and block sizes

2. **System Analysis**
   - Understand OS overhead
   - Measure virtualization impact
   - Compare kernel versions
   - Validate real-time constraints

3. **Educational**
   - Learn OS concepts hands-on
   - Visualize abstract concepts
   - Benchmark student implementations
   - Research OS behavior

## Optimization Guidelines

Based on findings from these tools:

1. **Minimize Context Switches**
   - Use thread pools
   - Batch operations
   - Avoid excessive synchronization

2. **Optimize Memory Access**
   - Sequential > random access
   - Consider huge pages for large datasets
   - Pre-fault critical paths
   - Align data to cache lines

3. **Choose Right Synchronization**
   - Mutex for general use
   - Spinlock for short critical sections
   - RWLock for read-heavy workloads
   - Consider lock-free alternatives

4. **Scheduler Optimization**
   - Set appropriate nice values
   - Use CPU affinity for cache locality
   - Avoid oversubscription
   - Profile thread behavior

5. **Memory Allocator Optimization**
   - Use appropriate allocation patterns
   - Consider custom allocators for hot paths
   - Pre-allocate when possible
   - Monitor fragmentation levels
   - Use memory pools for fixed-size allocations

6. **CPU Cache Optimization**
   - Structure data for cache line alignment
   - Minimize false sharing between threads
   - Prefer sequential over random access patterns
   - Group frequently accessed data together
   - Consider cache-oblivious algorithms
   - Use prefetch hints for predictable patterns

7. **Disk I/O Optimization**
   - Use sequential access patterns when possible
   - Choose optimal block sizes for your workload
   - Consider direct I/O for large sequential operations
   - Minimize fsync() calls and use async I/O
   - Use appropriate queue depths for your storage
   - Align I/O operations to device block boundaries
   - Batch small I/O operations together

## Contributing

Feel free to extend these tools with:
- Additional test scenarios
- New metrics
- Platform-specific optimizations
- Visualization improvements

## License

This project is for educational and research purposes.

## Acknowledgments

These tools demonstrate fundamental OS concepts and help developers understand system behavior for better application design.