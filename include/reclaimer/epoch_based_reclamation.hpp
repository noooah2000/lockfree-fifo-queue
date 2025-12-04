#pragma once
#include <cstddef>

namespace lfq::reclaimer {

// 骨架：等價 no-op（後續你補 EBR/QSBR 的 epoch 與批次回收）
struct epoch_based_reclamation {
  struct token { };
  static void quiescent() noexcept {} // QSBR：呼叫代表 thread 進入 quiescent
  static token enter() noexcept { return {}; }
  template <class Node>
  static void retire(Node* p) noexcept { delete p; } // TODO: 加入 epoch list, 觸發批次回收
};

} // namespace lfq::reclaimer
