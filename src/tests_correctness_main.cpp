#include <atomic>
#include <thread>
#include <vector>
#include <set>
#include <map>
#include <iostream>
#include <cassert>
#include <string>

#include "queue/lockfree_queue.hpp"
#include "queue/mutex_queue.hpp"
// 假設你之後會建立這些標頭檔
#include "reclaimer/hazard_pointers.hpp"
#include "reclaimer/epoch_based_reclamation.hpp"
#include "reclaimer/no_reclamation.hpp"

// 使用 Template 進行測試，Element Type 固定為 pair<int, int>
template <class QueueType>
static bool test_linearization(int num_producers = 4, int num_consumers = 4, int ops_per_producer = 10000) 
{
    using ElementType = std::pair<int, int>;
    QueueType queue;
    std::atomic<int> producers_finished{0};
    std::vector<std::thread> producer_threads, consumer_threads;

    // Producers: 產生 (producer_id, sequence)
    for (int p = 0; p < num_producers; p++) 
    {
        producer_threads.emplace_back([&, p] {
            for (int i = 0; i < ops_per_producer; i++) 
            { 
                // Spin until enqueue succeeds
                while (!queue.enqueue({p, i})) {}
                
                // 定期呼叫 quiescent
                if (i % 100 == 0) QueueType::quiescent(); 
            }
            if (producers_finished.fetch_add(1) + 1 == num_producers) 
            {
                queue.close();
            }
        });
    }

    // 驗證邏輯：記錄每個 Producer 最後被消費到的序號
    std::vector<std::map<int, int>> consumer_seen(num_consumers); 
    std::atomic<int> total_dequeued{0};

    for (int c = 0; c < num_consumers; c++) 
    {
        consumer_threads.emplace_back([&, c] {
            ElementType value;
            int ops_count = 0;
            while (true) 
            {
                if (queue.try_dequeue(value)) 
                {
                    total_dequeued.fetch_add(1);
                    ops_count++;

                    // 檢查線性一致性 (FIFO per producer)
                    auto& seen_map = consumer_seen[c];
                    auto it = seen_map.find(value.first);
                    int last_seq = (it == seen_map.end() ? -1 : it->second);

                    if (value.second < last_seq) 
                    {
                        std::cerr << "Order violation! Producer=" << value.first 
                                  << " Got=" << value.second 
                                  << " Last=" << last_seq << "\n";
                        std::abort();
                    }
                    seen_map[value.first] = value.second;

                    if (ops_count % 100 == 0) QueueType::quiescent();
                } 
                else if (queue.is_closed()) 
                {
                    // 佇列關閉且為空，結束消費
                    break;
                } 
                else 
                {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : producer_threads) t.join();
    for (auto& t : consumer_threads) t.join();

    int expected_total = num_producers * ops_per_producer;
    bool is_correct = (total_dequeued.load() == expected_total);
    
    if (!is_correct) 
    {
        std::cerr << "Mismatch! Total Dequeued=" << total_dequeued.load() 
                  << " Expected=" << expected_total << "\n";
    }
    return is_correct;
}

template <class QueueType>
static bool test_shutdown_semantics() 
{
    using ElementType = std::pair<int, int>;
    QueueType queue;
    ElementType dummy_item = {0, 0};

    // 1. 正常 Enqueue
    assert(queue.enqueue(dummy_item) == true);
    
    // 2. 關閉佇列
    queue.close();
    
    // 3. 關閉後 Enqueue 應失敗
    if (queue.enqueue(dummy_item) != false) return false;
    
    // 4. 關閉後 Dequeue 應成功 (因為還有剩餘元素)
    ElementType output;
    if (queue.try_dequeue(output) != true) return false;
    
    // 5. 再次 Dequeue 應失敗 (已空且關閉)
    if (queue.try_dequeue(output) != false) return false;

    return true;
}

template <typename QueueType>
static void run_suite(const std::string& name) 
{
    std::cout << "[RUN] " << name << "\n";
    
    bool linear_ok = test_linearization<QueueType>();
    bool shutdown_ok = test_shutdown_semantics<QueueType>();
    
    std::cout << "  Linearization: " << (linear_ok ? "PASS" : "FAIL") << "\n";
    std::cout << "  Shutdown Semantics: " << (shutdown_ok ? "PASS" : "FAIL") << "\n";
    std::cout << "--------------------------------------\n";
}

int main() 
{
    // 這裡的型別名稱 (HazardPointer 等) 需要等你實作 reclaimer 後確認
    // 目前假設命名為 CamelCase

    // Lock-free Tests
    run_suite<mpmcq::LockFreeQueue<std::pair<int, int>, mpmcq::reclaimer::hazard_pointers>>("LockFree (HP)");
    run_suite<mpmcq::LockFreeQueue<std::pair<int, int>, mpmcq::reclaimer::epoch_based_reclamation>>("LockFree (EBR)");
    run_suite<mpmcq::LockFreeQueue<std::pair<int, int>, mpmcq::reclaimer::no_reclamation>>("LockFree (None)");

    // Mutex Baseline
    run_suite<mpmcq::MutexQueue<std::pair<int, int>>>("MutexQueue");

    std::cout << "[NOTE] ABA tests are pending SMR implementation.\n";
    return 0;
}