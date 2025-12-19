# ==============================================================================
# Build Configuration
# ==============================================================================

# Compiler Settings
CXX             := g++
CXXFLAGS_COMMON := -std=c++20 -pthread -Iinclude -Wall -Wextra

# Feature Macros
# Control internal implementations (Memory Pool, Exponential Backoff)
DEF_NOPOOL  := 
DEF_POOL    := -DLFQ_USE_NODEPOOL
DEF_BACKOFF := -DLFQ_USE_BACKOFF

# Directory Structure
BIN       := build
RESULT    := results
BENCH_SRC := src/benchmark_main.cpp
TESTS_SRC := src/tests_correctness_main.cpp

# ==============================================================================
# Platform Detection
# ==============================================================================
UNAME_S := $(shell uname -s)

# Library Dependencies
# atomic library is usually required on Linux for std::atomic, but often implicit on macOS.
ATOMIC_LIB :=
ifeq ($(UNAME_S),Linux)
    ATOMIC_LIB := -latomic
endif

# Build Flags
# Release: Maximum optimization, assertions disabled.
CXXFLAGS_REL  := $(CXXFLAGS_COMMON) -O3 -DNDEBUG $(ATOMIC_LIB)

# ASan: Debug info enabled, AddressSanitizer enabled for memory error detection.
CXXFLAGS_ASAN := $(CXXFLAGS_COMMON) $(DEF_BACKOFF) -O1 -g -fsanitize=address -fno-omit-frame-pointer $(ATOMIC_LIB)

# ==============================================================================
# Target Definitions
# ==============================================================================

# 1. Benchmark Variants
# We generate 4 binaries to isolate the performance impact of Memory Pool and Backoff strategies.
BENCH_BIN_NOPOOL_NOBO := $(BIN)/bench_queue_nopool_nobackoff
BENCH_BIN_POOL_NOBO   := $(BIN)/bench_queue_pool_nobackoff
BENCH_BIN_NOPOOL_BO   := $(BIN)/bench_queue_nopool_backoff
BENCH_BIN_POOL_BO     := $(BIN)/bench_queue_pool_backoff

# 2. Stress Test
# Uses the optimized configuration (Pool + Backoff) for heavy concurrency validation.
STRESS_BIN := $(BIN)/stress_test_pool_backoff

# 3. Sanitizer Test
# Used for detecting memory leaks and race conditions.
ASAN_BIN := $(BIN)/asan_test

.PHONY: all clean help dirs run-stress run-asan

# Default Target: Build all artifacts
all: dirs \
     $(BENCH_BIN_NOPOOL_NOBO) $(BENCH_BIN_POOL_NOBO) \
     $(BENCH_BIN_NOPOOL_BO)   $(BENCH_BIN_POOL_BO) \
     $(STRESS_BIN) $(ASAN_BIN)

dirs:
	@mkdir -p $(BIN)
	@mkdir -p $(RESULT)

# ==============================================================================
# Compilation Rules
# ==============================================================================

# --- Benchmark Configurations ---

# Variant 1: Baseline (No Pool, No Backoff)
$(BENCH_BIN_NOPOOL_NOBO): $(BENCH_SRC)
	@echo "Building Bench (No Pool, No Backoff)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_NOPOOL) $< -o $@

# Variant 2: Pool Only
$(BENCH_BIN_POOL_NOBO): $(BENCH_SRC)
	@echo "Building Bench (Pool, No Backoff)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_POOL) $< -o $@

# Variant 3: Backoff Only
$(BENCH_BIN_NOPOOL_BO): $(BENCH_SRC)
	@echo "Building Bench (No Pool, With Backoff)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_NOPOOL) $(DEF_BACKOFF) $< -o $@

# Variant 4: Optimized (Pool + Backoff)
$(BENCH_BIN_POOL_BO): $(BENCH_SRC)
	@echo "Building Bench (Pool, With Backoff)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_POOL) $(DEF_BACKOFF) $< -o $@


# --- Correctness & Sanitizers ---

# Stress Test (Optimized Config)
$(STRESS_BIN): $(TESTS_SRC)
	@echo "Building Stress Test (Pool + Backoff)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_POOL) $(DEF_BACKOFF) $< -o $@

# AddressSanitizer Test
$(ASAN_BIN): $(TESTS_SRC)
	@echo "Building ASan Test..."
	@$(CXX) $(CXXFLAGS_ASAN) $(DEF_POOL) $< -o $@

# ==============================================================================
# Utility Commands
# ==============================================================================

run-stress: dirs $(STRESS_BIN)
	@echo ">>> Running Stress Test..."
	@$(STRESS_BIN)

run-asan: dirs $(ASAN_BIN)
	@echo ">>> Running ASan Test..."
	# ASan often requires unlimited virtual memory. 
	# The '|| true' ensures compatibility with environments where ulimit cannot be changed.
	@ulimit -v unlimited 2>/dev/null || true && $(ASAN_BIN)

clean:
	rm -rf $(BIN)
	rm -rf $(RESULT)

help:
	@echo "Available Targets:"
	@echo "  make all        : Build all executables (Benchmarks, Stress, ASan)"
	@echo "  make run-stress : Build and run the stress test"
	@echo "  make run-asan   : Build and run the AddressSanitizer test"
	@echo "  make clean      : Remove build artifacts and results"