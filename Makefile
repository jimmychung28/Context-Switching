# Operating System Performance Analysis Tools Makefile

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O2
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
LDFLAGS = -lpthread -lm

# Binaries
BINARIES = context_switch_macos scheduler_analyzer lock_visualizer vm_explorer vm_explorer_cpp memory_allocator memory_allocator_cpp

.PHONY: all clean

all: $(BINARIES)

# Context switch measurement (macOS version)
context_switch_macos: src/context_switch_macos.c
	$(CC) $(CFLAGS) -o $@ $<

# Scheduler fairness analyzer
scheduler_analyzer: src/scheduler_analyzer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Lock contention visualizer
lock_visualizer: src/lock_contention_visualizer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Virtual memory explorer (C version)
vm_explorer: src/vm_explorer.c
	$(CC) $(CFLAGS) -o $@ $< -lm

# Virtual memory explorer (C++ version)
vm_explorer_cpp: src/vm_explorer.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -pthread

# Memory allocator benchmarker
memory_allocator: src/memory_allocator_benchmarker.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Memory allocator benchmarker (C++ version)
memory_allocator_cpp: src/memory_allocator_benchmarker.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -pthread

# Linux-specific context switch measurement
context_switch: src/Cost\ of\ Context\ Switching.c
	$(CC) $(CFLAGS) -o $@ "$<" -lrt

clean:
	rm -f $(BINARIES) context_switch

install: all
	@echo "Built tools:"
	@ls -la $(BINARIES)
	@echo ""
	@echo "Usage examples:"
	@echo "  ./context_switch_macos"
	@echo "  ./scheduler_analyzer -d 5 -c 2 -i 2"
	@echo "  ./lock_visualizer -t 8 -l mutex"
	@echo "  ./vm_explorer -t all"
	@echo "  ./vm_explorer_cpp -t tlb -s 256"
	@echo "  ./memory_allocator -t all"
	@echo "  ./memory_allocator_cpp -t container"

help:
	@echo "Available targets:"
	@echo "  all              - Build all tools"
	@echo "  clean            - Remove built binaries"
	@echo "  install          - Build and show usage"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Individual tools:"
	@echo "  context_switch_macos   - Context switching measurement"
	@echo "  scheduler_analyzer     - Scheduler fairness analysis"
	@echo "  lock_visualizer        - Lock contention analysis"
	@echo "  vm_explorer           - Virtual memory analysis (C)"
	@echo "  vm_explorer_cpp       - Virtual memory analysis (C++)"
	@echo "  memory_allocator      - Memory allocator benchmarking (C)"
	@echo "  memory_allocator_cpp  - Memory allocator benchmarking (C++)"