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
- **Flag**: `ENABLE_POOL=1`
- **問題**: 標準 `new/delete` 在多執行緒下會觸發 System Allocator 的 Global Lock，導致 Lock-Free Queue 實際上卡在 Malloc 鎖上。
- **解法**: 實作了 `Thread-Local Node Cache`。
  - **Overloading**: 重載了 `Node` 的 `operator new/delete`，使 SMR 機制無痛整合 Object Pool。
  - **效果**: 大幅減少 `malloc` 呼叫，並提升 Cache Locality。

### 2. 防止 False Sharing (偽共享)
- **問題**: `Head` 和 `Tail` 指標若位於同一 Cache Line，會導致核心間頻繁的 Cache Invalidation (Ping-pong effect)。
- **解法**: 使用 `alignas(64)` 強制對齊 CPU Cache Line。

### 3. Exponential Backoff (指數退避)
- **Flag**: `ENABLE_BACKOFF=1`
- **問題**: 在高競爭 (High Contention) 下，頻繁的 CAS 失敗會導致匯流排飽和 (Bus Storm)，降低總吞吐量。
- **解法**: 引入指數退避機制，在 CAS 失敗時暫停 CPU (使用 `_mm_pause` 或 `yield`)，緩解匯流排壓力。

### 4. 記憶體順序優化 (Memory Ordering)
- **解法**: 將原本保守的 `memory_order_seq_cst` 優化為 `acquire/release` 語意，減少 CPU 的 Memory Fence 開銷。

## Build & Run

本專案支援透過 Makefile 參數進行特性開關，方便進行研究。

```bash
# 1. 標準編譯 (僅基礎實作，無額外優化)
# 適合用來觀察未優化前的瓶頸
make clean && make -j

# 2. 開啟關鍵優化 (推薦用於效能競賽)
# ENABLE_POOL=1    : 啟用 Thread-Local Object Pool (解決 malloc 鎖瓶頸)
# ENABLE_BACKOFF=1 : 啟用 Exponential Backoff (解決匯流排競爭)
make clean && make ENABLE_POOL=1 ENABLE_BACKOFF=1 -j

# 3. 執行正確性測試
make run-tests

# 4. 執行效能基準測試
# 範例: 使用 EBR 策略, 4P/4C, Payload 2us
./build/bench_queue --impl ebr --producers 4 --consumers 4 --payload-us 2
```

## 實現詳細說明

### 1. Epoch-Based Reclamation (EBR)
- **位置**: `include/reclaimer/epoch_based_reclamation.hpp`
- **機制**: 
  - 維護全域 `Global Epoch` 與每執行緒 `Local Epoch`。
  - 採用 **QSBR (Quiescent-State-Based Reclamation)** 精神。
  - **優化**: 
    - 採用 `try_lock` 進行回收掃描，避免多執行緒在回收邏輯上排隊 (Non-blocking reclamation)。
    - 批次回收閾值設為 4096，均攤掃描開銷。

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
    - 這是硬體物理限制 (Pointer Chasing vs Array Access)。
2.  **真實負載 (>= 2us Payload)**:
    - Lock-Free (EBR) 版本展現出優異的擴展性 (Scalability)，吞吐量顯著超越 Mutex 版本。
    - 在高併發 (20+ Threads) 下，Lock-Free 的延遲抖動 (Jitter) 遠低於 Mutex。

## 使用範例

```cpp
#include "queue/lockfree_queue.hpp"
#include "reclaimer/epoch_based_reclamation.hpp"

// 定義一個使用 EBR 的佇列
using EBRQueue = lfq::MPMCQueue<int, lfq::reclaimer::epoch_based_reclamation>;

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
    lockfree_queue.hpp      # M&S Queue (包含 NodePool, Backoff)
    mutex_queue.hpp         # Baseline
  reclaimer/
    hazard_pointers.hpp     # HP 策略
    epoch_based_reclamation.hpp  # EBR 策略 (Optimized)
    no_reclamation.hpp      # None 策略

src/
  benchmark_main.cpp        # 壓力測試工具
  tests_correctness_main.cpp # GoogleTest / Basic Assertions
```
