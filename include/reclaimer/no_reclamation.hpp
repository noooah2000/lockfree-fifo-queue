#pragma once
#include <cstddef>

namespace lfq::reclaimer {

// 先行安全版：直接 delete，無 SMR。作為 bring-up / None 策略。
struct no_reclamation {
  struct token { };               // 每執行緒上下文（此策略不需要）
  static void quiescent() noexcept {}       // QSBR 無動作
  static token enter() noexcept { return {}; } // 進入臨界區（無動作）
  template <class Node>
  static void retire(Node* p) noexcept { delete p; } // 直接釋放（不安全於真 M&S）
};

} // namespace lfq::reclaimer
