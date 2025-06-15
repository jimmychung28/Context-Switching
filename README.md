# Context Switching Cost Measurement

A C program that measures the overhead of context switching between processes in an operating system using pipe-based inter-process communication.

## Overview

This tool creates a parent and child process that communicate through pipes, forcing the OS scheduler to perform context switches. By measuring the time for multiple round-trip communications, it calculates the average cost of a context switch in nanoseconds.

## Features

- Measures real-world context switching overhead
- Supports CPU affinity to control process placement
- Uses high-resolution POSIX timers for accurate measurements
- Performs 1000 iterations for statistical reliability

## Requirements

- Linux operating system (uses GNU-specific CPU affinity features)
- GCC compiler
- POSIX real-time library support

## Building

```bash
gcc -o context_switch "src/Cost of Context Switching.c" -lrt
```

## Usage

```bash
./context_switch <parent-cpu> <child-cpu>
```

### Examples

```bash
# Parent on CPU 0, child on CPU 1 (different CPUs)
./context_switch 0 1

# Both processes on CPU 0 (same CPU)
./context_switch 0 0
```

## How It Works

1. **Process Creation**: Parent process forks to create a child process
2. **CPU Affinity**: Each process is pinned to a specified CPU core using `sched_setaffinity()`
3. **Pipe Setup**: Two pipes are created for bidirectional communication
4. **Measurement Loop**:
   - Parent writes a byte to child through pipe
   - Child reads the byte and writes it back
   - This forces a context switch each time
5. **Timing**: High-resolution clock measures total time for all iterations
6. **Result**: Average context switch time is calculated and displayed

## Implementation Details

- Uses `_GNU_SOURCE` for CPU affinity support
- Requires `_POSIX_C_SOURCE 199309L` for `clock_gettime()`
- Performs 1000 iterations (configurable via `TIMES` macro)
- Uses `CLOCK_REALTIME` for time measurements

## Understanding the Results

The program outputs the average context switching time in nanoseconds. Typical values:
- Same CPU: 1-5 microseconds (higher due to cache effects)
- Different CPUs: 0.5-3 microseconds (can be faster due to parallel execution)

Actual values depend on:
- CPU architecture and speed
- Operating system scheduler implementation
- System load and other running processes
- CPU cache configuration

## Limitations

- Linux-specific (uses GNU extensions)
- Requires appropriate permissions for CPU affinity
- Results can vary based on system load
- Measures user-space visible latency, not pure kernel overhead

## Future Enhancements

- Add statistical analysis (min/max/standard deviation)
- Support for different IPC mechanisms
- Configurable iteration count
- CSV/JSON output format
- Cross-platform compatibility