# Lock-free MPMC FIFO Queue (HP/EBR/None) + Mutex Baseline

A Multi-Producer/Multi-Consumer (MPMC) lock-free queue implementation based on the **Michael & Scott algorithm**.
This project focuses on solving **memory reclamation (SMR)** and **performance scalability** issues for lock-free data structures in C++ environments.

We implement multiple memory reclamation strategies and introduce optimizations like **Object Pool**, **False Sharing Prevention**, and **Exponential Backoff**, demonstrating superior performance over traditional mutex-based queues under real workloads.

## Core Features and Strategies

- **Core Algorithm**: Michael & Scott Non-blocking Queue
- **Memory Reclamation (SMR)**:
  - `epoch_based_reclamation` (**EBR**): Epoch-based reclamation mechanism, suitable for high-throughput scenarios.
  - `hazard_pointers` (**HP**): Classic Hazard Pointer implementation, providing the strongest lock-free guarantees (wait-free readers).
  - `no_reclamation` (**None**): Control group for benchmarking (Unsafe).
- **Baseline**: `mutex_queue` (based on `std::queue` + `std::mutex`)

## 🚀 Key Performance Optimizations (Technical Innovations)

To break through the physical bottlenecks of lock-free structures, we introduce the following optimizations:

### 1. Lock-Free Object Pool (Memory Pool)
- **Flag**: `LFQ_USE_NODEPOOL=1`
- **Problem**: Standard `new/delete` triggers the System Allocator's Global Lock in multi-threaded environments, causing lock-free queues to bottleneck on malloc locks.
- **Solution**: Implemented `Thread-Local Node Cache`.
  - **Overloading**: Overloaded `Node`'s `operator new/delete` for seamless Object Pool integration with SMR mechanisms.
  - **Batch Size**: Exchange 128 nodes with the global pool at once to reduce lock contention.
  - **Local Capacity**: Each thread's local buffer capacity is 512 nodes.
  - **Effect**: Significantly reduces `malloc` calls and improves Cache Locality.

### 2. False Sharing Prevention
- **Problem**: `Head` and `Tail` pointers in the same Cache Line cause frequent Cache Invalidation (ping-pong effect) between cores.
- **Solution**: Use `alignas(64)` to force alignment to CPU Cache Line boundaries.

### 3. Exponential Backoff
- **Flag**: `LFQ_USE_BACKOFF=1`
- **Problem**: Frequent CAS failures under high contention lead to bus saturation (Bus Storm), reducing overall throughput.
- **Solution**: Introduce exponential backoff, pausing CPU (using `_mm_pause` or `yield`) on CAS failure to alleviate bus pressure.
  - **Max Yield**: Maximum wait cycles of 512, then use `std::this_thread::yield()`.

### 4. Memory Ordering Optimization
- **Solution**: Optimize from conservative `memory_order_seq_cst` to `acquire/release` semantics, reducing CPU Memory Fence overhead.

### 5. Address Sanitizer (ASan) Support
- **Feature**: Auto-detect ASan and enable Poisoning mechanism.
- **Poisoning**: Reclaimed nodes are marked as "poisoned" to prevent Use-After-Free.
- **Unpoisoning**: Allocated nodes are unpoisoned to allow normal access.
- **Macros**: `ASAN_POISON_NODE(ptr, size)` and `ASAN_UNPOISON_NODE(ptr, size)`.

## Build & Run

This project supports feature toggles via Makefile parameters for research purposes.

```bash
# 1. Standard compilation (basic implementation only, no extra optimizations)
# Suitable for observing bottlenecks before optimization
make clean && make ENABLE_POOL=0 ENABLE_BACKOFF=0 -j

# 2. Enable key optimizations
# ENABLE_POOL=1    : Enable Thread-Local Object Pool (solves malloc lock bottleneck)
# ENABLE_BACKOFF=1 : Enable Exponential Backoff (solves bus contention)
make clean && make ENABLE_POOL=1 ENABLE_BACKOFF=1 -j

# 3. Run correctness tests
make run-stress

# 4. Run performance benchmarks
# Example: Using EBR strategy, 4P/4C, Payload 2us
./build/stress_test_pool_backoff --impl ebr --producers 4 --consumers 4 --payload-us 2
```

### Build Options Details

Makefile supports the following macro definitions:
- `LFQ_USE_NODEPOOL`: Enable Object Pool (disabled by default)
- `LFQ_USE_BACKOFF`: Enable Exponential Backoff (disabled by default)

After compilation, multiple executables are generated:
- `build/bench_queue_nopool_nobackoff`: Benchmark (no optimizations)
- `build/bench_queue_pool_nobackoff`: Pool only
- `build/bench_queue_nopool_backoff`: Backoff only  
- `build/bench_queue_pool_backoff`: All optimizations enabled
- `build/stress_test_pool_backoff`: Correctness test (Pool+Backoff enabled by default)
- `build/asan_test`: ASan memory check version

## Implementation Details

### 1. Epoch-Based Reclamation (EBR)
- **Location**: `include/reclaimer/epoch_based_reclamation.hpp`
- **Mechanism**: 
  - Maintains global `Global Epoch` and per-thread `Local Epoch`.
  - Adopts **QSBR (Quiescent-State-Based Reclamation)** principles.
  - **Optimizations**: 
    - Uses `try_lock` for reclamation scans to avoid multi-thread queuing in reclamation logic (Non-blocking reclamation).
    - Batch reclamation threshold set to 512 to amortize scan overhead.

### 2. Hazard Pointers (HP)
- **Location**: `include/reclaimer/hazard_pointers.hpp`
- **Mechanism**: 
  - Each thread maintains `K` Hazard Pointers (typically K=2 for M&S Queue).
  - Marks `Head` during dequeue to prevent reclamation by other threads.
  - **Feature**: Only reclaims nodes when confirmed no Hazard Pointers point to them.

### 3. Object Pool Integration
- **Location**: `include/queue/lockfree_queue.hpp`
- **Design**:
  - `Node` struct overloads `operator new` and `operator delete`.
  - When SMR modules (EBR/HP) call `delete node`, it automatically routes to `NodePool::free()` instead of system `free()`.

## Performance Analysis and Expected Results

Based on our experiments (see report for details):
1.  **Very Low Load (0us Payload)**: 
    - Due to `std::deque` (underlying Mutex) having excellent contiguous memory layout (Cache Locality), Mutex version may be slightly faster than Linked-List based Lock-Free Queue in this extreme scenario.
2.  **Real Load (>= 3us Payload)**:
    - Lock-Free versions show better scalability, with throughput significantly exceeding Mutex versions.

## Testing and Benchmarking

### Correctness Tests
- **Command**: `make run-stress`
- **Content**: 
  - Linearizability checks (Per-Producer FIFO)
  - Shutdown semantics tests
  - ABA problem demonstration (using UnsafeDirectReclamation)

### Performance Benchmarks
- **Command**: `./build/stress_test_pool_backoff --impl [hp|ebr|none|mutex] --producers P --consumers C --payload-us N`
- **Metrics**:
  - Throughput (Producer/Consumer ops/sec)
  - Latency percentiles (P50, P99, P99.9, Max)
  - Peak memory usage
  - Maximum queue depth

### Plotting Script
- **Location**: `scripts/plot_results.py`
- **Function**: Generate performance comparison charts from CSV results

## Usage Example

```cpp
#include "queue/lockfree_queue.hpp"
#include "reclaimer/epoch_based_reclamation.hpp"

// Define a queue using EBR
using EBRQueue = mpmcq::LockFreeQueue<int, mpmcq::reclaimer::epoch_based_reclamation>;

int main() {
    EBRQueue q;
    
    // Producer
    std::thread p([&]{
        q.enqueue(42);
    });

    // Consumer
    std::thread c([&]{
        int v;
        if (q.try_dequeue(v)) {
            // Process...
        }
        // Important: Periodically declare Quiescent State to drive reclamation
        EBRQueue::quiescent(); 
    });
    
    p.join(); c.join();
}
```

## Project Structure

```
include/
  queue/
    lockfree_queue.hpp      # M&S Lock-Free Queue (with NodePool, Backoff, ASan)
    mutex_queue.hpp         # Mutex-based Baseline
  reclaimer/
    hazard_pointers.hpp          # Hazard Pointers strategy (HP_COUNT_PER_THREAD=2)
    epoch_based_reclamation.hpp  # Epoch-Based Reclamation (EBR_RETIRE_THRESHOLD=512)
    no_reclamation.hpp           # No Reclamation strategy (Memory Leak)

src/
  benchmark_main.cpp         # Performance benchmarking tool
  tests_correctness_main.cpp # Correctness tests (linearizability, ABA demo)

scripts/
  plot_results.py           # Result visualization script
  run_matrix.sh             # Batch testing script

Makefile                    # Build configuration (supports multiple optimization combinations)
README.md                   # This file
```
