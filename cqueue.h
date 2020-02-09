
// Created by David Guillen Fandos <david@davidgf.net> 2019

#include <list>
#include <atomic>
#include <mutex>
#include <condition_variable>

template<typename T>
class ConcurrentQueue {
public:
	ConcurrentQueue(unsigned max_size) : max_size(max_size), nowriter(false) {}

	void close() {
		std::unique_lock<std::mutex> lock(mutex_);
		nowriter = true;
		readvar.notify_all();
	}

	void push(T item) {
		std::unique_lock<std::mutex> lock(mutex_);
		while (q.size() >= max_size)
			writevar.wait(lock);

		q.push_back(std::move(item));
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

private:
	unsigned max_size;
	std::list<T> q;     // list of items
	std::mutex mutex_;  // protection mutex
	std::condition_variable readvar, writevar; // Wait variables
	std::atomic<bool> nowriter;      // Indicates no more writes will happen
};


