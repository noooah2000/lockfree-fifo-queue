#pragma once
#include <cstddef>

namespace lfq::reclaimer {

// 骨架：先與 no_reclamation 等價（後續你填入 HP 真正實作）
struct hazard_pointers {
  struct token { };
  static void quiescent() noexcept {}
  static token enter() noexcept { return {}; }
  template <class Node>
  static void retire(Node* p) noexcept { delete p; } // TODO: HP retire → retire list + scan
};

} // namespace lfq::reclaimer
