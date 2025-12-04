#pragma once
#include <atomic>
#include <cstddef>
#include <utility>
#include <new>

namespace lfq {

// 極簡 Michael & Scott MPMC 骨架（為安全起見，目前 retire 直接 delete）
// 之後請將 Reclaimer 換成你完成的 HP/EBR 策略，修正 ABA/回收。
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
    Node* node = new Node(v);
    for (;;) {
      Node* t = tail_.load(std::memory_order_acquire);
      Node* next = t->next.load(std::memory_order_acquire);
      if (t == tail_.load(std::memory_order_acquire)) {
        if (next == nullptr) {
          if (t->next.compare_exchange_weak(next, node,
                 std::memory_order_release, std::memory_order_relaxed)) {
            // swing tail
            tail_.compare_exchange_strong(t, node,
                 std::memory_order_release, std::memory_order_relaxed);
            return true;
          }
        } else {
          // push tail forward
          tail_.compare_exchange_strong(t, next,
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
      if (h == head_.load(std::memory_order_acquire)) {
        if (next == nullptr) {
          // empty
          return false;
        }
        if (h == t) {
          // tail 落後，推進
          tail_.compare_exchange_strong(t, next,
               std::memory_order_release, std::memory_order_relaxed);
          continue;
        }
        // 取出值，head 前進
        out = next->value;
        if (head_.compare_exchange_weak(h, next,
               std::memory_order_release, std::memory_order_relaxed)) {
          // retire 舊 head（dummy）
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
  std::atomic<Node*> head_{nullptr};
  std::atomic<Node*> tail_{nullptr};
  std::atomic<bool>  closed_{false};
};

} // namespace lfq
