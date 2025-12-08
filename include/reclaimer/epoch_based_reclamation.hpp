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
constexpr int EBR_RETIRE_THRESHOLD = 512;

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
        // 讓 active 和 local_epoch 處於不同的 Cache Line 避免 False Sharing
        alignas(64) std::atomic<size_t> local_epoch{0};
        alignas(64) std::atomic<bool> active{false};
        
        std::vector<RetiredNode> retire_lists[3];
        EpochBasedReclaimationManager* manager = nullptr;

        ThreadContext(EpochBasedReclaimationManager* central_reclaimer) : manager(central_reclaimer) 
        {
            manager->register_thread(this);
            // 預先分配空間，減少 vector 擴展的開銷
            for(int i=0; i<3; ++i) retire_lists[i].reserve(EBR_RETIRE_THRESHOLD * 2);
        }

        ~ThreadContext() 
        {
            if (manager) 
            {
                manager->unregister_thread(this);
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
        thread_local ThreadContext ctx(&instance());
        return ctx;
    }

    void enter_critical() noexcept 
    {
        auto& ctx = get_context();
        // 先更新 Epoch
        ctx.local_epoch.store(global_epoch_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        // 再宣告自己 Active
        ctx.active.store(true, std::memory_order_seq_cst);
    }

    void exit_critical() noexcept 
    {
        auto& ctx = get_context();
        // 使用 release 即可，不需要 seq_cst
        ctx.active.store(false, std::memory_order_release);
    }

    template <typename T>
    void retire_node(T* ptr) noexcept 
    {
        auto& ctx = get_context();
        // 讀取 global epoch 不需要太強的順序
        size_t curr_epoch = global_epoch_.load(std::memory_order_relaxed);
        size_t idx = curr_epoch % 3;

        // 1. 建立一個 RetiredNode 結構體實體
        RetiredNode node_to_retire;
        // 2. 設定指標：將具體型別指標轉為通用 void* 存入
        node_to_retire.ptr = static_cast<void*>(ptr);
        // 3. 設定 Deleter：明確定義如何將 void* 轉回 T* 並執行 delete
        node_to_retire.deleter = [](void* p) {delete static_cast<T*>(p);};
        // 4. 將設定好的包裹存入對應世代的垃圾桶
        ctx.retire_lists[idx].push_back(node_to_retire);

        if (ctx.retire_lists[idx].size() > EBR_RETIRE_THRESHOLD) 
        {
            scan_and_retire();
        }
    }

    void quiescent_state() noexcept 
    {
        auto& ctx = get_context();
        size_t g = global_epoch_.load(std::memory_order_relaxed);
        // 更新 epoch 讓別人知道我活著且推進了
        ctx.local_epoch.store(g, std::memory_order_release);
        scan_and_retire();
    }

    void scan_and_retire() noexcept 
    {
        // 如果有人正在掃描，我們就不掃了，直接返回。這避免了所有執行緒卡在這裡排隊。
        std::unique_lock<std::mutex> lock(list_mtx_, std::try_to_lock);
        if (!lock.owns_lock()) return;

        size_t snapshot_epoch = global_epoch_.load(std::memory_order_acquire);
        bool can_advance = true;
        
        // 遍歷所有執行緒檢查 Epoch
        for (ThreadContext* ctx : thread_registry_) 
        {
            // 載入 active 狀態
            bool t_active = ctx->active.load(std::memory_order_acquire);
            if (t_active) 
            {
                // 如果活躍，檢查他的 epoch 是否落後
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
            
            // 既然我們持有鎖，且我們是負責推進 Epoch 的人
            // 這裡其實可以順便幫自己清理一下，但為了簡單，
            // 還是讓各個執行緒下次 retire 時自己清理
        }
        
        // 離開函式時自動解鎖
        // 額外清理：既然我們拿到了鎖，且可能推進了 Epoch
        // 我們可以嘗試清理「當前執行緒」的 Safe List
        // (注意：這裡只清理自己的，因為 ThreadContext 是 thread_local 的)
        // 為了安全存取自己的 ThreadContext，我們需要重新獲取引用
        // 但這裡在靜態函式有點麻煩，所以保持原樣，
        // 讓各執行緒下次呼叫 retire_node 時，透過下面的邏輯清理。
    }

    // 補充：在 scan 之外，我們也應該嘗試清理 safe list
    // 這樣即使沒搶到鎖，也能回收記憶體
    void attempt_local_cleanup() 
    {
        size_t snapshot_epoch = global_epoch_.load(std::memory_order_relaxed);
        // Safe bucket is (current + 1) % 3? No.
        // If global is e, then e is Current.
        // e-1 (Previous) might still be in use by lagging threads.
        // e-2 (Safe) is definitely safe.
        // 數學上: (snapshot_epoch + 1) % 3 是 Safe 的
        // 舉例：G=2. Current=2, Prev=1, Safe=0. (2+1)%3 = 0. 正確。
        
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

struct epoch_based_reclamation {
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
        // 每次 retire 時，順便嘗試清理本地的 safe list
        // 這樣即使 scan 失敗（沒搶到鎖），只要 Global Epoch 被別人推動了，我就能回收
        central_reclaimer.attempt_local_cleanup(); 
    }

    static void protect_at(int, void*) {}   // 對齊 HP 實作
};

} // namespace mpmcq::reclaimer