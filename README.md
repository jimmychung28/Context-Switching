# Operating System Performance Analysis Tools

A comprehensive suite of tools for analyzing and understanding operating system performance characteristics including context switching, scheduling, lock contention, virtual memory behavior, and memory allocation performance.

## Overview

This repository contains seven specialized tools for exploring different aspects of operating system performance:

1. **Context Switch Measurement** - Measures the overhead of process context switching
2. **Scheduler Fairness Analyzer** - Tests how the OS scheduler distributes CPU time
3. **Lock Contention Visualizer** - Analyzes synchronization primitive performance
4. **Virtual Memory Explorer (C)** - Deep dive into VM subsystem behavior
5. **Virtual Memory Explorer (C++)** - Modern C++ implementation with enhanced features
6. **Memory Allocator Benchmarker (C)** - Comprehensive malloc/free performance analysis
7. **Memory Allocator Benchmarker (C++)** - Advanced allocator testing with STL and custom allocators

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