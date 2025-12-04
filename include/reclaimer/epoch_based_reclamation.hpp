#pragma once
#include <atomic>
#include <vector>
#include <thread>
#include <cstddef>
#include <mutex>

namespace lfq::reclaimer {

// 基於 Epoch 的回收機制（簡化版 EBR/QSBR）
// 思路：全域 epoch 計數器，每個執行緒記錄自己在讀時的 epoch，
//      當一個 epoch 不被任何執行緒使用時，可安全回收該 epoch 內登記的節點

constexpr int EBR_RETIRE_THRESHOLD = 50;

class EpochBasedReclaimationManager {
public:
  static EpochBasedReclaimationManager& instance() {
    static EpochBasedReclaimationManager mgr;
    return mgr;
  }

  struct ThreadContext {
    int current_epoch = 0;
    bool active = false;           // 執行緒是否在臨界區
    std::vector<void*> retire_list; // 待回收節點列表
  };

  static ThreadContext& get_context() {
    thread_local ThreadContext ctx;
    return ctx;
  }

  // 執行緒進入臨界區
  void enter_critical() noexcept {
    auto& ctx = get_context();
    ctx.current_epoch = global_epoch_.load(std::memory_order_acquire);
    ctx.active = true;
  }

  // 執行緒離開臨界區（進入 quiescent state）
  void exit_critical() noexcept {
    auto& ctx = get_context();
    ctx.active = false;
  }

  // 登記節點待回收（當前 epoch）
  void retire_node(void* ptr) noexcept {
    auto& ctx = get_context();
    ctx.retire_list.push_back(ptr);
    
    if (ctx.retire_list.size() >= EBR_RETIRE_THRESHOLD) {
      scan_and_retire();
    }
  }

  // QSBR：呼叫此函式表示執行緒進入 quiescent state
  void quiescent_state() noexcept {
    auto& ctx = get_context();
    ctx.active = false;
    
    // 簡化版：累積到一定數量後執行回收
    if (ctx.retire_list.size() >= EBR_RETIRE_THRESHOLD) {
      scan_and_retire();
    }
  }

  // 掃描並回收可回收節點
  void scan_and_retire() noexcept {
    auto& ctx = get_context();
    
    // 簡化策略：延遲批次回收
    // 實際 EBR 會追蹤 epoch，但這個版本使用啟發式方法
    if (ctx.retire_list.size() >= EBR_RETIRE_THRESHOLD * 2) {
      for (void* p : ctx.retire_list) {
        delete[] static_cast<char*>(p);
      }
      ctx.retire_list.clear();
    }
  }

private:
  std::atomic<int> global_epoch_{0};
  
  EpochBasedReclaimationManager() = default;
};

struct epoch_based_reclamation {
  struct token { };

  static void quiescent() noexcept {
    EpochBasedReclaimationManager::instance().quiescent_state();
  }

  static token enter() noexcept {
    EpochBasedReclaimationManager::instance().enter_critical();
    return {};
  }

  template <class Node>
  static void retire(Node* p) noexcept {
    EpochBasedReclaimationManager::instance().retire_node(static_cast<void*>(p));
  }
};

} // namespace lfq::reclaimer
