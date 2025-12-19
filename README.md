# Lock-free MPMC FIFO Queue (HP/EBR/None) + Mutex Baseline

基於 **Michael & Scott 演算法** 的 Multi-Producer/Multi-Consumer (MPMC) 無鎖佇列實作。
本專案專注於解決 Lock-Free 資料結構在 C++ 環境下的 **記憶體回收 (SMR)** 與 **效能擴展性** 問題。

我們實作了多種記憶體回收策略，並引入了 **Object Pool**、**False Sharing Prevention** 與 **Exponential Backoff** 等優化技術，在真實負載下展現出優於傳統 Mutex 的效能。

## 核心功能與策略

- **Core Algorithm**: Michael & Scott Non-blocking Queue
- **Memory Reclamation (SMR)**:
  - `epoch_based_reclamation` (**EBR**): 基於 Epoch 的回收機制，適合高吞吐量場景。
  - `hazard_pointers` (**HP**): 經典的 Hazard Pointer 實作，提供最強的無鎖保證 (Wait-free readers)。
  - `no_reclamation` (**None**): 用於基準測試的對照組 (Unsafe)。
- **Baseline**: `mutex_queue` (基於 `std::queue` + `std::mutex`)

## 🚀 關鍵效能優化 (Technical Innovations)

為了突破 Lock-Free 的物理瓶頸，我們引入了以下優化：

### 1. Lock-Free Object Pool (記憶體池)
- **Flag**: `LFQ_USE_NODEPOOL=1`
- **問題**: 標準 `new/delete` 在多執行緒下會觸發 System Allocator 的 Global Lock，導致 Lock-Free Queue 實際上卡在 Malloc 鎖上。
- **解法**: 實作了 `Thread-Local Node Cache`。
  - **Overloading**: 重載了 `Node` 的 `operator new/delete`，使 SMR 機制無痛整合 Object Pool。
  - **Batch Size**: 一次與全域池交換 128 個節點，減少鎖競爭。
  - **Local Capacity**: 每個執行緒本地緩衝區容量 512 個節點。
  - **效果**: 大幅減少 `malloc` 呼叫，並提升 Cache Locality。

### 2. 防止 False Sharing (偽共享)
- **問題**: `Head` 和 `Tail` 指標若位於同一 Cache Line，會導致核心間頻繁的 Cache Invalidation (Ping-pong effect)。
- **解法**: 使用 `alignas(64)` 強制對齊 CPU Cache Line。

### 3. Exponential Backoff (指數退避)
- **Flag**: `LFQ_USE_BACKOFF=1`
- **問題**: 在高競爭 (High Contention) 下，頻繁的 CAS 失敗會導致匯流排飽和 (Bus Storm)，降低總吞吐量。
- **解法**: 引入指數退避機制，在 CAS 失敗時暫停 CPU (使用 `_mm_pause` 或 `yield`)，緩解匯流排壓力。
  - **Max Yield**: 最大等待次數 512，超過則使用 `std::this_thread::yield()`。

### 4. 記憶體順序優化 (Memory Ordering)
- **解法**: 將原本保守的 `memory_order_seq_cst` 優化為 `acquire/release` 語意，減少 CPU 的 Memory Fence 開銷。

### 5. Address Sanitizer (ASan) 支援
- **功能**: 自動偵測 ASan 並啟用 Poisoning 機制。
- **Poisoning**: 回收的節點會被標記為 "有毒"，防止 Use-After-Free。
- **Unpoisoning**: 分配的節點會被解除毒性，允許正常存取。
- **巨集**: `ASAN_POISON_NODE(ptr, size)` 和 `ASAN_UNPOISON_NODE(ptr, size)`。

## Build & Run

本專案支援透過 Makefile 參數進行特性開關，方便進行研究。

```bash
# 1. 標準編譯 (僅基礎實作，無額外優化)
# 適合用來觀察未優化前的瓶頸
make clean && make ENABLE_POOL=0 ENABLE_BACKOFF=0 -j

# 2. 開啟關鍵優化
# ENABLE_POOL=1    : 啟用 Thread-Local Object Pool (解決 malloc 鎖瓶頸)
# ENABLE_BACKOFF=1 : 啟用 Exponential Backoff (解決匯流排競爭)
make clean && make ENABLE_POOL=1 ENABLE_BACKOFF=1 -j

# 3. 執行正確性測試
make run-stress

# 4. 執行效能基準測試
# 範例: 使用 EBR 策略, 4P/4C, Payload 2us
./build/stress_test_pool_backoff --impl ebr --producers 4 --consumers 4 --payload-us 2
```

### 建構選項詳解

Makefile 支援以下巨集定義：
- `LFQ_USE_NODEPOOL`: 啟用 Object Pool (預設關閉)
- `LFQ_USE_BACKOFF`: 啟用 Exponential Backoff (預設關閉)

編譯後會產生多個執行檔：
- `build/bench_queue_nopool_nobackoff`: 基準測試 (無優化)
- `build/bench_queue_pool_nobackoff`: 僅啟用 Pool
- `build/bench_queue_nopool_backoff`: 僅啟用 Backoff  
- `build/bench_queue_pool_backoff`: 啟用所有優化
- `build/stress_test_pool_backoff`: 正確性測試 (預設啟用 Pool+Backoff)
- `build/asan_test`: ASan 記憶體檢查版本

## 實現詳細說明

### 1. Epoch-Based Reclamation (EBR)
- **位置**: `include/reclaimer/epoch_based_reclamation.hpp`
- **機制**: 
  - 維護全域 `Global Epoch` 與每執行緒 `Local Epoch`。
  - 採用 **QSBR (Quiescent-State-Based Reclamation)** 精神。
  - **優化**: 
    - 採用 `try_lock` 進行回收掃描，避免多執行緒在回收邏輯上排隊 (Non-blocking reclamation)。
    - 批次回收閾值設為 512，均攤掃描開銷。

### 2. Hazard Pointers (HP)
- **位置**: `include/reclaimer/hazard_pointers.hpp`
- **機制**: 
  - 每個執行緒維護 `K` 個 Hazard Pointers (通常 K=2 for M&S Queue)。
  - Dequeue 時標記 `Head`，防止被其他執行緒回收。
  - **特性**: 只有在確認無任何 Hazard Pointer 指向該節點時才回收。

### 3. Object Pool Integration
- **位置**: `include/queue/lockfree_queue.hpp`
- **設計**:
  - `Node` 結構體重載了 `operator new` 與 `operator delete`。
  - SMR 模組 (EBR/HP) 呼叫 `delete node` 時，會自動導向 `NodePool::free()` 而非系統 `free()`。

## 效能分析與預期結果

根據我們的實驗 (詳見報告)：
1.  **極低負載 (0us Payload)**: 
    - 由於 `std::deque` (Mutex底層) 擁有極佳的連續記憶體佈局 (Cache Locality)，在此極端場景下 Mutex 版本可能略快於 Linked-List 結構的 Lock-Free Queue。
2.  **真實負載 (>= 3us Payload)**:
    - Lock-Free 版本展現出較佳的擴展性 (Scalability)，吞吐量顯著超越 Mutex 版本。

## 測試與基準測試

### 正確性測試
- **命令**: `make run-stress`
- **內容**: 
  - 線性一致性檢查 (Per-Producer FIFO)
  - 關閉語意測試
  - ABA 問題演示 (使用 UnsafeDirectReclamation)

### 效能基準測試
- **命令**: `./build/stress_test_pool_backoff --impl [hp|ebr|none|mutex] --producers P --consumers C --payload-us N`
- **指標**:
  - 吞吐量 (Producer/Consumer ops/sec)
  - 延遲分位數 (P50, P99, P99.9, Max)
  - 記憶體峰值使用量
  - 隊列最大深度

### 繪圖腳本
- **位置**: `scripts/plot_results.py`
- **功能**: 從 CSV 結果生成效能比較圖表

## 使用範例

```cpp
#include "queue/lockfree_queue.hpp"
#include "reclaimer/epoch_based_reclamation.hpp"

// 定義一個使用 EBR 的佇列
using EBRQueue = mpmcq::LockFreeQueue<int, mpmcq::reclaimer::epoch_based_reclamation>;

int main() {
    EBRQueue q;
    
    // 生產者
    std::thread p([&]{
        q.enqueue(42);
    });

    // 消費者
    std::thread c([&]{
        int v;
        if (q.try_dequeue(v)) {
            // Process...
        }
        // 重要：定期宣告 Quiescent State 以驅動回收
        EBRQueue::quiescent(); 
    });
    
    p.join(); c.join();
}
```

## 文件結構

```
include/
  queue/
    lockfree_queue.hpp      # M&S Lock-Free Queue (包含 NodePool, Backoff, ASan)
    mutex_queue.hpp         # Mutex-based Baseline
  reclaimer/
    hazard_pointers.hpp          # Hazard Pointers 策略 (HP_COUNT_PER_THREAD=2)
    epoch_based_reclamation.hpp  # Epoch-Based Reclamation (EBR_RETIRE_THRESHOLD=512)
    no_reclamation.hpp           # No Reclamation 策略 (Memory Leak)

src/
  benchmark_main.cpp         # 效能基準測試工具
  tests_correctness_main.cpp # 正確性測試 (線性一致性, ABA 演示)

scripts/
  plot_results.py           # 結果視覺化腳本
  run_matrix.sh             # 批量測試腳本

Makefile                    # 建構配置 (支援多種優化組合)
README.md                   # 本文件
```
