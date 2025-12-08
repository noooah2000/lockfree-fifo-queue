#pragma once
/**
 * 1. Memory Leak (Default):
 *      不 delete:
 *          作為 Baseline，解釋了 "Cost of Malloc"
 *          (Pool 耗盡後被迫頻繁呼叫系統 malloc，撞上全域鎖與 System Call 瓶頸)
 * 2. Unsafe Reuse (有 ABA 問題):
 *      有 delete, 有 delete 多載:
 *          啟用 Object Pool 機制。記憶體被回收至 User-Space 的 Pool 而非還給 OS，
 *          大幅減少 malloc/free 鎖競爭，並最大化 Cache Locality (熱資料重用)。
 * 3. System Free:
 *      有 delete, 無 delete 多載:
 *          最慢的情境。繞過 Pool 直接呼叫系統 free。
 *          每次 Enqueue/Dequeue 都觸發 OS 記憶體管理器的 Global Lock 競爭，
 *          導致嚴重的 Scalability 崩潰 (比 "不 delete" 還慢，因為多了 free 的開銷)。
 */
namespace mpmcq::reclaimer
{
struct no_reclamation
{
    struct token
    {
    };

    // 不需要任何狀態，不需要 thread_local buffer
    static void quiescent() noexcept
    {
        // 空實作
    }

    static token enter() noexcept { return {}; }

    //   Memory Leak (Default)
    template <class Node>
    static void retire(Node* /*p*/) noexcept {
        // 什麼都不做，讓它洩漏
        // 這樣最安全，也最單純
    }

    // //   Unsafe Reuse / System Free
    // template <class Node>
    // static void retire(Node *p) noexcept
    // {
    //     delete p;
    // }

    static void protect_at(int, void*) {}   // 對齊 HP 實作
};

} // namespace mpmcq::reclaimer