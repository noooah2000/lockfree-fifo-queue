#pragma once
#include <queue>
#include <mutex>
#include <atomic>

namespace lfq {

template <class T>
class MPMCQueueMutex {
public:
  explicit MPMCQueueMutex(std::size_t /*capacity_hint*/ = 0) : closed_(false) {}

  bool enqueue(const T& v) {
    std::lock_guard<std::mutex> lk(m_);
    if (closed_) return false;
    q_.push(v);
    return true;
  }

  bool try_dequeue(T& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop();
    return true;
  }

  void close() {
    std::lock_guard<std::mutex> lk(m_);
    closed_ = true;
  }

  bool is_closed() const noexcept { return closed_.load(std::memory_order_relaxed); }
  static void quiescent() noexcept {} // 與 lock-free API 對齊

private:
  mutable std::mutex m_;
  std::queue<T> q_;
  std::atomic<bool> closed_;
};

} // namespace lfq
