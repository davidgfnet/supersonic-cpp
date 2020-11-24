
// Created by David Guillen Fandos <david@davidgf.net> 2019

#include <list>
#include <atomic>
#include <mutex>
#include <condition_variable>

template<typename T>
class ConcurrentQueue {
public:
	ConcurrentQueue(unsigned max_size) : max_size_(max_size), nowriter(false) {}

	void close() {
		std::unique_lock<std::mutex> lock(mutex_);
		nowriter = true;
		readvar.notify_all();
	}

	void push(T item) {
		std::unique_lock<std::mutex> lock(mutex_);
		while (q.size() >= max_size_)
			writevar.wait(lock);

		q.push_back(std::move(item));
		queued_++;
		lock.unlock();
		readvar.notify_one();
	}

	bool pop(T *item) noexcept {
		std::unique_lock<std::mutex> lock(mutex_);
		while (q.empty() && !nowriter)
			readvar.wait(lock);

		if (!q.empty()) {
			*item = std::move(q.front());
			q.pop_front();
			writevar.notify_one();
			return true;
		}

		// Writer signaled end already
		return false;
	}

	std::size_t queued() const {
		std::unique_lock<std::mutex> lock(mutex_);
		return queued_;
	}

	std::size_t size() const {
		std::unique_lock<std::mutex> lock(mutex_);
		return q.size();
	}

	std::size_t closed() const {
		std::unique_lock<std::mutex> lock(mutex_);
		return nowriter;
	}

private:
	unsigned max_size_, queued_;
	std::list<T> q;     // list of items
	mutable std::mutex mutex_;  // protection mutex
	std::condition_variable readvar, writevar; // Wait variables
	std::atomic<bool> nowriter;      // Indicates no more writes will happen
};


