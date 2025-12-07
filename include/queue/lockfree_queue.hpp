#pragma once
#include <atomic>
#include <cstddef>
#include <utility>
#include <new>
#include <thread>

namespace lfq {

// Michael & Scott MPMC FIFO Queue with SMR support
// 使用 Reclaimer 策略來防止 use-after-free 和 ABA 問題
template <class T, class Reclaimer>
class MPMCQueue {
  struct Node {
    std::atomic<Node*> next{nullptr};
    T value;
    explicit Node(const T& v) : next(nullptr), value(v) {}
    Node() : next(nullptr), value() {}
  };

public:
  explicit MPMCQueue(std::size_t /*cap_hint*/ = 0) {
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
      delete n;
      n = nxt;
    }
  }

  bool enqueue(const T& v) {
    if (closed_.load(std::memory_order_acquire)) return false;
    Node* node = new Node(v);   // TODO: 有 lock !!!
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
          Reclaimer::retire(h);
          return true;
        }
      }
    }
  }

  void close() { 
    closed_.store(true, std::memory_order_release); 
  }

  bool is_closed() const noexcept { 
    return closed_.load(std::memory_order_acquire); 
  }

  static void quiescent() noexcept { 
    Reclaimer::quiescent(); 
  }

private:
  // 一般 CPU Cache Line 是 64 bytes
  alignas(64) std::atomic<Node*> head_{nullptr};
  alignas(64) std::atomic<Node*> tail_{nullptr};
  alignas(64) std::atomic<bool>  closed_{false};
};

} // namespace lfq
