#include <atomic>
#include <thread>
#include <vector>
#include <set>
#include <map>
#include <iostream>
#include <cassert>
#include <string>
#include <chrono>

#include "queue/lockfree_queue.hpp"
#include "queue/mutex_queue.hpp"
#include "reclaimer/hazard_pointers.hpp"
#include "reclaimer/epoch_based_reclamation.hpp"
#include "reclaimer/no_reclamation.hpp"

// ==========================================
//   測試輔助與配置
// ==========================================

// [ABA 測試專用] 一個極度危險的回收器
// 它會立即 delete 節點，搭配 Object Pool 極易引發 ABA 問題
struct UnsafeDirectReclamation
{
    struct token
    {
    };
    static void quiescent() noexcept {}
    static token enter() noexcept { return {}; }
    static void protect_at(int, void *) {}

    template <typename Node>
    static void retire(Node *p) noexcept
    {
        // 直接刪除，讓 NodePool 立即回收該記憶體位址
        // 在高並發下，這個位址會迅速被 allocate 出來，引發 ABA
        delete p;
    }
};

// ==========================================
//   測試邏輯
// ==========================================

// 使用 Template 進行測試，Element Type 固定為 pair<int, int>
template <class QueueType>
static bool test_linearization(const std::string &test_name, int num_producers = 4, int num_consumers = 4, int ops_per_producer = 20000)
{
    using ElementType = std::pair<int, int>;
    QueueType queue;
    std::atomic<int> producers_finished{0};
    std::vector<std::thread> producer_threads, consumer_threads;

    std::cout << "  -> Running " << test_name << " [P:" << num_producers << ", C:" << num_consumers << ", Ops:" << ops_per_producer << "] ... " << std::flush;

    auto start_time = std::chrono::high_resolution_clock::now();

    // Producers: 產生 (producer_id, sequence)
    for (int p = 0; p < num_producers; p++)
    {
        producer_threads.emplace_back([&, p]
                                      {
            for (int i = 0; i < ops_per_producer; i++) 
            { 
                // Spin until enqueue succeeds
                while (!queue.enqueue({p, i})) {
                    // Backoff 稍微讓出 CPU，增加交錯機會
                    std::this_thread::yield(); 
                }
                
                // 定期呼叫 quiescent (模擬 EBR 需要的行為)
                if (i % 64 == 0) QueueType::quiescent(); 
            }
            if (producers_finished.fetch_add(1) + 1 == num_producers) 
            {
                queue.close();
            } });
    }

    // 驗證邏輯：記錄每個 Producer 最後被消費到的序號
    std::vector<std::map<int, int>> consumer_seen(num_consumers);
    std::atomic<int> total_dequeued{0};
    std::atomic<bool> violation_found{false};

    for (int c = 0; c < num_consumers; c++)
    {
        consumer_threads.emplace_back([&, c]
                                      {
            ElementType value;
            int ops_count = 0;
            while (!violation_found.load(std::memory_order_relaxed)) 
            {
                if (queue.try_dequeue(value)) 
                {
                    total_dequeued.fetch_add(1, std::memory_order_relaxed);
                    ops_count++;

                    // 檢查線性一致性 (FIFO per producer)
                    auto& seen_map = consumer_seen[c];
                    // 注意：這裡簡化了檢查，如果要嚴格檢查全域 FIFO，需要更複雜的重組邏輯
                    // 這裡主要檢查「單一消費者」是否看到「單一生產者」的亂序
                    // (在多消費者模型中，單一消費者可能看到跳號，但絕對不能看到逆序)
                    
                    auto it = seen_map.find(value.first);
                    int last_seq = (it == seen_map.end() ? -1 : it->second);

                    if (value.second < last_seq) 
                    {
                        std::cerr << "\n[FAIL] Order violation! Producer=" << value.first 
                                  << " Got=" << value.second 
                                  << " LastSeen=" << last_seq << "\n";
                        violation_found = true;
                    }
                    seen_map[value.first] = value.second;

                    if (ops_count % 64 == 0) QueueType::quiescent();
                } 
                else if (queue.is_closed()) 
                {
                    // 再次確認是否真的空了
                    if (!queue.try_dequeue(value)) break;
                    // 如果還能拿，繼續迴圈 (處理上面那個 try_dequeue 成功的情況)
                    // 這裡的邏輯稍微補償上面的 break
                    total_dequeued.fetch_add(1, std::memory_order_relaxed); 
                } 
                else 
                {
                    std::this_thread::yield();
                }
            } });
    }

    for (auto &t : producer_threads)
        t.join();
    for (auto &t : consumer_threads)
        t.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    if (violation_found)
        return false;

    int expected_total = num_producers * ops_per_producer;
    int actual_total = total_dequeued.load();
    bool count_ok = (actual_total == expected_total);

    if (count_ok)
    {
        std::cout << "OK (" << duration << "ms)\n";
    }
    else
    {
        std::cout << "FAIL (Count Mismatch)\n";
        std::cerr << "  Expected: " << expected_total << "\n";
        std::cerr << "  Actual:   " << actual_total << "\n";
        std::cerr << "  (This usually indicates ABA problem causing lost nodes)\n";
    }
    return count_ok;
}

template <class QueueType>
static bool test_shutdown_semantics()
{
    std::cout << "  -> Running Shutdown Semantics ... ";
    using ElementType = std::pair<int, int>;
    QueueType queue;
    ElementType dummy_item = {0, 0};

    // 1. 正常 Enqueue
    if (!queue.enqueue(dummy_item))
    {
        std::cout << "FAIL (Enqueue)\n";
        return false;
    }

    // 2. 關閉佇列
    queue.close();

    // 3. 關閉後 Enqueue 應失敗
    if (queue.enqueue(dummy_item) != false)
    {
        std::cout << "FAIL (Enqueue after close)\n";
        return false;
    }

    // 4. 關閉後 Dequeue 應成功 (因為還有剩餘元素)
    ElementType output;
    if (queue.try_dequeue(output) != true)
    {
        std::cout << "FAIL (Dequeue remaining)\n";
        return false;
    }

    // 5. 再次 Dequeue 應失敗 (已空且關閉)
    if (queue.try_dequeue(output) != false)
    {
        std::cout << "FAIL (Dequeue empty)\n";
        return false;
    }

    std::cout << "OK\n";
    return true;
}

template <typename QueueType>
static void run_suite(const std::string &name)
{
    std::cout << "======================================\n";
    std::cout << "[TEST SUITE] " << name << "\n";
    std::cout << "======================================\n";

    bool linear_ok = test_linearization<QueueType>("Linearization", 32, 32, 500000); // 增加並發度
    bool shutdown_ok = test_shutdown_semantics<QueueType>();

    std::cout << "--------------------------------------\n";
    std::cout << "Summary: " << ((linear_ok && shutdown_ok) ? "ALL PASS" : "FAIL") << "\n\n";
}

int main()
{
    std::cout << "Starting Lock-Free Queue Correctness Tests...\n\n";

    // 1. Mutex Baseline (應該總是通過)
    run_suite<mpmcq::MutexQueue<std::pair<int, int>>>("MutexQueue (Baseline)");

    // 2. Lock-Free with Hazard Pointers (應該通過)
    run_suite<mpmcq::LockFreeQueue<std::pair<int, int>, mpmcq::reclaimer::hazard_pointers>>("LockFree (Hazard Pointers)");

    // 3. Lock-Free with EBR (應該通過)
    run_suite<mpmcq::LockFreeQueue<std::pair<int, int>, mpmcq::reclaimer::epoch_based_reclamation>>("LockFree (EBR)");

    // 4. Lock-Free with No Reclamation (Memory Leak)
    // 這應該通過，因為沒有重用記憶體，所以不會有 ABA，只是會吃光 RAM
    std::cout << "[INFO] Testing 'No Reclamation' (Leak Mode)...\n"
              << "       This confirms the queue logic is correct when memory is infinite.\n";
    run_suite<mpmcq::LockFreeQueue<std::pair<int, int>, mpmcq::reclaimer::no_reclamation>>("LockFree (No Reclaim / Leak)");

    // 5. ABA Demonstration (Unsafe Reuse)
    // 這裡我們展示如果沒有 SMR，且直接 delete (觸發 Object Pool 重用)，會發生什麼事。
    std::cout << "======================================\n";
    std::cout << "[DEMO] ABA Vulnerability Demonstration\n";
    std::cout << "======================================\n";
    std::cout << "This test uses 'UnsafeDirectReclamation'. It immediately deletes nodes.\n";
    std::cout << "Because of the NodePool, addresses will be reused rapidly.\n";
    std::cout << "We expect this test to FAIL (Count Mismatch) or CRASH (Segfault).\n";
    std::cout << "Press ENTER to run this risky test (or Ctrl+C to stop)...";
    std::cin.get();

    try
    {
        // 使用更極端的參數來確保觸發 ABA
        // 16 Producers, 16 Consumers, 50000 Ops
        bool result = test_linearization<mpmcq::LockFreeQueue<std::pair<int, int>, UnsafeDirectReclamation>>(
            "Unsafe ABA Test", 32, 32, 500000);

        if (!result)
        {
            std::cout << "\n>>> SUCCESSFULLY DETECTED ABA PROBLEM! <<<\n";
            std::cout << "The queue failed linearization checks as expected without SMR.\n";
        }
        else
        {
            std::cout << "\n[WARNING] The test passed unexpectedly. ABA is probabilistic.\n"
                      << "Try increasing thread count or operations to trigger it.\n";
        }
    }
    catch (...)
    {
        std::cout << "\n>>> CRASH DETECTED (likely Segfault/Access Violation) <<<\n";
        std::cout << "This confirms ABA caused memory corruption.\n";
    }

    return 0;
}