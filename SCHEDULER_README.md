# Scheduler Fairness Analyzer

A comprehensive tool to analyze how the operating system scheduler distributes CPU time among different types of workloads. This program creates multiple threads with CPU-intensive, I/O-intensive, and mixed workloads to test scheduler fairness.

## Features

- **Multiple Workload Types**:
  - CPU-intensive: Pure computational work (trigonometric calculations)
  - I/O-intensive: Pipe-based I/O operations with blocking
  - Mixed: Alternates between CPU and I/O work

- **Scheduler Testing Capabilities**:
  - Measures actual CPU time vs wall clock time
  - Tests impact of nice values on scheduling
  - Calculates fairness metrics (Jain's Fairness Index)
  - Per-thread and per-workload-type statistics

- **Detailed Analytics**:
  - Thread-level CPU usage statistics
  - Workload type comparisons
  - Standard deviation and min/max analysis
  - Overall CPU utilization metrics

## Building

```bash
gcc -o scheduler_analyzer src/scheduler_analyzer.c -lpthread -lm
```

## Usage

```bash
./scheduler_analyzer [options]

Options:
  -d <seconds>    Duration of test (default: 10)
  -c <count>      Number of CPU-intensive threads
  -i <count>      Number of IO-intensive threads
  -m <count>      Number of mixed workload threads
  -n              Enable nice values (CPU: -5, IO: 5, Mixed: 0)
  -v              Verbose output
  -h              Show help
```

## Examples

### Basic Test (5 seconds, default thread counts)
```bash
./scheduler_analyzer -d 5
```

### Test with Nice Values
```bash
./scheduler_analyzer -d 30 -c 4 -i 4 -m 2 -n
```
This runs:
- 4 CPU-intensive threads (nice -5, higher priority)
- 4 I/O-intensive threads (nice 5, lower priority)
- 2 Mixed workload threads (nice 0, normal priority)

### Heavy CPU Load Test
```bash
./scheduler_analyzer -d 20 -c 16 -i 0 -m 0
```

### I/O vs CPU Comparison
```bash
./scheduler_analyzer -d 15 -c 8 -i 8 -m 0
```

## Understanding the Output

### Per-Thread Results
```
Thread ID | Type  | Nice | Iterations | CPU Time | CPU % | Wall Time
----------|-------|------|------------|----------|-------|----------
        0 |   CPU |   -5 |    1234567 |    8.543 |  25.3 |     9.123
```

- **Iterations**: Number of work cycles completed
- **CPU Time**: Actual CPU time consumed by thread
- **CPU %**: Percentage of total CPU time across all threads
- **Wall Time**: Real elapsed time from start to finish

### Workload Statistics
Shows mean, standard deviation, and min/max CPU times for each workload type.

### Fairness Metrics
- **Jain's Fairness Index**: Ranges from 0 to 1 (1 = perfect fairness)
- **CPU Utilization**: Total CPU usage across all cores

## What to Look For

1. **Fair Scheduling**: Without nice values, threads of the same type should get similar CPU time
2. **Nice Value Impact**: With -n flag, CPU-intensive threads should get more time
3. **I/O Blocking**: I/O threads will show lower CPU usage due to blocking
4. **Scheduler Efficiency**: High CPU utilization indicates good scheduler performance

## System Requirements

- POSIX-compliant system (Linux, macOS, BSD)
- pthread library support
- C99 compiler
- Multi-core CPU recommended for best results

## Interpreting Results

### Fairness Index Values
- **0.9-1.0**: Excellent fairness
- **0.7-0.9**: Good fairness
- **0.5-0.7**: Moderate fairness
- **< 0.5**: Poor fairness (investigate scheduler issues)

### Expected Behaviors
- CPU-intensive threads should fully utilize available CPU
- I/O-intensive threads should show lower CPU usage
- Mixed workloads should fall between CPU and I/O
- Nice values should create noticeable priority differences