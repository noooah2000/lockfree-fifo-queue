#pragma once
#include <cstddef>
#include <vector>
#include <thread>
#include <chrono>

namespace lfq::reclaimer {

// 先行安全版：直接 delete，無 SMR。作為 bring-up / None 策略。
// 改進：加入簡單的延遲回收機制以減少 ABA 問題
struct no_reclamation {
  struct token { };               // 每執行緒上下文（此策略不需要）

  static thread_local std::vector<void*> retire_buffer;
  static constexpr int RETIRE_BUFFER_SIZE = 100;

  static void quiescent() noexcept {
    // 檢查緩衝區，延遲回收
    if (retire_buffer.size() > RETIRE_BUFFER_SIZE) {
      // 執行延遲回收（這是一個簡單啟發式方法，
      // 不如真正的 SMR，但減少立即 use-after-free 的風險）
      auto old_buffer = retire_buffer;
      retire_buffer.clear();
      
      // 給其他執行緒一點時間清理
      std::this_thread::sleep_for(std::chrono::microseconds(10));
      
      for (void* p : old_buffer) {
        delete[] static_cast<char*>(p);
      }
    }
  }

  static token enter() noexcept { return {}; }

  template <class Node>
  static void retire(Node* p) noexcept { 
    retire_buffer.push_back(static_cast<void*>(p));
    if (retire_buffer.size() % RETIRE_BUFFER_SIZE == 0) {
      quiescent();
    }
  }
};

thread_local std::vector<void*> no_reclamation::retire_buffer;

} // namespace lfq::reclaimer
