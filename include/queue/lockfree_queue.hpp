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

namespace lfq {

template <typename Node> class NodePool;

// Michael & Scott MPMC FIFO Queue with SMR support
// 使用 Reclaimer 策略來防止 use-after-free 和 ABA 問題
template <class T, class Reclaimer>
class MPMCQueue {
public: // 改成 public，讓 Reclaimer 可以存取 Node
    struct Node {
        std::atomic<Node*> next{nullptr};
        T value;
    
        explicit Node(const T& v) : next(nullptr), value(v) {}
        Node() : next(nullptr), value() {}

        // 重載 new/delete，讓所有透過 new/delete 的操作自動走 Object Pool
        // 這讓 Reclaimer (EBR/HP) 不需要修改程式碼就能享受到 Pool 的加速
        void* operator new(size_t) {
                return NodePool<Node>::alloc();
        }

        void operator delete(void* p) {
                NodePool<Node>::free(static_cast<Node*>(p));
        }
    };

public:
    explicit MPMCQueue(std::size_t /*cap_hint*/ = 0) {
        // 這裡呼叫 new，會自動走 operator new -> NodePool::alloc
        Node* dummy = new Node();      // 初始 dummy
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
        closed_.store(false, std::memory_order_relaxed);
    }

    ~MPMCQueue() {
        // 清空殘留節點
        Node* n = head_.load(std::memory_order_relaxed);
        while (n) {
            Node* nxt = n->next.load(std::memory_order_relaxed);
            delete n; // 自動走 operator delete -> NodePool::free
            n = nxt;
        }
    }

    bool enqueue(const T& v) {
        if (closed_.load(std::memory_order_acquire)) return false;
    
        // 用標準寫法，因為我們重載了 new，它底層已經是 Pool 了
        // 這樣寫程式碼更乾淨，且與標準 M&S 演算法一致
        Node* node = new Node(v);
        // 或是
        // Node* node = NodePool<Node>::alloc();
        // new (node) Node(v); // Placement new

        for (;;) {
            Node* t = tail_.load(std::memory_order_acquire);
            Node* next = t->next.load(std::memory_order_acquire);
            if (t == tail_.load(std::memory_order_acquire)) {
                if (next == nullptr) {
                    if (t->next.compare_exchange_weak(next, node,
                                 std::memory_order_release, std::memory_order_relaxed)) {
                        // 嘗試推進 tail（可能失敗）
                        Node* expected_tail = t;
                        tail_.compare_exchange_strong(expected_tail, node,
                                 std::memory_order_release, std::memory_order_relaxed);
                        return true;
                    }
                } else {
                    // tail 落後，幫其推進
                    Node* expected_tail = t;
                    tail_.compare_exchange_strong(expected_tail, next,
                             std::memory_order_release, std::memory_order_relaxed);
                }
            }
        }
    }

    bool try_dequeue(T& out) {
        for (;;) {
            Node* h = head_.load(std::memory_order_acquire);
            Node* t = tail_.load(std::memory_order_acquire);
            Node* next = h->next.load(std::memory_order_acquire);
      
            // 驗證一致性：head 在讀取期間未變
            if (h == head_.load(std::memory_order_acquire)) {
                if (next == nullptr) {
                    // 佇列為空
                    return false;
                }
        
                if (h == t) {
                    // tail 落後，幫其推進
                    Node* expected_tail = t;
                    tail_.compare_exchange_strong(expected_tail, next,
                             std::memory_order_release, std::memory_order_relaxed);
                    continue;
                }
        
                // 安全複製值（在刪除前）
                out = next->value;
        
                // 嘗試推進 head
                if (head_.compare_exchange_weak(h, next,
                             std::memory_order_release, std::memory_order_relaxed)) {
                    // 成功推進 head，可以回收舊 head
                    // Reclaimer 內部會呼叫 delete h
                    // 因為重載了 operator delete，所以會自動歸還給 Pool
                    Reclaimer::retire(h);
                    return true;
                }
            }
        }
    }

    void close() { closed_.store(true, std::memory_order_release); }
    bool is_closed() const noexcept { return closed_.load(std::memory_order_acquire); }
    static void quiescent() noexcept { Reclaimer::quiescent(); }

private:
    // 一般 CPU Cache Line 是 64 bytes
    alignas(64) std::atomic<Node*> head_{nullptr};
    alignas(64) std::atomic<Node*> tail_{nullptr};
    alignas(64) std::atomic<bool>  closed_{false};
};

// Object Pool 實作 (解決 new 的 Malloc 瓶頸)
template <typename Node>
class NodePool {
public:
        // 每個執行緒有個小倉庫
        static thread_local std::vector<Node*> local_cache;
        // 全域大倉庫 (還是需要鎖，但頻率低很多)
        static std::mutex global_mtx;
        static std::vector<Node*> global_pool;

        static Node* alloc() {
                if (!local_cache.empty()) {
                        Node* n = local_cache.back();
                        local_cache.pop_back();
                        return n;
                }
                // 本地沒了，去全域搬貨 (一次搬 32 個，減少搶鎖次數)
                {
                        std::lock_guard<std::mutex> lk(global_mtx);
                        if (!global_pool.empty()) {
                                int count = 0;
                                while(!global_pool.empty() && count < 32) {
                                        local_cache.push_back(global_pool.back());
                                        global_pool.pop_back();
                                        count++;
                                }
                        }
                }
                // 如果還是空的，真的 new 一個
                if (!local_cache.empty()) {
                        Node* n = local_cache.back();
                        local_cache.pop_back();
                        return n;
                }
        
                // 真的需要記憶體時才用全域 new
                // 這裡不能用 new Node() 否則會遞迴呼叫 operator new，要用全域 ::new
                return static_cast<Node*>(::operator new(sizeof(Node)));
        }

        static void free(Node* n) {
                // 重要：重置 next 指標，防止髒數據
                n->next.store(nullptr, std::memory_order_relaxed);
        
                // 如果本地太多，還一些給全域
                if (local_cache.size() > 64) {
                        std::lock_guard<std::mutex> lk(global_mtx);
                        // 搬一半過去
                        for (int i=0; i<32; ++i) {
                                global_pool.push_back(local_cache.back());
                                local_cache.pop_back();
                        }
                }
                local_cache.push_back(n);
        }
};

// 必須在 Class 外部定義 static 成員，否則 Linker 會噴錯
    void close() 
thread_local std::vector<Node*> NodePool<Node>::local_cache;

template <typename Node>
std::mutex NodePool<Node>::global_mtx;

template <typename Node>
std::vector<Node*> NodePool<Node>::global_pool;

} // namespace lfq
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