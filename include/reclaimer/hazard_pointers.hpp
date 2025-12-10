#pragma once
#include <atomic>
#include <vector>
#include <thread>
#include <cstddef>
#include <mutex>
#include <algorithm> // for std::sort, std::binary_search
#include <iostream>

namespace mpmcq::reclaimer
{

// 每個執行緒最多同時保護 K 個指針 (M&S Queue 的 enqueue/dequeue 最多同時需要 2~3 個)
constexpr int HP_COUNT_PER_THREAD = 3;
// 觸發掃描的閾值 (越大吞吐量越高，但記憶體佔用越多)
constexpr int HP_RETIRE_THRESHOLD = 128; 

// Hazard Pointer Record (每個執行緒擁有一份，但掛在全域鏈表上)
struct alignas(64) HPRecType 
{
    std::atomic<void*> hp[HP_COUNT_PER_THREAD];
    std::atomic<bool> active{false};
    HPRecType* next{nullptr};

    HPRecType() 
    {
        for (int i = 0; i < HP_COUNT_PER_THREAD; ++i)
        {
            hp[i].store(nullptr, std::memory_order_relaxed);
        }
    }
};

class HazardPointerManager
{
public:
    static HazardPointerManager &instance()
    {
        static HazardPointerManager mgr;
        return mgr;
    }

    // 待回收節點的封裝
    struct RetiredNode 
    {
        void* ptr;
        void (*deleter)(void*); // 存儲刪除函數，確保能呼叫到 Object Pool 的 operator delete
    };

    // 每個執行緒的本地上下文
    struct ThreadContext
    {
        HPRecType* my_rec = nullptr;    // 指向全域鏈表中的自己的 Record
        std::vector<RetiredNode> retire_list; 

        ThreadContext(HazardPointerManager& mgr) 
        {
            my_rec = mgr.acquire_record();
        }

        ~ThreadContext() 
        {
            if (my_rec) 
            {
                // 執行緒結束時，釋放 Record 讓別人用
                // 注意：這裡我們不清理 retire_list，嚴格來說這些垃圾會洩漏。
                // 在正式實作中應該把這些節點轉移到全域孤兒列表 (Global Orphan List)。
                // 但為了作業專案的複雜度控制，我們允許這裡的小量洩漏。
                HazardPointerManager::instance().release_record(my_rec);
            }
        }
    };

    static ThreadContext &get_context()
    {
        thread_local ThreadContext ctx(instance());
        return ctx;
    }

    // 1. 設置保護 (公告天下我正在讀 ptr)
    void protect(int idx, void *ptr) noexcept
    {
        auto &ctx = get_context();
        if (idx < HP_COUNT_PER_THREAD)
        {
            ctx.my_rec->hp[idx].store(ptr, std::memory_order_seq_cst);
        }
    }

    // 2. 清除保護
    void clear(int idx) noexcept
    {
        auto &ctx = get_context();
        if (idx < HP_COUNT_PER_THREAD)
        {
            ctx.my_rec->hp[idx].store(nullptr, std::memory_order_release);
        }
    }

    // 3. 登記退休 (放入本地緩衝區)
    template <typename T>
    void retire_node(T *ptr) noexcept
    {
        auto &ctx = get_context();
        ctx.retire_list.push_back({
            static_cast<void*>(ptr),
            [](void* p) { delete static_cast<T*>(p); } // 這會呼叫 T 的 operator delete (Object Pool)
        });

        if (ctx.retire_list.size() >= HP_RETIRE_THRESHOLD)
        {
            scan_and_retire();
        }
    }

    // 4. 掃描與回收 (核心演算法)
    void scan_and_retire() noexcept
    {
        auto &ctx = get_context();
        
        // 階段一：收集所有其他執行緒的 Hazard Pointers
        std::vector<void*> hazards;
        // 預估容量，避免頻繁 alloc
        hazards.reserve(HP_COUNT_PER_THREAD * 16); 

        // 遍歷全域鏈表 (這是一個簡單的 Lock-Free 遍歷，因為我們只append不delete節點)
        HPRecType* curr = head_rec_.load(std::memory_order_acquire);
        while (curr) {
            if (curr->active.load(std::memory_order_acquire)) {
                for (int i = 0; i < HP_COUNT_PER_THREAD; ++i) {
                    // 如果缺少中間的 Fence (或 seq_cst 操作)，
                    // Reader 可能先讀取 Head 確認有效，然後才公告 HP。
                    // 而在這微小的時間差內，Reclaimer 可能已經讀取了 HP (發現沒人看) 並刪除了節點。
                    // 加上 atomic_thread_fence (即seq_cst) 後，這種重排被物理禁止，確保了正確性。

                    // Jay: 可能不用 seq_cst，待驗證
                    //      可能 "非 x86" 平台需要
                    void* p = curr->hp[i].load(std::memory_order_acquire);
                    // void* p = curr->hp[i].load(std::memory_order_seq_cst);
                    if (p) hazards.push_back(p);
                }
            }
            curr = curr->next;
        }

        // 階段二：排序，以便二分搜尋
        std::sort(hazards.begin(), hazards.end());

        // 階段三：過濾 retire_list
        // 我們將需要保留的節點移到 vector 前端，最後一次 truncate
        std::vector<RetiredNode>& list = ctx.retire_list;
        
        size_t kept_count = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            // 如果此節點存在於 hazards 中 -> 不能刪 -> 保留
            if (std::binary_search(hazards.begin(), hazards.end(), list[i].ptr)) {
                if (i != kept_count) {
                    list[kept_count] = list[i];
                }
                kept_count++;
            } else {
                // 沒人看 -> 安全 -> 刪除 (歸還給 Pool)
                list[i].deleter(list[i].ptr);
            }
        }
        
        // 縮小 vector 大小，移除已刪除的元素
        list.resize(kept_count);
    }

    // 輔助：申請一個 HP Record (重複利用或新增)
    HPRecType* acquire_record() {
        // 1. 嘗試在鏈表中找一個沒在用的 (active == false)
        HPRecType* curr = head_rec_.load(std::memory_order_acquire);
        while (curr) {
            if (!curr->active.load(std::memory_order_acquire)) {
                bool expected = false;
                if (curr->active.compare_exchange_strong(expected, true, std::memory_order_seq_cst)) {
                    return curr;
                }
            }
            curr = curr->next;
        }

        // 2. 沒找到，new 一個新的並掛到鏈表頭部
        // 注意：這裡會有微小的記憶體洩漏 (Rec 節點本身只增不減)，但這在 HP 算法中是標準做法
        HPRecType* new_rec = new HPRecType();
        new_rec->active.store(true, std::memory_order_relaxed);
        
        // CAS Loop 插入鏈表頭
        HPRecType* old_head = head_rec_.load(std::memory_order_relaxed);
        do {
            new_rec->next = old_head;
        } while (!head_rec_.compare_exchange_weak(old_head, new_rec, std::memory_order_release, std::memory_order_relaxed));

        return new_rec;
    }

    // 輔助：釋放 Record (標記為不活躍)
    void release_record(HPRecType* rec) {
        // 清空指針
        for (int i = 0; i < HP_COUNT_PER_THREAD; ++i)
            rec->hp[i].store(nullptr, std::memory_order_release);
        // 標記為可重用
        rec->active.store(false, std::memory_order_release);
    }

private:
    std::atomic<HPRecType*> head_rec_{nullptr};
    HazardPointerManager() = default;
};

// 用戶端接口
struct hazard_pointers
{
    struct token { };

    // 為了相容 Benchmark 的介面，這裡也實作 quiescent
    // 在 HP 中，quiescent 通常意味著 "嘗試回收所有積壓垃圾"
    static void quiescent() noexcept
    {
        // 簡單做一次掃描
        HazardPointerManager::instance().scan_and_retire();
    }

    static token enter() noexcept { return {}; }

    template <class Node>
    static void retire(Node *p) noexcept
    {
        HazardPointerManager::instance().retire_node(p);
    }

    // 核心功能：設置 Hazard Pointer
    static void protect_at(int idx, void *ptr) noexcept
    {
        if (ptr) HazardPointerManager::instance().protect(idx, ptr);
        else HazardPointerManager::instance().clear(idx);
    }
};

} // namespace mpmcq::reclaimer