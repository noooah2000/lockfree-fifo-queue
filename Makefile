# ==========================================
# Settings
# ==========================================
CXX      		:= g++
CXXFLAGS_COMMON := -std=c++20 -pthread -Iinclude -Wall -Wextra

# 巨集定義
DEF_NOPOOL := 
DEF_POOL   := -DLFQ_USE_NODEPOOL
DEF_BACKOFF:= -DLFQ_USE_BACKOFF

# 路徑
BIN       := build
RESULT    := results
BENCH_SRC := src/benchmark_main.cpp
TESTS_SRC := src/tests_correctness_main.cpp

# ==========================================
# Targets Definition
# ==========================================

# 1. Benchmark Executables
BENCH_BIN_NOPOOL := $(BIN)/bench_queue_nopool
BENCH_BIN_POOL   := $(BIN)/bench_queue_pool

# 2. Stress Executables (Release Mode)
STRESS_BIN_NOPOOL := $(BIN)/stress_test_nopool
STRESS_BIN_POOL   := $(BIN)/stress_test_pool

# 3. ASan Executables (Debug + Sanitizer)
ASAN_BIN_NOPOOL   := $(BIN)/asan_test_nopool
ASAN_BIN_POOL     := $(BIN)/asan_test_pool

# Flags
CXXFLAGS_REL  := $(CXXFLAGS_COMMON) $(DEF_BACKOFF) -O3 -DNDEBUG -latomic
CXXFLAGS_ASAN := $(CXXFLAGS_COMMON) $(DEF_BACKOFF) -O1 -g -fsanitize=address -fno-omit-frame-pointer -latomic

.PHONY: all clean help dirs \
        run-bench-all \
        run-stress run-stress-pool \
        run-asan run-asan-pool

# 預設編譯全部 (6個執行檔)
all: dirs \
     $(BENCH_BIN_NOPOOL) $(BENCH_BIN_POOL) \
     $(STRESS_BIN_NOPOOL) $(STRESS_BIN_POOL) \
     $(ASAN_BIN_NOPOOL) $(ASAN_BIN_POOL)

dirs:
	@mkdir -p $(BIN)
	@mkdir -p $(RESULT)

# ==========================================
# Build Rules
# ==========================================

# --- Benchmark ---
$(BENCH_BIN_NOPOOL): $(BENCH_SRC)
	@echo "Building Bench (No Pool)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_NOPOOL) $< -o $@

$(BENCH_BIN_POOL): $(BENCH_SRC)
	@echo "Building Bench (With Pool)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_POOL) $< -o $@

# --- Stress ---
$(STRESS_BIN_NOPOOL): $(TESTS_SRC)
	@echo "Building Stress (No Pool)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_NOPOOL) $< -o $@

$(STRESS_BIN_POOL): $(TESTS_SRC)
	@echo "Building Stress (With Pool)..."
	@$(CXX) $(CXXFLAGS_REL) $(DEF_POOL) $< -o $@

# --- ASan ---
$(ASAN_BIN_NOPOOL): $(TESTS_SRC)
	@echo "Building ASan (No Pool)..."
	@$(CXX) $(CXXFLAGS_ASAN) $(DEF_NOPOOL) $< -o $@

$(ASAN_BIN_POOL): $(TESTS_SRC)
	@echo "Building ASan (With Pool)..."
	@$(CXX) $(CXXFLAGS_ASAN) $(DEF_POOL) $< -o $@

# ==========================================
# Run Helper Commands
# ==========================================

# 跑壓力測試 (預設 NoPool)
run-stress: dirs \
	$(STRESS_BIN_NOPOOL)
	@echo ">>> Running Stress Test (Direct New/Delete)..."
	@$(STRESS_BIN_NOPOOL)

# 跑壓力測試 (Pool)
run-stress-pool: dirs \
	$(STRESS_BIN_POOL)
	@echo ">>> Running Stress Test (Object Pool)..."
	@$(STRESS_BIN_POOL)

# 跑 ASan (預設 NoPool)
run-asan: dirs \
	$(ASAN_BIN_NOPOOL)
	@echo ">>> Running ASan Test (No Pool)..."
	@ulimit -v unlimited && $(ASAN_BIN_NOPOOL)

# 跑 ASan (Pool)
run-asan-pool: dirs \
	$(ASAN_BIN_POOL)
	@echo ">>> Running ASan Test (Object Pool)..."
	@ulimit -v unlimited && $(ASAN_BIN_POOL)

clean:
	rm -rf $(BIN)
	rm -rf $(RESULT)

help:
	@echo "Build Targets:"
	@echo "  make all              : Build all 6 executables"
	@echo ""
	@echo "Run Targets (Auto-build if needed):"
	@echo "  make run-stress       : Run Stress Test (No Pool)"
	@echo "  make run-stress-pool  : Run Stress Test (With Pool)"
	@echo "  make run-asan         : Run ASan Test (No Pool)"
	@echo "  make run-asan-pool    : Run ASan Test (With Pool)"
	@echo ""
	@echo "Benchmark Script:"
	@echo "  ./run_bench.sh        : Run full benchmark suite (Pool vs NoPool)"