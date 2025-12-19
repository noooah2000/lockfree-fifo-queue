#pragma once
#include <queue>
#include <mutex>
#include <atomic>

namespace mpmcq 
{

/**
 * @brief A thread-safe unbounded queue implemented with std::mutex.
 * * This class serves as a baseline for performance comparisons against 
 * lock-free implementations. It provides thread safety using a single 
 * coarse-grained lock.
 */
template <class T>
class MutexQueue 
{
public:
    // The capacity hint is ignored as the underlying std::queue grows dynamically.
    explicit MutexQueue(std::size_t /*capacity_hint*/ = 0) : closed_(false) {}

    bool enqueue(const T& v) 
    {
        // Early exit if closed to avoid unnecessary locking
        if (closed_) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        
        // Double-check status after acquiring the lock
        if (closed_) return false;
        
        queue_.push(v);
        return true;
    }

    bool try_dequeue(T& out) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;

        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void close() 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

    bool is_closed() const noexcept 
    { 
        return closed_.load(std::memory_order_relaxed); 
    }

    // No-op: Required to satisfy the interface expected by benchmarks 
    // that support Safe Memory Reclamation (SMR) mechanisms.
    static void quiescent() noexcept {} 

private:
    mutable std::mutex mutex_;
    std::queue<T> queue_;
    std::atomic<bool> closed_;
};

} // namespace mpmcq