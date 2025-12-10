#pragma once
#include <atomic>
#include <cstddef>
#include <utility>
#include <new>
#include <thread>
#include <vector>
#include <mutex>

// 檢查 C++ 標準，需要 C++17
#if __cplusplus < 201703L
    #error "This header requires C++17 or later (use -std=c++17)"
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
#endif

// 定義一個跨平台的 cpu_relax
inline void cpu_relax() noexcept 
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause(); 
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#elif defined(__powerpc__) || defined(__ppc__)
    asm volatile("or 27,27,27" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

namespace mpmcq 
{

struct SimpleBackoff 
{
    // 把單純 ++n 改成指數 backoff，避免每個 threads 重試的頻率一樣而造成 bus 壅擠
    int n = 1;
    static constexpr int MAX_YIELD = 64; 
    inline void pause() noexcept 
    {
#ifdef LFQ_USE_BACKOFF // 只有定義了 Backoff 才執行
        if (n <= MAX_YIELD) 
        {
            for (int i = 0; i < n; ++i) {
                cpu_relax();
            }
            n *= 2;
        } 
        else 
        {
            // 如果真的搶太兇，就讓出 Time Slice
            std::this_thread::yield();
            n = 1;
        }
#endif // LFQ_USE_BACKOFF
    }
};

// 必須將 NodePool 定義在 LockFreeQueue 之前，否則編譯器找不到
template <typename Node>
class NodePool 
{
    // 1. 定義一個內部類別來管理本地緩衝區
    struct LocalBuffer 
    {
        std::vector<Node*> vec;
        // 解構子：當執行緒結束時自動執行
        ~LocalBuffer() 
        {
            if (!vec.empty())
            {
                // 把私房錢歸還給全域池 (讓其他執行緒重用)
                std::lock_guard<std::mutex> lock(NodePool::global_pool_mutex);
                for (Node* node : vec) 
                {
                    NodePool::global_pool.push_back(node);
                }
            }
        }
    };

public:
    // 2. 將原本直接宣告 vector 改為宣告這個 Wrapper
    // 注意：這裡是 thread_local，每個執行緒都有一個獨立的 LocalBuffer 物件
    inline static thread_local LocalBuffer local_pool;
    inline static std::mutex global_pool_mutex;
    inline static std::vector<Node*> global_pool;

    static Node* allocate() 
    {
        // 改用 local_pool.vec 來存取 vector
        if (!local_pool.vec.empty()) 
        {
            Node* new_node = local_pool.vec.back();
            local_pool.vec.pop_back();
            return new_node;
        }

        // 2. 本地沒了，去全域搬貨 (一次搬 32 個)
        {
            std::lock_guard<std::mutex> lock(global_pool_mutex);
            if (!global_pool.empty()) 
            {
                int count = 0;
                while(!global_pool.empty() && count < 32) 
                {
                    local_pool.vec.push_back(global_pool.back());
                    global_pool.pop_back();
                    count++;
                }
            }
        }

        // 3. 搬完後再查一次本地
        if (!local_pool.vec.empty()) 
        {
            Node* new_node = local_pool.vec.back();
            local_pool.vec.pop_back();
            return new_node;
        }
        
        // 4. 真的沒貨，跟 OS 要記憶體 (使用 ::operator new 防止遞迴)
        return static_cast<Node*>(::operator new(sizeof(Node)));
    }

    static void deallocate(Node* recycled_node) 
    {
        // 多此一舉 先註解掉
        // 重置 next 指標，防止髒數據
        // recycled_node->next.store(nullptr, std::memory_order_relaxed);
        
        // 如果本地積太多 (>64)，還一半給全域
        // 改用 local_pool.vec
        if (local_pool.vec.size() > 64) 
        {
            std::lock_guard<std::mutex> lock(global_pool_mutex);
            for (int i=0; i<32; ++i) 
            {
                global_pool.push_back(local_pool.vec.back());
                local_pool.vec.pop_back();
            }
        }
        // 放回本地
        local_pool.vec.push_back(recycled_node);
    }
};

// Michael & Scott MPMC FIFO Queue with SMR support
template <class T, class Reclaimer>
class LockFreeQueue 
{
    struct Node 
    {
        std::atomic<Node*> next{nullptr};
        T value;

        explicit Node(const T& v) : next(nullptr), value(v) {}
        Node() : next(nullptr), value() {}

        // 重載 new/delete 使用 NodePool
#ifdef LFQ_USE_NODEPOOL // 只有定義了 NodePool 才使用重載
        void* operator new(size_t) 
        {
            return NodePool<Node>::allocate();
        }

        void operator delete(void* p) 
        {
            NodePool<Node>::deallocate(static_cast<Node*>(p));
        }
#endif // LFQ_USE_NODEPOOL
    };

public:
    explicit LockFreeQueue(std::size_t /*cap_hint*/ = 0) 
    {
        Node* dummy = new Node();      // init dummy 哨兵節點，真正的資料存在 Head->next 裡
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
        closed_.store(false, std::memory_order_relaxed);
    }

    ~LockFreeQueue() 
    {
        Node* curr_node = head_.load(std::memory_order_relaxed);
        while (curr_node) 
        {
            Node* next_node = curr_node->next.load(std::memory_order_relaxed);
            delete curr_node; // 這裡會呼叫 Node::operator delete 歸還給 Pool
            curr_node = next_node;
        }
    }

    bool enqueue(const T& v) 
    {
        auto token = Reclaimer::enter();
        if (is_closed()) return false;
        Node* new_node = new Node(v);
        SimpleBackoff bk;

        for (;;) 
        {
            Node* curr_tail = tail_.load(std::memory_order_acquire);
            Node* tail_next = curr_tail->next.load(std::memory_order_acquire);

            if (is_closed()) { delete new_node; return false; }

            if (curr_tail == tail_.load(std::memory_order_acquire)) 
            {
                if (tail_next == nullptr) 
                {
                    if (curr_tail->next.compare_exchange_weak(tail_next, 
                                                              new_node,
                                                              std::memory_order_release, 
                                                              std::memory_order_relaxed)) 
                    {
                        // 嘗試推進 tail（可能失敗）
                        Node* expected_tail = curr_tail;
                        (void)tail_.compare_exchange_strong(expected_tail, 
                                                            new_node,
                                                            std::memory_order_release, 
                                                            std::memory_order_relaxed);
                        return true;
                    }
                } 
                else 
                {
                    // tail 落後，幫其推進
                    Node* expected_tail = curr_tail;
                    tail_.compare_exchange_strong(expected_tail, 
                                                  tail_next,
                                                  std::memory_order_release, 
                                                  std::memory_order_relaxed);
                }
                bk.pause(); // 失敗時退避
            }
        }
    }

    bool try_dequeue(T& out)
    {
        auto token = Reclaimer::enter();
        SimpleBackoff bk;
        for (;;)
        {
            Node* curr_head = head_.load(std::memory_order_acquire);
            
            // [HP 關鍵步驟 1] 先掛上 Hazard Pointer 保護 curr_head
            Reclaimer::protect_at(0, curr_head);

            // [HP 關鍵步驟 2] 記憶體屏障 (Memory Fence)，確保 protect 指令先執行
            //std::atomic_thread_fence(std::memory_order_seq_cst); (protect改memory_order_seq_cst了 所以這邊可以不用)

            // [HP 關鍵步驟 3] 再次檢查 head 是否改變
            // 如果 head 已經變了，代表 curr_head 可能已經被別人 pop 並且 delete 掉了
            // 這時候我們的保護可能太晚了，必須重來
            if (curr_head != head_.load(std::memory_order_acquire))
            {
                // 失敗重試前，不需要清除保護，下次迴圈會覆蓋
                continue; 
            }

            Node* curr_tail = tail_.load(std::memory_order_acquire);
            Node* head_next = curr_head->next.load(std::memory_order_acquire);
            
            // 注意：這裡 next 也要保護嗎？
            // M&S Queue 的演算法特性是只要保護 Head 即可安全讀取 next
            // 因為如果 Head 沒變，Head 節點就不會被釋放，next 也就是安全的。
            
            if (head_next == nullptr)
            {
                Reclaimer::protect_at(0, nullptr); // 離開前清除保護
                return false;
            }
            
            // 2. [新增] 保護 Next 節點
            // 在讀取 value 之前，必須確保 next 節點不會被釋放
            Reclaimer::protect_at(1, head_next);

            // 3. [新增] 關鍵二次檢查
            // 我們保護了 head_next，但在保護指令生效前，head_next 可能已經被刪除了。
            // 只要確認 head 仍然是 curr_head，根據佇列特性，head 的 next 就必定還沒被完全 pop 出去。
            if (curr_head != head_.load(std::memory_order_acquire))
            {
                continue; // 重試
            }

            if (curr_head == curr_tail)
            {
                Node* expected_tail = curr_tail;
                tail_.compare_exchange_strong(expected_tail, 
                                              head_next, 
                                              std::memory_order_release, 
                                              std::memory_order_relaxed);
                bk.pause();
                 // 重試前清除保護(雖然不清除也行，但習慣好)
                Reclaimer::protect_at(0, nullptr);
                Reclaimer::protect_at(1, nullptr); 
                continue;
            }

            // 現在可以安全讀取了，因為 head_next 被我們保護著
            out = head_next->value;
            
            if (head_.compare_exchange_weak(curr_head, 
                                            head_next, 
                                            std::memory_order_release, 
                                            std::memory_order_relaxed))
            {            
                // 成功推進 head，可以回收舊 head
                Reclaimer::protect_at(0, nullptr); // 成功 dequeue，自己不用保護了
                Reclaimer::retire(curr_head);              // 丟給回收員
                return true;
            }
            
            // 迴圈重試，protect_at 會在開頭重設
            bk.pause();
        }
    }

    void close() 
    { 
        closed_.store(true, std::memory_order_release); 
    }

    bool is_closed() const noexcept 
    { 
        return closed_.load(std::memory_order_acquire); 
    }

    static void quiescent() noexcept 
    { 
        Reclaimer::quiescent(); 
    }

private:
    alignas(64) std::atomic<Node*> head_{nullptr};
    alignas(64) std::atomic<Node*> tail_{nullptr};
    alignas(64) std::atomic<bool>  closed_{false};
};

} // namespace mpmcq