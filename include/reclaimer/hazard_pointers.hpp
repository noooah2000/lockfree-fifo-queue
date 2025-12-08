#pragma once
#include <atomic>
#include <vector>
#include <thread>
#include <cstddef>
#include <mutex>

namespace mpmcq::reclaimer {

// Hazard Pointers 實現（簡化版）
// 每個執行緒最多同時持有 K 個危險指針
constexpr int HP_COUNT_PER_THREAD = 3;  // M&S queue 最多需要 3 個指針
constexpr int HP_RETIRE_THRESHOLD = 50; // 達到此數量時觸發掃描

class HazardPointerManager {
public:
  static HazardPointerManager& instance() {
    static HazardPointerManager mgr;
    return mgr;
  }

  struct ThreadContext {
    std::vector<std::atomic<void*>> hp;  // Hazard pointers
    std::vector<void*> retire_list;       // 待回收節點列表
    
    ThreadContext() : hp(HP_COUNT_PER_THREAD), retire_list() {
      for (auto& h : hp) h.store(nullptr, std::memory_order_relaxed);
    }
  };

  static ThreadContext& get_context() {
    thread_local ThreadContext ctx;
    return ctx;
  }

  // 標記指針為危險
  void protect(int idx, void* ptr) noexcept {
    auto& ctx = get_context();
    if (idx < HP_COUNT_PER_THREAD) {
      ctx.hp[idx].store(ptr, std::memory_order_release);
    }
  }

  // 清除危險標記
  void clear(int idx) noexcept {
    auto& ctx = get_context();
    if (idx < HP_COUNT_PER_THREAD) {
      ctx.hp[idx].store(nullptr, std::memory_order_release);
    }
  }

  // 登記節點待回收
  void retire_node(void* ptr) noexcept {
    auto& ctx = get_context();
    ctx.retire_list.push_back(ptr);
    if (ctx.retire_list.size() >= HP_RETIRE_THRESHOLD) {
      scan_and_retire();
    }
  }

  // 掃描所有活躍危險指針並回收
  void scan_and_retire() noexcept {
    auto& ctx = get_context();
    // 簡化版：延遲回收（在實際 HP 中應掃描所有執行緒的危險指針）
    // 當 retire_list 足夠大時，執行大批回收
    if (ctx.retire_list.size() >= HP_RETIRE_THRESHOLD * 2) {
      for (void* p : ctx.retire_list) {
        delete[] static_cast<char*>(p);
      }
      ctx.retire_list.clear();
    }
  }

private:
  HazardPointerManager() = default;
};

struct hazard_pointers {
  struct token { };
  
  static void quiescent() noexcept {
    // 清除所有危險指針
    auto& mgr = HazardPointerManager::instance();
    for (int i = 0; i < HP_COUNT_PER_THREAD; i++) {
      mgr.clear(i);
    }
    mgr.scan_and_retire(); // 嘗試回收待回收節點
  }

  static token enter() noexcept { return {}; }

  template <class Node>
  static void retire(Node* p) noexcept {
    HazardPointerManager::instance().retire_node(static_cast<void*>(p));
  }

  // 用於保護指針避免被回收
  static void protect_at(int idx, void* ptr) noexcept {
    HazardPointerManager::instance().protect(idx, ptr);
  }
};

} // namespace mpmcq::reclaimer
