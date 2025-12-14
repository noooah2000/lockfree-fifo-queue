# ==========================================
# Settings
# ==========================================
CXX             := g++
CXXFLAGS_COMMON := -std=c++20 -pthread -Iinclude -Wall -Wextra

# 巨集定義
DEF_NOPOOL  := 
DEF_POOL    := -DLFQ_USE_NODEPOOL
DEF_BACKOFF := -DLFQ_USE_BACKOFF

# 路徑
BIN       := build
RESULT    := results
BENCH_SRC := src/benchmark_main.cpp
TESTS_SRC := src/tests_correctness_main.cpp

# ==========================================
# OS Detection & Library Settings
# ==========================================
UNAME_S := $(shell uname -s)

# 預設不加 -latomic (適用 macOS)
ATOMIC_LIB :=

# 如果是 Linux，則加入 -latomic
ifeq ($(UNAME_S),Linux)
    ATOMIC_LIB := -latomic
endif

# Flags
# 注意：這裡將原本寫死的 -latomic 改成了變數 $(ATOMIC_LIB)
CXXFLAGS_REL  := $(CXXFLAGS_COMMON) -O3 -DNDEBUG $(ATOMIC_LIB)
CXXFLAGS_ASAN := $(CXXFLAGS_COMMON) $(DEF_BACKOFF) -O1 -g -fsanitize=address -fno-omit-frame-pointer $(ATOMIC_LIB)

# ==========================================
# Targets Definition
# ==========================================

# 1. Benchmark Executables (4 Combinations)
BENCH_BIN_NOPOOL_NOBO := $(BIN)/bench_queue_nopool_nobackoff
BENCH_BIN_POOL_NOBO   := $(BIN)/bench_queue_pool_nobackoff
BENCH_BIN_NOPOOL_BO   := $(BIN)/bench_queue_nopool_backoff
BENCH_BIN_POOL_BO     := $(BIN)/bench_queue_pool_backoff

# 2. Stress Executables (Release Mode - Default to Pool + Backoff for best stress)
STRESS_BIN := $(BIN)/stress_test_pool_backoff

# 3. ASan Executables (Debug + Sanitizer)
ASAN_BIN := $(BIN)/asan_test

.PHONY: all clean help dirs \
        run-stress run-asan

# 預設編譯全部
all: dirs \
     $(BENCH_BIN_NOPOOL_NOBO) $(BENCH_BIN_POOL_NOBO) \
     $(BENCH_BIN_NOPOOL_BO)   $(BENCH_BIN_POOL_BO) \
     $(STRESS_BIN) $(ASAN_BIN)

dirs:
	@mkdir -p $(BIN)
	@mkdir -p $(RESULT)

# ==========================================
# Build Rules
# ==========================================

# --- Benchmark (4 Combinations) ---

# 1. No Pool, No Backoff
$(BENCH_BIN_NOPOOL_NOBO): $(BENCH_SRC)
	@echo "Building Bench (No Pool, No Backoff)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_NOPOOL) $< -o $@

# 2. Pool, No Backoff
$(BENCH_BIN_POOL_NOBO): $(BENCH_SRC)
	@echo "Building Bench (Pool, No Backoff)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_POOL) $< -o $@

# 3. No Pool, With Backoff
$(BENCH_BIN_NOPOOL_BO): $(BENCH_SRC)
	@echo "Building Bench (No Pool, With Backoff)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_NOPOOL) $(DEF_BACKOFF) $< -o $@

# 4. Pool, With Backoff
$(BENCH_BIN_POOL_BO): $(BENCH_SRC)
	@echo "Building Bench (Pool, With Backoff)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_POOL) $(DEF_BACKOFF) $< -o $@


# --- Stress (Pool + Backoff) ---
$(STRESS_BIN): $(TESTS_SRC)
	@echo "Building Stress Test (Pool + Backoff)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_POOL) $(DEF_BACKOFF) $< -o $@

# --- ASan (Pool + Backoff) ---
$(ASAN_BIN): $(TESTS_SRC)
	@echo "Building ASan Test..."
	@$(CXX) $(CXXFLAGS_ASAN) $(DEF_POOL) $< -o $@

# ==========================================
# Run Helper Commands
# ==========================================

run-stress: dirs $(STRESS_BIN)
	@echo ">>> Running Stress Test..."
	@$(STRESS_BIN)

run-asan: dirs $(ASAN_BIN)
	@echo ">>> Running ASan Test..."
	# macOS 上 ulimit -v 常常無效或不可設，為了相容性這裡加上 || true
	@ulimit -v unlimited 2>/dev/null || true && $(ASAN_BIN)

clean:
	rm -rf $(BIN)
	rm -rf $(RESULT)

help:
	@echo "Build Targets:"
	@echo "  make all              : Build all executables"
	@echo "  make clean            : Clean build files"