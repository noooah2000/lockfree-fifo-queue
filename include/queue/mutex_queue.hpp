#pragma once
#include <queue>
#include <mutex>
#include <atomic>

namespace mpmcq 
{

template <class T>
class MutexQueue 
{
public:
	explicit MutexQueue(std::size_t /*capacity_hint*/ = 0) : closed_(false) {}

	bool enqueue(const T& v) 
	{
		if (closed_) return false;
		std::lock_guard<std::mutex> lock(mutex_);
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

	bool is_closed() const noexcept { return closed_.load(std::memory_order_relaxed); }
	static void quiescent() noexcept {} // 與 lock-free API 對齊

private:
	mutable std::mutex mutex_;
	std::queue<T> queue_;
	std::atomic<bool> closed_;
};

} // namespace mpmcq 
