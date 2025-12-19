#pragma once
#include <atomic>
#include <vector>
#include <thread>
#include <cstddef>
#include <mutex>
#include <algorithm> // for std::sort, std::binary_search

namespace mpmcq::reclaimer
{

// Maximum number of hazard pointers a single thread can hold simultaneously.
// For standard M&S Queue operations (enqueue/dequeue), 2-3 pointers are sufficient.
constexpr int HP_COUNT_PER_THREAD = 2;

// Threshold for triggering a garbage collection scan.
// Larger values improve throughput by batching work but increase peak memory usage.
constexpr int HP_RETIRE_THRESHOLD = 128; 

// ==========================================
// Hazard Pointer Record
// ==========================================
// A structure allocated for each thread to publish the pointers it is currently accessing.
// These records are linked in a global list but owned by individual threads.
struct alignas(64) HPRecType 
{
    std::atomic<void*> hp[HP_COUNT_PER_THREAD];
    std::atomic<bool> is_acquired{false};
    HPRecType* next{nullptr};

    HPRecType() 
    {
        for (int i = 0; i < HP_COUNT_PER_THREAD; ++i)
        {
            hp[i].store(nullptr, std::memory_order_relaxed);
        }
    }
};

/**
 * @brief Singleton manager for Hazard Pointers.
 * * This class manages the lifecycle of Hazard Pointer Records (HPRecType)
 * and orchestrates the scanning and reclamation process.
 */
class HazardPointerManager
{
public:
    static HazardPointerManager& instance()
    {
        static HazardPointerManager central_reclaimer;
        return central_reclaimer;
    }

    // Type-erased wrapper for a node that is ready to be deleted.
    struct RetiredNode 
    {
        void* ptr;
        void (*deleter)(void*); // Function pointer to ensure correct destruction (handling Object Pool)
    };

    // Thread-local context to cache the acquired HP record and retired nodes.
    struct ThreadContext
    {
        HPRecType* my_rec = nullptr;    // Pointer to this thread's global record
        std::vector<RetiredNode> retire_list; 

        ThreadContext(HazardPointerManager& central_reclaimer) 
        {
            my_rec = central_reclaimer.acquire_record();
        }

        ~ThreadContext() 
        {
            if (my_rec) 
            {
                // Upon thread exit, release the record for reuse.
                // Note: Ideally, we should move remaining nodes in retire_list to a global orphan list.
                // For simplicity in this implementation, we allow a small leak here.
                if (!retire_list.empty()) 
                {
                    HazardPointerManager::instance().scan_and_retire(); 
                }
                HazardPointerManager::instance().release_record(my_rec);
            }
        }
    };

    static ThreadContext& get_context()
    {
        thread_local ThreadContext ctx(instance());
        return ctx;
    }

    /**
     * @brief Publish a pointer to be protected.
     * * This announces to all other threads: "I am reading this pointer, do not delete it."
     * * @param idx The index slot in the HP array (0 to HP_COUNT_PER_THREAD-1).
     * @param ptr The raw pointer address to protect.
     */
    void protect(int idx, void *ptr) noexcept
    {
        auto &ctx = get_context();
        if (idx < HP_COUNT_PER_THREAD)
        {
            // Use seq_cst to ensure the publication is visible before reading the node's data.
            ctx.my_rec->hp[idx].store(ptr, std::memory_order_seq_cst);
        }
    }

    // Clear a protection slot.
    void clear(int idx) noexcept
    {
        auto &ctx = get_context();
        if (idx < HP_COUNT_PER_THREAD)
        {
            ctx.my_rec->hp[idx].store(nullptr, std::memory_order_release);
        }
    }

    /**
     * @brief Mark a node for retirement.
     * * The node is added to a thread-local buffer. Actual deletion is deferred
     * until the buffer is full and a scan confirms no other thread is using it.
     */
    template <typename T>
    void retire_node(T *ptr) noexcept
    {
        auto &ctx = get_context();

        RetiredNode node_to_retire;
        node_to_retire.ptr = static_cast<void*>(ptr);
        node_to_retire.deleter = [](void* p) { delete static_cast<T*>(p); };
        ctx.retire_list.push_back(node_to_retire);

        if (ctx.retire_list.size() >= HP_RETIRE_THRESHOLD)
        {
            scan_and_retire();
        }
    }

    /**
     * @brief The core reclamation algorithm (Scan).
     * * 1. Collect all hazard pointers currently published by all threads.
     * 2. Sort them for fast lookup.
     * 3. Iterate through the local retirement list:
     * - If a node is in the hazard list, keep it (someone is using it).
     * - If not, safe to delete.
     */
    void scan_and_retire() noexcept
    {
        auto &ctx = get_context();
        
        // Phase 1: Collect Hazard Pointers from all threads
        std::vector<void*> hazards;
        hazards.reserve(HP_COUNT_PER_THREAD * 16); 

        // Iterate through the global linked list of HP records.
        // This is safe because we never delete HPRecType nodes, only add them.
        HPRecType* curr_rec = head_rec_.load(std::memory_order_acquire);
        while (curr_rec) {
            if (curr_rec->is_acquired.load(std::memory_order_acquire)) 
            {
                for (int i = 0; i < HP_COUNT_PER_THREAD; ++i) 
                {
                    // Note on Memory Ordering:
                    // acquire ensures we see the latest value published by the reader.
                    // On non-x86 architectures (ARM/PowerPC), seq_cst fences might be 
                    // required here and in protect() to strictly order the "publish HP" 
                    // vs "retire Node" sequence. For x86, acquire/release is often sufficient.
                    void* p = curr_rec->hp[i].load(std::memory_order_acquire);
                    if (p) hazards.push_back(p);
                }
            }
            curr_rec = curr_rec->next;
        }

        // Phase 2: Sort for binary search
        std::sort(hazards.begin(), hazards.end());

        // Phase 3: Check retirement list against hazards
        std::vector<RetiredNode>& list = ctx.retire_list;
        
        size_t kept_count = 0;
        for (size_t i = 0; i < list.size(); ++i) 
        {
            // If pointer exists in hazards -> cannot delete -> keep it.
            if (std::binary_search(hazards.begin(), hazards.end(), list[i].ptr)) 
            {
                if (i != kept_count) 
                {
                    list[kept_count] = list[i];
                }
                kept_count++;
            } 
            else 
            {
                // No one is watching -> Safe to delete -> Return to Pool
                list[i].deleter(list[i].ptr);
            }
        }
        
        // Remove deleted elements from the vector
        list.resize(kept_count);
    }

    // Helper: Acquire an HP Record (reuse existing or create new)
    HPRecType* acquire_record() 
    {
        // 1. Try to find a free record in the existing list
        HPRecType* curr_rec = head_rec_.load(std::memory_order_acquire);
        while (curr_rec) 
        {
            if (!curr_rec->is_acquired.load(std::memory_order_acquire)) 
            {
                bool expected = false;
                if (curr_rec->is_acquired.compare_exchange_strong(expected, 
                                                         true, 
                                                         std::memory_order_seq_cst)) 
                {
                    return curr_rec;
                }
            }
            curr_rec = curr_rec->next;
        }

        // 2. No free record found, allocate a new one and prepend to list
        // Note: HP records are never deleted, which is standard for this algorithm.
        HPRecType* new_rec = new HPRecType();
        new_rec->is_acquired.store(true, std::memory_order_relaxed);
        
        // CAS Loop to insert at head
        HPRecType* old_head = head_rec_.load(std::memory_order_relaxed);
        do {
            new_rec->next = old_head;
        } while (!head_rec_.compare_exchange_weak(old_head, new_rec, 
                                                  std::memory_order_release, 
                                                  std::memory_order_relaxed));

        return new_rec;
    }

    // Helper: Release a record back to the pool (mark as not acquired)
    void release_record(HPRecType* rec) 
    {
        // Clear all pointers
        for (int i = 0; i < HP_COUNT_PER_THREAD; ++i)
        {
            rec->hp[i].store(nullptr, std::memory_order_release);
        }
        // Mark as free
        rec->is_acquired.store(false, std::memory_order_release);
    }

private:
    std::atomic<HPRecType*> head_rec_{nullptr};
    HazardPointerManager() = default;
};

/**
 * @brief Public Interface for Hazard Pointer Reclamation.
 * * Complies with the Reclaimer concept required by LockFreeQueue.
 */
struct hazard_pointers
{
    struct token { };

    // API Compatibility: Performs a manual scan attempt.
    static void quiescent() noexcept
    {
        HazardPointerManager::instance().scan_and_retire();
    }

    static token enter() noexcept { return {}; }

    template <class Node>
    static void retire(Node *p) noexcept
    {
        HazardPointerManager::instance().retire_node(p);
    }

    // Core functionality: Protect a pointer at a specific index.
    static void protect_at(int idx, void *ptr) noexcept
    {
        if (ptr) HazardPointerManager::instance().protect(idx, ptr);
        else HazardPointerManager::instance().clear(idx);
    }
};

} // namespace mpmcq::reclaimer