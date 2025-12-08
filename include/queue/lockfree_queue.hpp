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
class NodePool {
public:
    // C++17 inline static: 不需要再到 class 外面定義了，超乾淨！
    inline static thread_local std::vector<Node*> local_pool;
    inline static std::mutex global_pool_mutex;
    inline static std::vector<Node*> global_pool;

    static Node* allocate() 
    {
        // 1. 查本地背包
        if (!local_pool.empty()) 
        {
            Node* new_node = local_pool.back();
            local_pool.pop_back();
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
                    local_pool.push_back(global_pool.back());
                    global_pool.pop_back();
                    count++;
                }
            }
        }

        // 3. 搬完後再查一次本地
        if (!local_pool.empty()) 
        {
            Node* new_node = local_pool.back();
            local_pool.pop_back();
            return new_node;
        }
        
        // 4. 真的沒貨，跟 OS 要記憶體 (使用 ::operator new 防止遞迴)
        return static_cast<Node*>(::operator new(sizeof(Node)));
    }

    static void deallocate(Node* recycled_node) 
    {
        // 重置 next 指標，防止髒數據
        recycled_node->next.store(nullptr, std::memory_order_relaxed);
        
        // 如果本地積太多 (>64)，還一半給全域
        if (local_pool.size() > 64) 
        {
            std::lock_guard<std::mutex> lock(global_pool_mutex);
            for (int i=0; i<32; ++i) 
            {
                global_pool.push_back(local_pool.back());
                local_pool.pop_back();
            }
        }
        // 放回本地
        local_pool.push_back(recycled_node);
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
        Node* dummy = new Node();      // init dummy
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
        SimpleBackoff bk;
        for (;;) 
        {
            Node* curr_head = head_.load(std::memory_order_acquire);
            Node* curr_tail = tail_.load(std::memory_order_acquire);
            Node* head_next = curr_head->next.load(std::memory_order_acquire);
            
            // 驗證一致性：head 在讀取期間未變
            if (curr_head == head_.load(std::memory_order_acquire)) 
            {
                // 佇列為空
                if (head_next == nullptr) return false;

                if (curr_head == curr_tail) 
                {
                    // tail 落後，幫其推進
                    Node* expected_tail = curr_tail;
                    (void)tail_.compare_exchange_strong(expected_tail, 
                                                        head_next,
                                                        std::memory_order_release, 
                                                        std::memory_order_relaxed);
                    bk.pause();
                    continue;
                }
                
                out = head_next->value;
                
                // 嘗試推進 head
                if (head_.compare_exchange_weak(curr_head, 
                                                head_next,
                                                std::memory_order_release, 
                                                std::memory_order_relaxed)) 
                {
                    // 成功推進 head，可以回收舊 head
                    Reclaimer::retire(curr_head);
                    return true;
                }
                // CAS 失敗，休息一下
                bk.pause();
            }
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