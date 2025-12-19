#pragma once

namespace mpmcq::reclaimer
{

/**
 * @brief No-Op Reclamation Strategy (Intentional Memory Leak).
 * * This strategy performs no memory reclamation whatsoever. Nodes removed from 
 * the queue are simply abandoned (leaked).
 * * **Purpose & Benchmarking Context:**
 * 1. **Memory Leak (Current Implementation):** * Acts as a performance baseline. It isolates the raw algorithmic overhead of 
 * the queue by removing the cost of memory reclamation and the "Cost of Malloc" 
 * (since nodes are not freed and re-allocated from the OS in a loop).
 * * 2. **Unsafe Reuse (ABA Risk):** * (If `delete` is enabled with an Object Pool): Nodes are recycled immediately. 
 * This is fast due to cache locality but unsafe in concurrent environments 
 * without additional mechanisms (leads to ABA problems).
 * * 3. **System Free:** * (If `delete` is enabled without an Object Pool): The slowest scenario. 
 * Bypasses pools and calls the OS `free` directly, causing severe scalability 
 * collapse due to global lock contention in the OS memory allocator.
 */
struct no_reclamation
{
    struct token { };

    // No-op: This strategy requires no quiescent state tracking, 
    // thread-local buffers, or background scanning.
    static void quiescent() noexcept
    {
    }

    static token enter() noexcept { return {}; }

    /**
     * @brief Retires a node by intentionally leaking it.
     * * This implementation does nothing.
     * - Safety: Absolute safety (no dangling pointers), assuming infinite memory.
     * - Performance: Maximum possible throughput for the queue logic itself.
     */
    template <class Node>
    static void retire(Node* /*p*/) noexcept 
    {
        // Intentionally leak memory.
    }

    // // Alternative Implementation: Unsafe Reuse / System Free
    // // Uncommenting the below line changes behavior to immediate reclamation.
    // template <class Node>
    // static void retire(Node *p) noexcept
    // {
    //     delete p;
    // }

    // No-op: No Hazard Pointers or protection slots are needed.
    // Kept for API compatibility with the Reclaimer concept (e.g., HazardPointer).
    static void protect_at(int, void*) {}   
};

} // namespace mpmcq::reclaimer