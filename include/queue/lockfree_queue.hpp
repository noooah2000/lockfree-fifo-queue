#pragma once
#include <atomic>
#include <cstddef>
#include <utility>
#include <new>
#include <thread>
#include <vector>
#include <mutex>

namespace mpmcq 
{

template <typename Node> class NodePool;

// Michael & Scott MPMC FIFO Queue with SMR support
// using Reclaimer scheme to prevent use-after-free and ABA problems
template <class T, class Reclaimer>
<<<<<<< HEAD
class MPMCQueue {
public: // 改成 public，讓 Reclaimer 可以存取 Node
  struct Node {
    std::atomic<Node*> next{nullptr};
    T value;
    
=======
class LockFreeQueue 
{
  struct Node 
  {
    std::atomic<Node*> next{nullptr};
    T value;

>>>>>>> 3b51fd1 (WIP: save work before update)
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
<<<<<<< HEAD
  explicit MPMCQueue(std::size_t /*cap_hint*/ = 0) {
    // 這裡呼叫 new，會自動走 operator new -> NodePool::alloc
    Node* dummy = new Node();      // 初始 dummy
=======
  explicit LockFreeQueue(std::size_t /*cap_hint*/ = 0) 
  {
    Node* dummy = new Node();      // init dummy
>>>>>>> 3b51fd1 (WIP: save work before update)
    head_.store(dummy, std::memory_order_relaxed);
    tail_.store(dummy, std::memory_order_relaxed);
    closed_.store(false, std::memory_order_relaxed);
  }

  ~LockFreeQueue() 
  {
    // 清空殘留節點
<<<<<<< HEAD
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
=======
    Node* curr_node = head_.load(std::memory_order_relaxed);
    while (node) 
    {
      Node* next_node = curr_node->next.load(std::memory_order_relaxed);
      delete curr_node;
      curr_node = next_node;
    }
  }

  bool enqueue(const T& v) 
  {
    if (is_closed()) return false;
    Node* new_node = new Node(v);

    for (;;) 
    {
      Node* curr_tail = tail_.load(std::memory_order_acquire);
      Node* tail_next = curr_tail->next.load(std::memory_order_acquire);

      if (is_closed()) {delete new_node; return false;}

      if (curr_tail == tail_.load(std::memory_order_acquire)) 
      {
        if (tail_next == nullptr) 
        {
          if (curr_tail->next.compare_exchange_weak(tail_next, 
                                                    new_node,
                                                    std::memory_order_release, 
                                                    std::memory_order_relaxed)) 
          {
            // update tail
            Node* expected_tail = curr_tail;
            tail_.compare_exchange_strong(expected_tail, 
                                          new_node,
                                          std::memory_order_release, 
                                          std::memory_order_relaxed);
>>>>>>> 3b51fd1 (WIP: save work before update)
            return true;
          }
        } 
        else 
        {
          // update tail
          Node* expected_tail = curr_tail;
          tail_.compare_exchange_strong(expected_tail, 
                                        tail_next,
                                        std::memory_order_release, 
                                        std::memory_order_relaxed);
        }
      }
    }
  }

  bool try_dequeue(T& out) 
  {
    for (;;) 
    {
      Node* curr_head = head_.load(std::memory_order_acquire);
      Node* curr_tail = tail_.load(std::memory_order_acquire);
      Node* head_next = curr_head->next.load(std::memory_order_acquire);
      
      if (curr_head == head_.load(std::memory_order_acquire)) 
      {
        if (head_next == nullptr) return false; // queue is empty

        if (curr_head == curr_tail) // there is a producer that haven't updated the tial
        {
          // consumer help updating the tail
          Node* expected_tail = curr_tail;
          tail_.compare_exchange_strong(expected_tail, 
                                        head_next,
                                        std::memory_order_release, 
                                        std::memory_order_relaxed);
          continue;
        }
        
        // 安全複製值（在刪除前）
        out = head_next->value;
        
        // update head
        if (head_.compare_exchange_weak(curr_head, 
                                        head_next,
                                        std::memory_order_release, 
                                        std::memory_order_relaxed)) 
        {
          // 成功推進 head，可以回收舊 head
<<<<<<< HEAD
          // Reclaimer 內部會呼叫 delete h
          // 因為重載了 operator delete，所以會自動歸還給 Pool
          Reclaimer::retire(h);
=======
          Reclaimer::retire(curr_head);
>>>>>>> 3b51fd1 (WIP: save work before update)
          return true;
        }
      }
    }
  }

<<<<<<< HEAD
  void close() { closed_.store(true, std::memory_order_release); }
  bool is_closed() const noexcept { return closed_.load(std::memory_order_acquire); }
  static void quiescent() noexcept { Reclaimer::quiescent(); }
=======
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
>>>>>>> 3b51fd1 (WIP: save work before update)

private:
  // 一般 CPU Cache Line 是 64 bytes
  alignas(64) std::atomic<Node*> head_{nullptr};
  alignas(64) std::atomic<Node*> tail_{nullptr};
  alignas(64) std::atomic<bool>  closed_{false};
};

<<<<<<< HEAD
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
template <typename Node>
thread_local std::vector<Node*> NodePool<Node>::local_cache;

template <typename Node>
std::mutex NodePool<Node>::global_mtx;

template <typename Node>
std::vector<Node*> NodePool<Node>::global_pool;

} // namespace lfq
=======
} // namespace mpmcq 
>>>>>>> 3b51fd1 (WIP: save work before update)
