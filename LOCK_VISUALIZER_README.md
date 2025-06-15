# Lock Contention Visualizer

A comprehensive tool for analyzing and visualizing lock contention patterns across different synchronization primitives. This program helps developers understand the performance characteristics of mutexes, spinlocks, and read-write locks under various workloads.

## Features

### Lock Types Supported
- **Mutex**: Standard pthread mutex
- **Spinlock**: Custom implementation with busy-waiting
- **RWLock**: Read-Write lock for concurrent readers
- **Adaptive**: Adaptive mutex (falls back to regular mutex on macOS)

### Workload Patterns
- **Balanced**: Alternating read/write operations
- **Read-Heavy**: Configurable read percentage (default 80%)
- **Write-Heavy**: Configurable write percentage (default 80%)
- **Thundering Herd**: All threads start simultaneously
- **Random**: Random mix of operations

### Analysis Features
- Per-thread contention statistics
- Wait time distribution histograms
- Real-time visualization mode
- Jain's Fairness Index calculation
- Performance insights and recommendations

## Building

```bash
gcc -o lock_visualizer src/lock_contention_visualizer.c -lpthread -lm
```

## Usage

```bash
./lock_visualizer [options]

Options:
  -t <threads>    Number of threads (default: 8)
  -d <seconds>    Duration of test (default: 10)
  -l <type>       Lock type: mutex, spin, rwlock, adaptive
  -w <pattern>    Workload: balanced, read-heavy, write-heavy, thunder, random
  -r <percent>    Read percentage for read/write workloads (default: 80)
  -i <microsec>   Work time inside lock (default: 10)
  -o <microsec>   Work time outside lock (default: 100)
  -v              Verbose output
  -z              Enable real-time visualization
  -h              Show help
```

## Examples

### Basic Mutex Test
```bash
./lock_visualizer -t 8 -d 5
```

### Spinlock Thundering Herd Test
```bash
./lock_visualizer -t 16 -l spin -w thunder -z
```
Watch in real-time as all threads compete for the lock simultaneously.

### Read-Write Lock Analysis
```bash
./lock_visualizer -t 8 -l rwlock -w read-heavy -r 90
```
Tests RWLock with 90% read operations.

### Heavy Contention Test
```bash
./lock_visualizer -t 32 -l mutex -i 100 -o 10
```
Long critical sections (100μs) with short work outside (10μs).

## Understanding the Output

### Per-Thread Statistics
```
Thread | Acquisitions | Contentions | Cont% | Avg Wait(μs) | Max Wait(μs) | Reads | Writes
-------|--------------|-------------|-------|--------------|--------------|-------|--------
     0 |        12345 |         987 |   8.0 |         45.2 |        512.3 |  6172 |   6173
```

- **Acquisitions**: Total lock acquisitions by thread
- **Contentions**: Times thread had to wait for lock
- **Cont%**: Contention rate percentage
- **Avg/Max Wait**: Wait time statistics in microseconds

### Wait Time Histogram
Shows distribution of wait times across all threads:
```
Wait Time Distribution (in microseconds):
     0.0-10.0      | ████████████████████████ 2500
    10.0-20.0      | ████████████ 1200
    20.0-30.0      | ████ 400
```

### Performance Metrics
- **Jain's Fairness Index**: 1.0 = perfect fairness, <0.5 = poor fairness
- **Total Contention Rate**: Percentage of acquisitions that faced contention
- **Acquisitions/second**: Throughput metric

## Performance Insights

### When to Use Each Lock Type

**Mutex**
- General purpose, good for most scenarios
- Efficient sleep/wake mechanism
- Best when contention is moderate to high

**Spinlock**
- Low-latency when contention is rare
- Good for very short critical sections (<10μs)
- Wastes CPU cycles under high contention

**RWLock**
- Excellent for read-heavy workloads (>70% reads)
- Allows concurrent readers
- Higher overhead than mutex for write-heavy loads

## Real-time Visualization Mode (-z)

Displays live contention metrics:
```
=== Lock Contention Real-time Monitor ===

Total Acquisitions: 45678
Total Contentions: 3456 (7.6%)
Current Waiters: 3
Max Waiters: 7

Contention Level: ███████░░░░░░░░░░░░░ 3/8
```

## Interpreting Results

### Low Contention (<5%)
- Lock is well-suited for workload
- Consider if lock is necessary

### Moderate Contention (5-20%)
- Normal for many applications
- Monitor for performance impact

### High Contention (>20%)
- Performance bottleneck likely
- Consider:
  - Reducing critical section size
  - Lock-free alternatives
  - Finer-grained locking

### Fairness Issues (Index <0.7)
- Some threads are starved
- Check for priority inversions
- Consider fair locking strategies

## Tips for Testing

1. **Baseline Test**: Start with default parameters
2. **Vary Thread Count**: Test with 2x and 4x CPU cores
3. **Adjust Work Ratios**: Change -i and -o to simulate your workload
4. **Compare Lock Types**: Test same scenario with different locks
5. **Use Real-time Mode**: Watch contention patterns develop

## Common Patterns

### CPU-Bound Application
```bash
./lock_visualizer -t 16 -i 100 -o 100
```

### I/O-Bound Application
```bash
./lock_visualizer -t 32 -i 10 -o 1000
```

### Database-like Workload
```bash
./lock_visualizer -l rwlock -w read-heavy -r 95
```