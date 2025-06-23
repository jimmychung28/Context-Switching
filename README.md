# Operating System Performance Analysis Toolkit

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20BSD-blue.svg)]()
[![Language](https://img.shields.io/badge/language-C%2FC%2B%2B-orange.svg)]()

A comprehensive, production-ready suite of **16 specialized tools** for deep analysis and understanding of operating system performance characteristics. This toolkit provides both C and C++ implementations covering all major OS subsystems: process management, memory systems, storage I/O, networking, and CPU utilization.

## 🚀 Quick Start

```bash
# Clone and build all tools
git clone <repository-url>
cd Context-Switching
make all

# Run a quick performance overview
./context_switch_macos                           # Context switch overhead
./scheduler_analyzer -d 5 -c 2 -i 2             # Scheduler fairness
./lock_visualizer -t 4 -l mutex                 # Lock contention
./vm_explorer -t tlb                            # Virtual memory
./cache_analyzer -t hierarchy                   # CPU cache performance
./disk_io_analyzer -t pattern -s 100            # Disk I/O patterns
./network_io_benchmarker -t throughput          # Network performance
```

## 📋 Table of Contents

- [Overview](#overview)
- [Architecture and Design](#architecture-and-design)
- [Tool Categories](#tool-categories)
- [Installation](#installation)
- [Detailed Tool Documentation](#detailed-tool-documentation)
- [Performance Insights and Benchmarks](#performance-insights-and-benchmarks)
- [Use Cases and Applications](#use-cases-and-applications)
- [Advanced Usage Examples](#advanced-usage-examples)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)

## 🎯 Overview

This toolkit addresses the critical need for **quantitative operating system performance analysis** in modern computing environments. Each tool is designed to:

- **Measure real-world performance characteristics** with microsecond precision
- **Provide actionable insights** for system optimization
- **Support cross-platform analysis** (Linux, macOS, BSD)
- **Scale from single-core to many-core systems**
- **Integrate with existing development workflows**

### Why This Toolkit?

Modern applications face increasing performance challenges:
- **Multi-core scaling** requires understanding of synchronization overhead
- **Memory hierarchies** demand cache-aware algorithms
- **Storage systems** need I/O pattern optimization
- **Network applications** must handle connection scaling
- **Cloud deployments** require predictable performance characteristics

This toolkit provides the measurement foundation for addressing these challenges.

## 🏗️ Architecture and Design

### Design Principles

1. **Precision**: High-resolution timing with nanosecond accuracy
2. **Scalability**: Linear scaling from 1 to 64+ cores/threads
3. **Portability**: Cross-platform support with platform-specific optimizations
4. **Safety**: Memory-safe implementations with comprehensive error handling
5. **Extensibility**: Template-based and modular design for easy extension

### Implementation Approaches

**C Implementations**: 
- Minimal overhead for maximum measurement accuracy
- Direct system call interface
- Custom synchronization primitives for macOS compatibility
- Optimized for embedded and resource-constrained environments

**C++ Implementations**:
- Modern C++17 features (RAII, templates, STL)
- Exception-safe resource management
- Advanced statistical analysis
- Type-safe template-based patterns
- Future-ready with async/await patterns

## 🔧 Tool Categories

### 🔄 Process and Scheduling Analysis
| Tool | Purpose | Key Metrics |
|------|---------|-------------|
| Context Switch Measurement | Process switching overhead | Switch time: 2-5μs typical |
| Scheduler Fairness Analyzer | CPU time distribution | Fairness index: >0.9 optimal |

### 🔒 Synchronization and Concurrency
| Tool | Purpose | Key Metrics |
|------|---------|-------------|
| Lock Contention Visualizer | Synchronization performance | Contention: <30% optimal |

### 💾 Memory System Analysis
| Tool | Purpose | Key Metrics |
|------|---------|-------------|
| Virtual Memory Explorer | VM subsystem behavior | TLB coverage: ~6MB |
| Memory Allocator Benchmarker | Dynamic allocation performance | malloc/free: 0.1-1μs |
| CPU Cache Performance Analyzer | Cache hierarchy analysis | L1: ~1 cycle, L3: ~50 cycles |

### 💽 Storage and Network I/O
| Tool | Purpose | Key Metrics |
|------|---------|-------------|
| Disk I/O Performance Analyzer | Storage system optimization | Sequential: 100-3000 MB/s |
| Network I/O Benchmarker | Network performance analysis | TCP/UDP: 100-10000 MB/s |

## 📦 Installation

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install build-essential gcc g++ libc6-dev

# macOS
xcode-select --install

# CentOS/RHEL
sudo yum groupinstall "Development Tools"
```

### Build Options

```bash
# Standard build (all tools)
make all

# Individual tool builds
make context_switch_macos scheduler_analyzer lock_visualizer
make vm_explorer vm_explorer_cpp
make memory_allocator memory_allocator_cpp
make cache_analyzer cache_analyzer_cpp
make disk_io_analyzer disk_io_analyzer_cpp
make network_io_benchmarker network_io_benchmarker_cpp

# Clean build
make clean && make all

# Install with usage examples
make install
```

### Verification

```bash
# Verify all tools built successfully
ls -la context_switch_macos scheduler_analyzer lock_visualizer vm_explorer* \
       memory_allocator* cache_analyzer* disk_io_analyzer* network_io_benchmarker*

# Run quick functionality tests
./context_switch_macos
./scheduler_analyzer -d 2
./vm_explorer -t tlb -s 64
```

## 📖 Detailed Tool Documentation

### 1. Context Switch Measurement

**Purpose**: Quantifies the overhead of process context switching, a fundamental OS operation affecting application performance.

**C Implementation** (`context_switch_macos.c`):
```bash
./context_switch_macos
```

**C++ Implementation** (`context_switch_cpp.cpp`):
```bash
./context_switch_cpp [options]
  -n <count>    Number of iterations (default: 1000)
  -v            Verbose output with detailed statistics
```

**Measurement Methods**:
- **Pipe-based IPC**: Traditional process switching measurement
- **Shared Memory**: Low-overhead synchronization method
- **Thread Switching**: Intra-process context switch measurement

**Key Insights**:
- Context switches typically cost 2-5 microseconds
- Same-CPU switches show cache effects
- Thread switches are 10-50x faster than process switches
- NUMA systems show higher variance

**Example Output**:
```
Context Switch Performance Analysis
==================================
Process Context Switches:
  Average: 3.2 μs
  Min: 2.1 μs, Max: 8.7 μs
  50th percentile: 3.1 μs
  95th percentile: 4.8 μs
  99th percentile: 6.2 μs

Thread Context Switches:
  Average: 0.15 μs
  Performance ratio: 21.3x faster than processes
```

### 2. Scheduler Fairness Analyzer

**Purpose**: Analyzes how the operating system scheduler distributes CPU time across different types of workloads under varying conditions.

**C Implementation**:
```bash
./scheduler_analyzer [options]
  -t <threads>    Number of threads (default: 8)
  -d <seconds>    Duration of test (default: 10)
  -c <count>      Number of CPU-intensive threads
  -i <count>      Number of IO-intensive threads
  -m <count>      Number of mixed workload threads
  -n              Enable nice values for priority testing
```

**C++ Implementation**:
```bash
./scheduler_analyzer_cpp [options]
  -d <seconds>    Duration of test (default: 10)
  -t <threads>    Number of threads (default: 4)
  -n              Enable nice values for priority testing
  -v              Verbose output with detailed statistics
```

**Workload Types**:
- **CPU-intensive**: Mathematical computations (matrix multiplication, prime finding)
- **I/O-intensive**: Sleep-based I/O simulation with realistic timings
- **Mixed**: Combination workloads (67% CPU, 33% I/O)
- **Memory-intensive**: Large memory operations with random access patterns

**Fairness Metrics**:
- **Jain's Fairness Index**: Measures CPU time distribution equality
- **Coefficient of Variation**: Statistical measure of fairness
- **Contention Rates**: Thread blocking and waiting statistics

**Example Analysis**:
```bash
# Standard fairness test with mixed workloads
./scheduler_analyzer_cpp -d 15 -t 8 -v

# Priority testing with nice values
./scheduler_analyzer_cpp -d 20 -t 12 -n

# Long-duration stress test
./scheduler_analyzer -d 300 -c 4 -i 4 -m 4 -n
```

**Expected Results**:
- Fair scheduler: Jain's Index >0.9
- CPU threads: ~33% CPU each (3 threads)
- I/O threads: <1% CPU usage
- Nice values: 2x priority difference typically

### 3. Lock Contention Visualizer

**Purpose**: Analyzes synchronization primitive performance under different contention patterns and workloads.

**C Implementation**:
```bash
./lock_visualizer [options]
  -t <threads>    Number of threads (default: 8)
  -l <type>       Lock type: mutex, spin, rwlock, adaptive
  -w <pattern>    Workload: balanced, read-heavy, write-heavy, thunder, random
  -z              Enable real-time visualization
```

**C++ Implementation**:
```bash
./lock_visualizer_cpp [options]
  -t <threads>    Number of threads (default: 8)
  -d <seconds>    Duration of test (default: 10)
  -l <type>       Lock type: mutex, recursive, shared, spin, adaptive
  -w <pattern>    Workload: balanced, read, write, thunder, random, bursty
  -z              Enable real-time visualization
```

**Lock Types**:
- **std::mutex**: Standard POSIX mutex
- **std::recursive_mutex**: Allows recursive locking
- **std::shared_mutex**: Reader-writer lock (C++17)
- **SpinLock**: Custom high-performance spinlock
- **AdaptiveMutex**: Hybrid spin-then-block implementation

**Workload Patterns**:
- **Balanced**: 50% read, 50% write operations
- **Read-heavy**: 80% read, 20% write (optimal for RWLock)
- **Write-heavy**: 20% read, 80% write
- **Thundering herd**: High contention stress test
- **Bursty**: Periodic high activity simulation

**Performance Analysis**:
```bash
# Compare lock types under high contention
./lock_visualizer_cpp -l mutex -w thunder -t 16 -z
./lock_visualizer_cpp -l spin -w thunder -t 16 -z
./lock_visualizer_cpp -l shared -w thunder -t 16 -z

# Analyze read-heavy workloads
./lock_visualizer_cpp -l shared -w read -t 8 -d 30

# Test adaptive lock behavior
./lock_visualizer_cpp -l adaptive -w random -t 12 -v
```

### 4. Virtual Memory Explorer

**Purpose**: Deep analysis of virtual memory subsystem performance, including TLB behavior, page faults, and memory management strategies.

**Implementation**:
```bash
./vm_explorer [options]
  -s <size>    Memory size in MB (default: 256)
  -t <test>    Test type: tlb, fault, huge, mmap, cow, all
  -p <pattern> Access pattern: seq, rand, stride, chase
```

**Test Categories**:

**TLB Miss Analysis**:
- Measures Translation Lookaside Buffer efficiency
- Tests various access patterns and memory sizes
- Identifies TLB coverage limits (~6MB typical)

**Page Fault Measurement**:
- Quantifies demand paging overhead
- Tests major vs minor page faults
- Analyzes prefaulting strategies

**Huge Pages Testing**:
- Compares 2MB vs 4KB page performance
- Measures TLB pressure reduction
- Analyzes huge page allocation overhead

**Memory Mapping Comparison**:
- mmap() vs malloc() performance analysis
- Anonymous vs file-backed mappings
- Memory-mapped I/O performance

**Copy-on-Write Analysis**:
- fork() efficiency measurement
- COW fault overhead quantification
- Memory sharing effectiveness

**Advanced Usage**:
```bash
# Comprehensive VM analysis
./vm_explorer_cpp -t all -s 1024

# TLB stress testing
./vm_explorer -t tlb -s 2048 -p rand

# Huge pages effectiveness
./vm_explorer -t huge -s 1024 -p seq

# Memory mapping performance
./vm_explorer -t mmap -s 512 -p stride
```

### 5. Memory Allocator Benchmarker

**Purpose**: Comprehensive analysis of dynamic memory allocation performance across different patterns and allocators.

**C Implementation**:
```bash
./memory_allocator [options]
  -t <type>     Test type: speed, frag, scale, all
  -p <pattern>  Allocation pattern: seq, rand, exp, bimodal, real
  -s <size>     Min allocation size (default: 8)
  -S <size>     Max allocation size (default: 8192)
  -n <count>    Number of iterations (default: 1000)
```

**C++ Implementation**:
```bash
./memory_allocator_cpp [options]
  -t <type>     Test type: speed, frag, scale, container, custom, all
  -p <pattern>  Allocation pattern: seq, rand, exp, bimodal, real
```

**Test Types**:

**Speed Testing**:
- malloc/free latency measurement
- Allocation size impact analysis
- Threading overhead quantification

**Fragmentation Analysis**:
- Memory fragmentation tracking over time
- RSS vs requested memory monitoring
- Fragmentation scoring algorithms

**Scalability Testing**:
- Multi-threaded allocation performance
- Lock contention in allocators
- NUMA-aware allocation strategies

**Container Performance** (C++ only):
- std::vector, std::map, std::unordered_map benchmarks
- STL allocator comparison
- Container growth strategy analysis

**Custom Allocator Testing** (C++ only):
- Standard vs aligned allocators
- PMR (Polymorphic Memory Resources) performance
- Pool allocator effectiveness

**Allocation Patterns**:
- **Sequential**: Predictable size increases
- **Random**: Uniform random size distribution
- **Exponential**: Real-world exponential distribution
- **Bimodal**: Small + large allocation mix
- **Realistic**: Application-derived patterns

### 6. CPU Cache Performance Analyzer

**Purpose**: Comprehensive analysis of CPU cache hierarchy performance and cache-related optimization opportunities.

**C Implementation**:
```bash
./cache_analyzer [options]
  -t <type>     Test type: hierarchy, false_sharing, bouncing, prefetcher, all
  -T <threads>  Number of threads (default: 4)
  -v            Verbose output with system information
```

**C++ Implementation**:
```bash
./cache_analyzer_cpp [options]
  -t <type>     Test type: hierarchy, false_sharing, bouncing, prefetcher, all
  -T <threads>  Number of threads (default: 4)
  -v            Verbose output with detailed system information
```

**Analysis Categories**:

**Cache Hierarchy Analysis**:
- L1, L2, L3 cache performance measurement
- Cache hit/miss ratio estimation
- Memory access latency quantification
- Bandwidth measurement per cache level

**False Sharing Detection**:
- Cache line bouncing between cores
- Performance impact quantification (2-10x slowdown)
- Cache-friendly vs problematic data layouts

**Cache Line Bouncing**:
- Inter-core cache coherency overhead
- Write-invalidate performance impact
- Optimal data partitioning strategies

**Hardware Prefetcher Analysis**:
- Prefetcher effectiveness measurement
- Stride pattern optimization
- Prefetcher defeat strategies

**Access Pattern Types**:
- **Sequential**: Cache-friendly linear access
- **Random**: Cache-hostile random access
- **Stride**: Configurable stride patterns
- **Pointer Chase**: Linked list traversal patterns

**Performance Insights**:
```bash
# Complete cache analysis
./cache_analyzer_cpp -t all -T 8 -v

# False sharing investigation
./cache_analyzer -t false_sharing -T 4

# Prefetcher effectiveness testing
./cache_analyzer_cpp -t prefetcher -v
```

**Expected Performance Characteristics**:
- L1 Cache: ~32KB, 1-2 cycles latency
- L2 Cache: ~256KB-1MB, 3-10 cycles latency
- L3 Cache: ~8-32MB, 10-50 cycles latency
- Memory: 100-300 cycles latency

### 7. Disk I/O Performance Analyzer

**Purpose**: Comprehensive disk I/O performance analysis and optimization strategy development.

**C Implementation**:
```bash
./disk_io_analyzer [options]
  -f <filename>  Test file path (default: ./diskio_test)
  -s <size>      File size in MB (default: 100)
  -t <type>      Test type: pattern, block, sync, modes, all
  -T <threads>   Number of threads (default: 4)
  -d <duration>  Test duration in seconds (default: 10)
```

**C++ Implementation**:
```bash
./disk_io_analyzer_cpp [options]
  -f <filename>  Test file path (default: ./diskio_test)
  -s <size>      File size in MB (default: 100)
  -t <type>      Test type: pattern, block, sync, modes, all
  -T <threads>   Number of threads (default: 4)
  -d <duration>  Test duration in seconds (default: 10)
  -v             Verbose output with detailed statistics
```

**Analysis Categories**:

**I/O Pattern Analysis**:
- Sequential vs random read/write performance
- Mixed workload simulation (67% read, 33% write)
- Access pattern impact on throughput and latency

**Block Size Optimization**:
- Systematic block size testing (4KB to 1MB)
- Optimal block size identification
- Syscall overhead vs transfer efficiency trade-offs

**Synchronization Overhead**:
- fsync() and fdatasync() performance impact
- Durability vs performance trade-offs
- Write barrier overhead measurement

**I/O Mode Comparison**:
- Buffered I/O: Standard OS cache utilization
- Direct I/O: Cache bypass for raw performance
- Synchronous I/O: Immediate durability guarantees

**Threading and Scalability**:
- Multi-threaded I/O performance scaling
- Queue depth optimization
- Thread pool vs thread-per-operation

**Storage Performance Examples**:
```bash
# Comprehensive I/O analysis
./disk_io_analyzer_cpp -t all -s 500 -T 8 -v

# Block size optimization for SSDs
./disk_io_analyzer -t block -s 1000 -T 16

# Sync overhead impact measurement
./disk_io_analyzer_cpp -t sync -T 4 -d 30

# I/O mode comparison for databases
./disk_io_analyzer -t modes -s 2000 -T 8
```

### 8. Network I/O Benchmarker

**Purpose**: Comprehensive network I/O performance analysis and protocol optimization.

**C Implementation**:
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
```

**C++ Implementation**:
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
```

**Analysis Categories**:

**Protocol Comparison**:
- TCP vs UDP throughput and latency analysis
- Protocol overhead measurement
- Reliability vs performance trade-offs

**Socket Buffer Optimization**:
- Send/receive buffer size impact
- Memory usage vs performance trade-offs
- Kernel buffer tuning strategies

**I/O Multiplexer Performance**:
- select() vs poll() vs epoll/kqueue comparison
- Connection scaling analysis
- Event-driven vs threaded architecture performance

**Connection Scaling**:
- Multi-connection performance measurement
- Connection pooling effectiveness
- Resource utilization analysis

**Latency Analysis**:
- Round-trip time measurement
- Latency percentile distribution
- Jitter and variance analysis

**Network Performance Examples**:
```bash
# Complete network performance suite
./network_io_benchmarker_cpp -t all -v

# High-throughput TCP testing
./network_io_benchmarker -t throughput -P tcp -s 65536 -T 8

# Low-latency UDP analysis
./network_io_benchmarker_cpp -t latency -P udp -s 64 -v

# I/O multiplexer comparison
./network_io_benchmarker -t multiplexer -P tcp

# Socket buffer optimization
./network_io_benchmarker_cpp -t buffer -P tcp -v
```

## 📊 Performance Insights and Benchmarks

### Context Switching Performance

| System Type | Typical Range | Factors |
|-------------|---------------|---------|
| Modern Linux x86_64 | 2-4 μs | CPU architecture, kernel version |
| macOS ARM64 (M1/M2) | 1-3 μs | Unified memory architecture |
| Virtualized Environment | 5-15 μs | Hypervisor overhead |
| Container | 2-5 μs | Minimal overhead vs bare metal |

### Memory System Performance

| Component | Latency | Bandwidth | Notes |
|-----------|---------|-----------|-------|
| L1 Cache | 1-2 cycles | 1-2 TB/s | Per-core, fastest access |
| L2 Cache | 3-10 cycles | 200-500 GB/s | Per-core or shared |
| L3 Cache | 10-50 cycles | 100-300 GB/s | Shared across cores |
| Main Memory | 100-300 cycles | 50-100 GB/s | NUMA effects significant |
| TLB Coverage | - | - | ~6MB before performance impact |

### Lock Performance Characteristics

| Lock Type | Overhead | Best Use Case | Contention Tolerance |
|-----------|----------|---------------|---------------------|
| std::mutex | 10-50 ns | General purpose | Moderate (20-40%) |
| Spinlock | 5-20 ns | Short critical sections | Low (<30%) |
| std::shared_mutex | 15-100 ns | Read-heavy workloads | High for readers |
| Adaptive | Variable | Mixed workloads | High |

### Storage Performance Expectations

| Storage Type | Sequential Read | Random Read | Sequential Write | Random Write |
|--------------|----------------|-------------|------------------|--------------|
| NVMe SSD | 2000-7000 MB/s | 50-200 MB/s | 1000-5000 MB/s | 30-150 MB/s |
| SATA SSD | 500-600 MB/s | 20-50 MB/s | 400-500 MB/s | 15-40 MB/s |
| 7200 RPM HDD | 100-200 MB/s | 1-3 MB/s | 80-150 MB/s | 1-3 MB/s |
| RAM Disk | 5000-20000 MB/s | 3000-15000 MB/s | 5000-20000 MB/s | 3000-15000 MB/s |

### Network Performance Baselines

| Network Type | Bandwidth | Latency | Notes |
|--------------|-----------|---------|-------|
| Localhost (loopback) | 10-100 GB/s | 10-100 μs | CPU and memory limited |
| 1 Gigabit Ethernet | 100-120 MB/s | 100-500 μs | Protocol overhead |
| 10 Gigabit Ethernet | 1000-1200 MB/s | 50-200 μs | CPU becomes bottleneck |
| 100 Gigabit Ethernet | 10000+ MB/s | 10-50 μs | Requires kernel bypass |

## 💡 Use Cases and Applications

### System Optimization

**Database Performance Tuning**:
```bash
# Analyze I/O patterns for database workloads
./disk_io_analyzer -t pattern -s 10000 -T 16
./memory_allocator -t frag -p real
./lock_visualizer -l shared -w read -t 32
```

**Web Server Optimization**:
```bash
# Network and memory optimization
./network_io_benchmarker -t all -P tcp
./memory_allocator_cpp -t container -p bimodal
./scheduler_analyzer -d 60 -i 8 -c 4
```

**HPC Application Analysis**:
```bash
# NUMA and cache optimization
./cache_analyzer -t all -T 64
./vm_explorer -t all -s 8192
./scheduler_analyzer -d 300 -c 64
```

### Development and Testing

**Performance Regression Testing**:
```bash
#!/bin/bash
# Automated performance regression suite
echo "Context Switch Baseline: $(./context_switch_macos | grep Average)"
echo "Cache Performance: $(./cache_analyzer -t hierarchy | grep L3)"
echo "I/O Throughput: $(./disk_io_analyzer -t pattern -d 5 | grep Throughput)"
```

**Scalability Analysis**:
```bash
# Thread scaling analysis
for threads in 1 2 4 8 16 32; do
    echo "Threads: $threads"
    ./lock_visualizer -t $threads -l mutex -w balanced -d 10
done
```

### Research and Education

**Operating Systems Coursework**:
- Context switching concepts visualization
- Scheduler fairness demonstrations
- Memory hierarchy exploration
- Lock contention analysis

**System Research**:
- Baseline performance characterization
- Algorithm comparison frameworks
- Hardware impact analysis

## 🔧 Advanced Usage Examples

### Custom Performance Analysis Scripts

**System Health Check**:
```bash
#!/bin/bash
# comprehensive_perf_check.sh
echo "=== System Performance Health Check ==="

echo "Context Switch Performance:"
./context_switch_macos

echo -e "\nScheduler Fairness (30 second test):"
./scheduler_analyzer -d 30 -c 4 -i 4 -m 2

echo -e "\nMemory Allocation Performance:"
./memory_allocator_cpp -t speed -n 10000

echo -e "\nCache Performance:"
./cache_analyzer -t hierarchy -v

echo -e "\nDisk I/O Performance:"
./disk_io_analyzer -t pattern -s 1000 -d 15

echo -e "\nNetwork Performance:"
./network_io_benchmarker -t throughput -d 10
```

**Comparative Analysis**:
```bash
#!/bin/bash
# compare_lock_types.sh
echo "=== Lock Type Performance Comparison ==="

for lock_type in mutex spin shared adaptive; do
    echo "Testing $lock_type:"
    ./lock_visualizer_cpp -l $lock_type -w balanced -t 8 -d 10
    echo ""
done
```

### Integration with Monitoring Systems

**Prometheus Metrics Export**:
```bash
#!/bin/bash
# Export performance metrics in Prometheus format
PREFIX="os_perf"

# Context switch metrics
CTX_TIME=$(./context_switch_macos | grep -o '[0-9.]*' | head -1)
echo "${PREFIX}_context_switch_microseconds $CTX_TIME"

# Cache performance metrics
L1_TIME=$(./cache_analyzer -t hierarchy | grep "L1" | grep -o '[0-9.]*' | head -1)
echo "${PREFIX}_l1_cache_access_nanoseconds $L1_TIME"

# Disk I/O metrics
DISK_THROUGHPUT=$(./disk_io_analyzer -t pattern -d 5 | grep "Throughput" | grep -o '[0-9.]*' | head -1)
echo "${PREFIX}_disk_throughput_mbps $DISK_THROUGHPUT"
```

### Continuous Integration Integration

**GitHub Actions Workflow**:
```yaml
name: Performance Regression Tests
on: [push, pull_request]

jobs:
  performance-tests:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v2
    - name: Build Performance Tools
      run: make all
    - name: Run Performance Baseline Tests
      run: |
        ./context_switch_macos > baseline.txt
        ./scheduler_analyzer -d 5 >> baseline.txt
        ./cache_analyzer -t hierarchy >> baseline.txt
    - name: Archive Performance Results
      uses: actions/upload-artifact@v2
      with:
        name: performance-baseline
        path: baseline.txt
```

## 🐛 Troubleshooting

### Common Issues and Solutions

**Build Issues**:

```bash
# Missing pthread library
# Solution: Install development packages
sudo apt-get install libc6-dev libpthread-stubs0-dev  # Ubuntu/Debian
sudo yum groupinstall "Development Tools"              # CentOS/RHEL

# C++17 support missing
# Solution: Use modern compiler
g++ --version  # Ensure GCC 7+ or Clang 5+
```

**Runtime Issues**:

```bash
# Permission denied for network tests
# Solution: Use unprivileged ports or run with privileges
./network_io_benchmarker -p 12345  # Use port > 1024

# Insufficient memory for large tests
# Solution: Reduce test size
./vm_explorer -s 100     # Reduce from default 256MB
./disk_io_analyzer -s 50 # Reduce from default 100MB
```

**Performance Anomalies**:

```bash
# Inconsistent results
# Solution: Isolate CPU cores and disable frequency scaling
sudo cpupower frequency-set -g performance
taskset -c 0 ./context_switch_macos

# High variance in measurements
# Solution: Increase measurement duration
./scheduler_analyzer -d 60  # Longer test duration
./disk_io_analyzer -d 30    # Longer measurement period
```

### Platform-Specific Notes

**macOS**:
- Some features require elevated privileges
- Thermal throttling may affect results
- Code signing may be required for some operations

**Linux**:
- NUMA topology affects memory tests
- CPU governor settings impact measurements
- Container environments may show different characteristics

**Virtual Machines**:
- Context switch overhead significantly higher
- Storage performance depends on hypervisor
- Network performance may be limited by virtual networking

### Performance Tuning for Accurate Measurements

**System Preparation**:
```bash
# Disable CPU frequency scaling
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Disable swap to avoid interference
sudo swapoff -a

# Set process priority
sudo nice -n -20 ./context_switch_macos

# Pin to specific CPU cores
taskset -c 0,1 ./scheduler_analyzer -t 2
```

**Measurement Best Practices**:
- Run multiple iterations and analyze variance
- Use appropriate test durations (10-60 seconds)
- Monitor system load during tests
- Account for thermal throttling on laptops
- Use dedicated benchmark systems when possible

## 🤝 Contributing

We welcome contributions to improve and extend this performance analysis toolkit!

### Development Setup

```bash
git clone <repository-url>
cd Context-Switching
make all
make install  # Verify all tools work
```

### Contribution Areas

**New Tools**: Additional OS subsystem analyzers
**Platform Support**: BSD, Solaris, Windows Subsystem for Linux
**Visualization**: Graphical output and real-time monitoring
**Integration**: Cloud monitoring and CI/CD integrations
**Optimization**: Performance improvements and new algorithms

### Code Style Guidelines

- **C Code**: Follow Linux kernel style guidelines
- **C++ Code**: Modern C++17 features, RAII, exception safety
- **Documentation**: Comprehensive comments and usage examples
- **Testing**: Verify on multiple platforms and configurations

### Submitting Changes

1. Fork the repository
2. Create a feature branch
3. Add comprehensive tests
4. Update documentation
5. Submit a pull request with detailed description

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgments

- Linux kernel developers for performance interfaces
- FreeBSD and macOS teams for system call documentation
- Academic research community for performance analysis methodologies
- Open source contributors for inspiration and code review

## 📚 References and Further Reading

- **"Systems Performance" by Brendan Gregg** - Comprehensive system performance analysis
- **"Computer Systems: A Programmer's Perspective" by Bryant & O'Hallaron** - Low-level system understanding
- **"The Art of Multiprocessor Programming" by Herlihy & Shavit** - Concurrent programming insights
- **Linux kernel documentation** - Implementation details and interfaces
- **Intel and AMD optimization manuals** - CPU-specific performance characteristics

---

**⭐ Star this repository if you find it useful for your performance analysis work!**

**📝 Report issues or request features through GitHub Issues**

**📧 Contact the maintainers for enterprise support and consulting**