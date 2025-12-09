# Minimal Makefile: build two executables
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O3 -pthread -latomic
INCLUDES := -Iinclude

# 用戶可選參數，預設為 0 (不開啟)
# 使用方式: make ENABLE_POOL=1 ENABLE_BACKOFF=1
ENABLE_POOL    ?= 0
ENABLE_BACKOFF ?= 0
# ENABLE_DEPTH   ?= 0

# 根據參數追加 CXXFLAGS
ifeq ($(ENABLE_POOL), 1)
	CXXFLAGS += -DLFQ_USE_NODEPOOL
endif

ifeq ($(ENABLE_BACKOFF), 1)
	CXXFLAGS += -DLFQ_USE_BACKOFF
endif

# ifeq ($(ENABLE_DEPTH), 1)
# 	CXXFLAGS += -DBENCH_USE_DEPTH
# endif

BIN       := build
RESULT    := results
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
	$(BENCH_BIN) --help || true

run-tests: $(TESTS_BIN)
	$(TESTS_BIN)

clean:
	rm -rf $(BIN)
	rm -rf $(RESULT)