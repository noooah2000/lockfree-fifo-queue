#pragma once
#include <atomic>
#include <vector>
#include <thread>
#include <mutex>
#include <list>
#include <algorithm>
#include <cassert>

namespace mpmcq::reclaimer 
{

// Threshold to trigger a global epoch scan attempt.
constexpr int EBR_RETIRE_THRESHOLD = 512;

/**
 * @brief Singleton manager for Epoch-Based Reclamation (EBR).
 * * This class coordinates the global epoch and manages thread-local contexts.
 * It implements a standard 3-epoch system (Current, Previous, Safe) to ensure
 * memory safety in lock-free data structures.
 */
class EpochBasedReclaimationManager 
{
public:
    static EpochBasedReclaimationManager& instance() 
    {
        static EpochBasedReclaimationManager central_reclaimer;
        return central_reclaimer;
    }

    struct RetiredNode 
    {
        void* ptr;
        void (*deleter)(void*);
    };

    struct ThreadContext 
    {
        // Align to cache-line (64 bytes) to prevent false sharing between 
        // local_epoch and in_critical flag, which are frequently accessed.
        alignas(64) std::atomic<size_t> local_epoch{0};
        alignas(64) std::atomic<bool> in_critical{false};
        
        // 3-Epoch buffer: [0]..[2] correspond to Current, Previous, and Safe buckets.
        std::vector<RetiredNode> retire_lists[3];
        EpochBasedReclaimationManager* manager = nullptr;

        ThreadContext(EpochBasedReclaimationManager& central_reclaimer) : manager(&central_reclaimer) 
        {
            manager->register_thread(this);
            // Pre-allocate memory to minimize dynamic resizing overhead during runtime.
            for(int i=0; i<3; ++i) retire_lists[i].reserve(EBR_RETIRE_THRESHOLD * 2);
        }

        ~ThreadContext() 
        {
            if (manager) 
            {
                manager->unregister_thread(this);
                // Clean up any remaining retired nodes to prevent memory leaks upon thread exit.
                for (int i = 0; i < 3; ++i) 
                {
                    for (auto& node : retire_lists[i]) node.deleter(node.ptr);
                    retire_lists[i].clear();
                }
            }
        }
    };

    static ThreadContext& get_context() 
    {
        thread_local ThreadContext ctx(instance());
        return ctx;
    }

    /**
     * @brief Mark the current thread as active in a critical section.
     * * Updates the local epoch to match the global epoch and sets the
     * active flag. This prevents the global epoch from advancing past
     * this thread's view of the world.
     */
    void enter_critical() noexcept 
    {
        auto& ctx = get_context();
        size_t global_epoch = global_epoch_.load(std::memory_order_relaxed);
        ctx.local_epoch.store(global_epoch, std::memory_order_relaxed);
        ctx.in_critical.store(true, std::memory_order_seq_cst);
    }

    /**
     * @brief Mark the current thread as inactive.
     * * Signals that this thread holds no references to shared memory,
     * allowing the global epoch to potentially advance.
     */
    void exit_critical() noexcept 
    {
        auto& ctx = get_context();
        ctx.in_critical.store(false, std::memory_order_release);
    }

    template <typename T>
    void retire_node(T* ptr) noexcept 
    {
        auto& ctx = get_context();
        // Determine the bucket based on the current global epoch.
        size_t curr_epoch = global_epoch_.load(std::memory_order_relaxed);
        size_t idx = curr_epoch % 3;

        // 1. Create a type-erased record of the retired node
        RetiredNode node_to_retire;
        node_to_retire.ptr = static_cast<void*>(ptr);
        
        // 2. Define the deleter to correctly cast back and delete later
        node_to_retire.deleter = [](void* p) { delete static_cast<T*>(p); };
        
        // 3. Add to the local retirement list for the current epoch
        ctx.retire_lists[idx].push_back(node_to_retire);

        // 4. Trigger a scan attempt if the buffer exceeds the threshold
        if (ctx.retire_lists[idx].size() > EBR_RETIRE_THRESHOLD) 
        {
            scan_and_retire();
        }
    }

    // Manually signal a quiescent state (checkpoint), mostly for testing or specific algorithms.
    void quiescent_state() noexcept 
    {
        auto& ctx = get_context();
        size_t global_epoch = global_epoch_.load(std::memory_order_relaxed);
        ctx.local_epoch.store(global_epoch, std::memory_order_release);
        scan_and_retire();
    }

    /**
     * @brief Attempt to advance the global epoch and reclaim memory.
     * * This function checks if all active threads have caught up to the current
     * global epoch. If so, it advances the global epoch, effectively making
     * the "safe" bucket available for deletion.
     */
    void scan_and_retire() noexcept 
    {
        // Non-blocking lock attempt. If another thread is scanning, we skip.
        // This prevents the "convoy effect" where threads queue up to reclaim memory.
        std::unique_lock<std::mutex> lock(list_mtx_, std::try_to_lock);
        if (!lock.owns_lock()) return;

        size_t snapshot_epoch = global_epoch_.load(std::memory_order_acquire);
        bool can_advance = true;
        
        // Iterate through all registered threads to check their epochs
        for (ThreadContext* ctx : thread_registry_) 
        {
            bool t_in_critical = ctx->in_critical.load(std::memory_order_acquire);
            if (t_in_critical) 
            {
                // If the thread is active, ensure its epoch matches the snapshot.
                size_t t_epoch = ctx->local_epoch.load(std::memory_order_acquire);
                if (t_epoch != snapshot_epoch) 
                {
                    can_advance = false;
                    break;
                }
            }
        }

        if (can_advance) 
        {
            size_t next_epoch = snapshot_epoch + 1;
            global_epoch_.store(next_epoch, std::memory_order_release);
            
            // Note: Actual deletion happens in attempt_local_cleanup() by individual threads.
        }
    }

    /**
     * @brief Opportunistic cleanup of the safe epoch bucket.
     * * This allows threads to reclaim memory locally even if they failed to 
     * acquire the lock for a full scan, provided the global epoch has advanced.
     */
    void attempt_local_cleanup() 
    {
        size_t snapshot_epoch = global_epoch_.load(std::memory_order_relaxed);
        
        // Calculate the index of the "Safe" bucket.
        // In a 3-epoch system (Current=e, Prev=e-1, Safe=e-2), 
        // the math (e + 1) % 3 correctly points to the Safe bucket index.
        size_t safe_idx = (snapshot_epoch + 1) % 3;
        auto& ctx = get_context();
        
        if (!ctx.retire_lists[safe_idx].empty()) 
        {
            clean_list(ctx.retire_lists[safe_idx]);
        }
    }

private:
    std::atomic<size_t> global_epoch_{0};
    std::mutex list_mtx_;
    std::list<ThreadContext*> thread_registry_;

    EpochBasedReclaimationManager() = default;

    void register_thread(ThreadContext* ctx) 
    {
        std::lock_guard<std::mutex> lock(list_mtx_);
        thread_registry_.push_back(ctx);
    }

    void unregister_thread(ThreadContext* ctx) 
    {
        std::lock_guard<std::mutex> lock(list_mtx_);
        thread_registry_.remove(ctx);
    }

    void clean_list(std::vector<RetiredNode>& list) 
    {
        for (auto& node : list) node.deleter(node.ptr);
        list.clear();
    }
};

/**
 * @brief Public Interface for Epoch-Based Reclamation.
 * * Complies with the standard Reclaimer concept used in LockFreeQueue.
 */
struct epoch_based_reclamation {
    // RAII wrapper for Critical Section management
    struct token 
    {
        ~token() 
        {
            EpochBasedReclaimationManager::instance().exit_critical();
        }
    };

    static void quiescent() noexcept 
    {
        EpochBasedReclaimationManager::instance().quiescent_state();
    }

    static token enter() noexcept 
    {
        EpochBasedReclaimationManager::instance().enter_critical();
        return {};
    }

    template <typename Node>
    static void retire(Node* p) noexcept 
    {
        auto& central_reclaimer = EpochBasedReclaimationManager::instance();
        central_reclaimer.retire_node(p);
        
        // Opportunistically try to clean up the safe list.
        // Even if scan_and_retire() failed (lock busy), we can still recycle 
        // if the global epoch was advanced by another thread.
        central_reclaimer.attempt_local_cleanup(); 
    }

    // No-op: EBR does not require per-pointer protection like Hazard Pointers.
    // Kept for API compatibility with LockFreeQueue.
    static void protect_at(int, void*) {}   
};

} // namespace mpmcq::reclaimer