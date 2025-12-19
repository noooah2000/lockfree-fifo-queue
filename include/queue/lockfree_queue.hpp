#pragma once
#include <atomic>
#include <cstddef>
#include <utility>
#include <new>
#include <thread>
#include <vector>
#include <mutex>

// ==========================================
// Address Sanitizer (ASan) Hooks
// ==========================================
#ifndef __has_feature
    #define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
    #include <sanitizer/asan_interface.h>
    // Mark memory as poisoned (inaccessible) when a node is reclaimed.
    #define ASAN_POISON_NODE(ptr, size) ASAN_POISON_MEMORY_REGION(ptr, size)
    // Unpoison memory (accessible) when a node is allocated.
    #define ASAN_UNPOISON_NODE(ptr, size) ASAN_UNPOISON_MEMORY_REGION(ptr, size)
#else
    #define ASAN_POISON_NODE(ptr, size)
    #define ASAN_UNPOISON_NODE(ptr, size)
#endif

// Ensure C++17 or later
#if __cplusplus < 201703L
    #error "This header requires C++17 or later (use -std=c++17)"
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
#endif

// Platform-independent CPU relaxation (pause/yield)
inline void cpu_relax() noexcept 
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause(); 
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#elif defined(__powerpc__) || defined(__ppc__)
    asm volatile("or 27,27,27" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

namespace mpmcq 
{

/**
 * @brief Exponential backoff strategy with jitter to reduce contention.
 * * This utility helps alleviate bus contention during high-concurrency 
 * scenarios by introducing random delays (jitter) and exponentially 
 * increasing wait times before yielding the CPU.
 */
struct SimpleBackoff 
{
    int n = 1;
    static constexpr int MAX_YIELD = 2048; 

    inline void pause() noexcept 
    {
#ifdef LFQ_USE_BACKOFF
        if (n <= MAX_YIELD) 
        {
            // Use Xorshift algorithm for fast thread-local random number generation.
            // This is significantly faster than rand() or std::random_device.
            static thread_local uint32_t seed = 0x9E3779B9; // Golden ratio as initial seed
            
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;

            // Calculate jitter to avoid lockstep (thundering herd problem).
            // Range: [n, 2n - 1]
            int jitter_cycles = n + (seed & (n - 1)); 

            for (int i = 0; i < jitter_cycles; ++i) {
                cpu_relax();
            }
            n <<= 1; 
        } 
        else 
        {
            // Yield CPU time slice if contention persists
            std::this_thread::yield();
            n = 1; 
        }
#endif
    }
};

// ==========================================
// Object Pool Configuration
// ==========================================
constexpr size_t POOL_BATCH_SIZE = 4096;   // Number of nodes exchanged with global pool
constexpr size_t POOL_LOCAL_CAP  = 65536;  // Local buffer capacity (must be > BATCH_SIZE)

/**
 * @brief High-performance thread-local object pool.
 * * Designed to minimize allocator contention (malloc/free) in high-throughput
 * scenarios. It uses thread-local fixed-size buffers and a shared global 
 * pool for rebalancing.
 * * @tparam Node The type of object to manage.
 */
template <typename Node>
class NodePool 
{
    struct LocalBuffer;

    // Track if the thread-local buffer is alive.
    // Set to 'this' on construction, nullptr on destruction.
    inline static thread_local LocalBuffer* local_pool_ptr = nullptr;

    // Atomic counter for dirty checking to avoid unnecessary lock contention.
    inline static std::atomic<size_t> global_count{0};

    // Thread-local buffer management
    struct LocalBuffer 
    {
        // Fixed-size array to avoid dynamic allocation overhead of std::vector.
        Node* nodes[POOL_LOCAL_CAP];
        size_t top = 0; 
        
        LocalBuffer() {
            local_pool_ptr = this;
        }

        ~LocalBuffer() 
        {
            local_pool_ptr = nullptr; 
            
            // Return remaining nodes to the global pool upon thread exit
            if (top > 0)
            {
                std::lock_guard<std::mutex> lock(NodePool::global_pool_mutex);
                for (size_t i = 0; i < top; ++i) 
                {
                    NodePool::global_pool.push_back(nodes[i]);
                }
                NodePool::global_count.fetch_add(top, std::memory_order_relaxed);
            }
        }

        bool empty() const { return top == 0; }
        
        // Suggest flushing when the buffer is near capacity
        bool should_flush() const { return top >= (POOL_LOCAL_CAP - 16); }
        
        void push(Node* ptr) { nodes[top++] = ptr; }
        Node* pop() { return nodes[--top]; }
        size_t size() const { return top; }
    };

public:
    inline static thread_local LocalBuffer local_pool;
    inline static std::mutex global_pool_mutex;
    inline static std::vector<Node*> global_pool;

    static Node* allocate() 
    {
        // 1. Fast path: Allocate from local buffer (no lock)
        if (!local_pool.empty()) 
        {
            Node* new_node = local_pool.pop();
            ASAN_UNPOISON_NODE(new_node, sizeof(Node));
            return new_node;
        }

        // 2. Optimization: Dirty check global count
        // If global pool is likely empty, skip the lock and go directly to OS allocation.
        if (global_count.load(std::memory_order_relaxed) >= POOL_BATCH_SIZE)
        {
            // 3. Slow path: Refill from global pool
            std::lock_guard<std::mutex> lock(global_pool_mutex);
            if (!global_pool.empty()) 
            {
                size_t count = 0;
                while(!global_pool.empty() && count < POOL_BATCH_SIZE) 
                {
                    local_pool.push(global_pool.back());
                    global_pool.pop_back();
                    count++;
                }
                if (count > 0) {
                    global_count.fetch_sub(count, std::memory_order_relaxed);
                }
            }
        }

        // 4. Retry local allocation after refill attempt
        if (!local_pool.empty()) 
        {
            Node* new_node = local_pool.pop();
            ASAN_UNPOISON_NODE(new_node, sizeof(Node));
            return new_node;
        }
        
        // 5. Fallback: Allocate from OS to prevent recursion
        return static_cast<Node*>(::operator new(sizeof(Node)));
    }

    static void deallocate(Node* recycled_node) 
    {
        // Ensure the local pool is still alive (thread is not exiting)
        if (local_pool_ptr) 
        {
            // Flush to global pool if local buffer is too full
            if (local_pool_ptr->should_flush()) 
            {
                std::lock_guard<std::mutex> lock(global_pool_mutex);
                size_t moved_count = 0;
                for (size_t i=0; i < POOL_BATCH_SIZE; ++i) 
                {
                    if (local_pool_ptr->empty()) break;
                    Node* node_to_return = local_pool_ptr->pop();
                    global_pool.push_back(node_to_return);
                    moved_count++;
                }
                if (moved_count > 0) {
                    global_count.fetch_add(moved_count, std::memory_order_relaxed);
                }
            }
            
            // Push to local buffer
            local_pool_ptr->push(recycled_node);
            ASAN_POISON_NODE(recycled_node, sizeof(Node));
        }
        else 
        {
            // Fallback: Direct delete if thread pool is destroyed
            ::operator delete(recycled_node);
        }
    }
};

/**
 * @brief A lock-free MPMC queue based on the Michael & Scott algorithm.
 * * This implementation supports pluggable Safe Memory Reclamation (SMR) strategies
 * (e.g., Hazard Pointers, Epoch-Based Reclamation) via the `Reclaimer` template
 * parameter. It also optionally integrates with a `NodePool` for memory optimization.
 * * @tparam T The type of elements stored in the queue.
 * @tparam Reclaimer The SMR policy to use for memory safety.
 */
template <class T, class Reclaimer>
class LockFreeQueue 
{
    struct Node 
    {
        std::atomic<Node*> next{nullptr};
        T value;

        explicit Node(const T& v) : next(nullptr), value(v) {}
        Node() : next(nullptr), value() {}

#ifdef LFQ_USE_NODEPOOL
        void* operator new(size_t) 
        {
            return NodePool<Node>::allocate();
        }

        void operator delete(void* p) 
        {
            NodePool<Node>::deallocate(static_cast<Node*>(p));
        }
#endif
    };

public:
    explicit LockFreeQueue(std::size_t /*cap_hint*/ = 0) 
    {
        Node* dummy = new Node(); // Dummy sentinel node
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
        closed_.store(false, std::memory_order_relaxed);
    }

    ~LockFreeQueue() 
    {
        Node* curr_node = head_.load(std::memory_order_relaxed);
        while (curr_node) 
        {
            Node* next_node = curr_node->next.load(std::memory_order_relaxed);
            delete curr_node;
            curr_node = next_node;
        }
    }

    bool enqueue(const T& v) 
    {
        [[maybe_unused]] auto token = Reclaimer::enter();
        if (is_closed()) return false;
        Node* new_node = new Node(v);
        SimpleBackoff bk;

        for (;;) 
        {
            Node* curr_tail = tail_.load(std::memory_order_acquire);
            
            // Protect tail to prevent reclamation while accessing
            Reclaimer::protect_at(0, curr_tail);

            // Validation: Ensure tail hasn't changed during protection
            if (curr_tail != tail_.load(std::memory_order_acquire)) 
            {
                continue;
            }

            // Safe to access next pointer now
            Node* tail_next = curr_tail->next.load(std::memory_order_acquire);

            if (is_closed()) 
            { 
                Reclaimer::protect_at(0, nullptr);
                delete new_node; 
                return false; 
            }

            if (curr_tail == tail_.load(std::memory_order_acquire)) 
            {
                if (tail_next == nullptr) 
                {
                    // Try to link new node to the end
                    if (curr_tail->next.compare_exchange_weak(tail_next, 
                                                              new_node,
                                                              std::memory_order_release, 
                                                              std::memory_order_relaxed)) 
                    {
                        // Try to advance tail (best effort)
                        Node* expected_tail = curr_tail;
                        (void)tail_.compare_exchange_strong(expected_tail, 
                                                            new_node,
                                                            std::memory_order_release, 
                                                            std::memory_order_relaxed);
                        
                        Reclaimer::protect_at(0, nullptr);
                        return true;
                    }
                } 
                else 
                {
                    // Tail is lagging, help advance it
                    Node* expected_tail = curr_tail;
                    tail_.compare_exchange_strong(expected_tail, 
                                                  tail_next,
                                                  std::memory_order_release, 
                                                  std::memory_order_relaxed);
                }
                bk.pause();
            }
        }
    }

    bool try_dequeue(T& out)
    {
        [[maybe_unused]] auto token = Reclaimer::enter();
        SimpleBackoff bk;
        for (;;)
        {
            Node* curr_head = head_.load(std::memory_order_acquire);
            
            // HP Step 1: Protect current head
            Reclaimer::protect_at(0, curr_head);

            // HP Step 2: Validate head hasn't changed
            if (curr_head != head_.load(std::memory_order_acquire))
            {
                continue; 
            }

            Node* curr_tail = tail_.load(std::memory_order_acquire);
            Node* head_next = curr_head->next.load(std::memory_order_acquire);
            
            if (head_next == nullptr)
            {
                Reclaimer::protect_at(0, nullptr);
                Reclaimer::protect_at(1, nullptr);
                return false;
            }
            
            // HP Step 3: Protect next node before accessing value
            Reclaimer::protect_at(1, head_next);

            // HP Step 4: Double check head consistency
            // Ensure head_next is still valid relative to head
            if (curr_head != head_.load(std::memory_order_acquire))
            {
                continue;
            }

            // Handle condition where queue looks empty but has a lagging tail
            if (curr_head == curr_tail)
            {
                Node* expected_tail = curr_tail;
                tail_.compare_exchange_strong(expected_tail, 
                                              head_next, 
                                              std::memory_order_release, 
                                              std::memory_order_relaxed);
                bk.pause();
                continue;
            }

            // Safe to read value
            out = head_next->value;
            
            // Try to swing Head
            if (head_.compare_exchange_weak(curr_head, 
                                            head_next, 
                                            std::memory_order_release, 
                                            std::memory_order_relaxed))
            {            
                Reclaimer::protect_at(0, nullptr);
                Reclaimer::protect_at(1, nullptr); 
                Reclaimer::retire(curr_head);
                return true;
            }
            
            bk.pause();
        }
    }

    void close() 
    { 
        closed_.store(true, std::memory_order_release); 
    }

    bool is_closed() const noexcept 
    { 
        return closed_.load(std::memory_order_acquire); 
    }

    static void quiescent() noexcept 
    { 
        Reclaimer::quiescent(); 
    }

private:
    alignas(64) std::atomic<Node*> head_{nullptr};
    alignas(64) std::atomic<Node*> tail_{nullptr};
    alignas(64) std::atomic<bool>  closed_{false};
};

} // namespace mpmcq