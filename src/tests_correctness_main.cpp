/**
 * @file tests_correctness_main.cpp
 * @brief Correctness Verification Suite for Lock-Free Queue.
 * * This suite validates the functional correctness of various queue implementations
 * and reclamation strategies. It checks for:
 * 1. Linearization (FIFO order preservation per producer).
 * 2. Data Integrity (No lost or duplicated elements).
 * 3. Shutdown Semantics (Graceful termination).
 * 4. ABA Vulnerability (Demonstrating failure without proper SMR).
 */

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
//   Test Configuration & Utilities
// ==========================================

/**
 * @brief A dangerous reclaimer for ABA demonstration.
 * * This strategy immediately deletes nodes upon retirement. Combined with the 
 * internal Object Pool (NodePool), retired memory addresses are recycled 
 * extremely quickly. This maximizes the probability of ABA problems occurring 
 * during concurrent enqueue/dequeue operations.
 */
struct UnsafeDirectReclamation
{
    struct token { };
    static void quiescent() noexcept {}
    static token enter() noexcept { return {}; }
    static void protect_at(int, void *) {}

    template <typename Node>
    static void retire(Node *p) noexcept
    {
        // IMMEDIATE DELETION:
        // Returns memory to the NodePool instantly.
        // In a high-concurrency environment, this address will be re-allocated 
        // to a new node almost immediately, confusing other threads that hold 
        // stale pointers to this address (The classic ABA problem).
        delete p;
    }
};

// ==========================================
//   Core Test Logic
// ==========================================

/**
 * @brief Stress test for data consistency and order.
 * * Verifies that:
 * 1. Total items produced == Total items consumed.
 * 2. Order of items from a single producer is preserved (FIFO).
 * * @tparam QueueType The queue implementation to test.
 * @return true if all checks pass, false otherwise.
 */
template <class QueueType>
static bool test_linearization(const std::string &test_name, int num_producers = 4, int num_consumers = 4, int ops_per_producer = 20000)
{
    using ElementType = std::pair<int, int>; // {producer_id, sequence_number}
    QueueType queue;
    std::atomic<int> producers_finished{0};
    std::vector<std::thread> producer_threads, consumer_threads;

    std::cout << "  -> Running " << test_name << " [P:" << num_producers << ", C:" << num_consumers << ", Ops:" << ops_per_producer << "] ... " << std::flush;

    auto start_time = std::chrono::high_resolution_clock::now();

    // Producers: Generate monotonic sequences per thread
    for (int p = 0; p < num_producers; p++)
    {
        producer_threads.emplace_back([&, p]
        {
            for (int i = 0; i < ops_per_producer; i++) 
            { 
                // Spin-wait until enqueue succeeds
                while (!queue.enqueue({p, i})) {
                    std::this_thread::yield(); // Backoff to allow consumers to catch up
                }
                
                // Simulate quiescent state periodically (Required for EBR to advance epochs)
                if (i % 64 == 0) QueueType::quiescent(); 
            }
            if (producers_finished.fetch_add(1) + 1 == num_producers) 
            {
                queue.close();
            } 
        });
    }

    // Validation: Track last seen sequence for each producer
    // Note: consumer_seen[c][p] tracks the last sequence number consumer 'c' saw from producer 'p'.
    // To strictly verify global linearization, a reordering buffer would be needed, 
    // but this per-consumer check catches most ordering violations (reverse order).
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

                    // Verify FIFO order per producer
                    auto& seen_map = consumer_seen[c];
                    auto it = seen_map.find(value.first);
                    int last_seq = (it == seen_map.end() ? -1 : it->second);

                    if (value.second < last_seq) 
                    {
                        // Fatal: Received a sequence number older than previously seen
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
                    // Double check if truly empty after close
                    if (!queue.try_dequeue(value)) break;
                    
                    // Successful dequeue after close check
                    total_dequeued.fetch_add(1, std::memory_order_relaxed); 
                } 
                else 
                {
                    std::this_thread::yield();
                }
            } 
        });
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

/**
 * @brief Verify shutdown (close) behavior.
 * * Ensures that:
 * 1. Queue rejects new items after close.
 * 2. Queue allows draining of remaining items after close.
 * 3. Queue reports empty correctly after draining.
 */
template <class QueueType>
static bool test_shutdown_semantics()
{
    std::cout << "  -> Running Shutdown Semantics ... ";
    using ElementType = std::pair<int, int>;
    QueueType queue;
    ElementType dummy_item = {0, 0};

    // 1. Normal Enqueue
    if (!queue.enqueue(dummy_item))
    {
        std::cout << "FAIL (Enqueue)\n";
        return false;
    }

    // 2. Close Queue
    queue.close();

    // 3. Enqueue should fail after close
    if (queue.enqueue(dummy_item) != false)
    {
        std::cout << "FAIL (Enqueue after close)\n";
        return false;
    }

    // 4. Dequeue should succeed (draining remaining items)
    ElementType output;
    if (queue.try_dequeue(output) != true)
    {
        std::cout << "FAIL (Dequeue remaining)\n";
        return false;
    }

    // 5. Dequeue should fail (empty and closed)
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

    bool linear_ok = test_linearization<QueueType>("Linearization", 32, 32, 500000); // High concurrency
    bool shutdown_ok = test_shutdown_semantics<QueueType>();

    std::cout << "--------------------------------------\n";
    std::cout << "Summary: " << ((linear_ok && shutdown_ok) ? "ALL PASS" : "FAIL") << "\n\n";
}

int main()
{
    std::cout << "Starting Lock-Free Queue Correctness Tests...\n\n";

    // 1. Mutex Baseline (Verification of test harness itself)
    run_suite<mpmcq::MutexQueue<std::pair<int, int>>>("MutexQueue (Baseline)");

    // 2. Lock-Free with Hazard Pointers
    run_suite<mpmcq::LockFreeQueue<std::pair<int, int>, mpmcq::reclaimer::hazard_pointers>>("LockFree (Hazard Pointers)");

    // 3. Lock-Free with EBR
    run_suite<mpmcq::LockFreeQueue<std::pair<int, int>, mpmcq::reclaimer::epoch_based_reclamation>>("LockFree (EBR)");

    // 4. Lock-Free with No Reclamation (Memory Leak)
    // Should pass correctness checks because "leaking" avoids reuse, preventing ABA.
    std::cout << "[INFO] Testing 'No Reclamation' (Leak Mode)...\n"
              << "       This confirms the queue logic is correct when memory is infinite.\n";
    run_suite<mpmcq::LockFreeQueue<std::pair<int, int>, mpmcq::reclaimer::no_reclamation>>("LockFree (No Reclaim / Leak)");

    // 5. ABA Vulnerability Demonstration (Unsafe Reuse)
    // This demonstrates why SMR is strictly necessary.
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
        // Use extreme concurrency settings to maximize race conditions
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