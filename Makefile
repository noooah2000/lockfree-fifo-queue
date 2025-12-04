# Minimal Makefile: build two executables
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -pthread
INCLUDES := -Iinclude

BIN       := build
BENCH_SRC := src/benchmark_main.cpp
TESTS_SRC := src/tests_correctness_main.cpp
BENCH_BIN := $(BIN)/bench_queue
TESTS_BIN := $(BIN)/tests_correctness

.PHONY: all run-bench run-tests clean

all: $(BENCH_BIN) $(TESTS_BIN)

$(BIN):
	mkdir -p $(BIN)

$(BENCH_BIN): $(BENCH_SRC) | $(BIN)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(TESTS_BIN): $(TESTS_SRC) | $(BIN)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

run-bench: $(BENCH_BIN)
	$(BENCH_BIN)

run-tests: $(TESTS_BIN)
	$(TESTS_BIN)

clean:
	rm -rf $(BIN)
