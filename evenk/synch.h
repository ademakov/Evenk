//
// Synchronization Primitives
//
// Copyright (c) 2015-2016  Aleksey Demakov
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#ifndef EVENK_SYNCH_H_
#define EVENK_SYNCH_H_

#include <condition_variable>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>

#include <pthread.h>

#include "evenk/backoff.h"
#include "evenk/futex.h"

namespace evenk {

//
// Mutexes
//

class posix_mutex
{
public:
	posix_mutex() noexcept : mutex_(PTHREAD_MUTEX_INITIALIZER)
	{
	}

	posix_mutex(const posix_mutex &) = delete;
	posix_mutex &operator=(const posix_mutex &) = delete;

	~posix_mutex() noexcept
	{
		pthread_mutex_destroy(&mutex_);
	}

	void lock()
	{
		int ret = pthread_mutex_lock(&mutex_);
		if (ret)
			throw std::system_error(
				ret, std::system_category(), "pthread_mutex_lock()");
	}

	void unlock()
	{
		int ret = pthread_mutex_unlock(&mutex_);
		if (ret)
			throw std::system_error(
				ret, std::system_category(), "pthread_mutex_unlock()");
	}

private:
	friend class posix_cond_var;

	pthread_mutex_t mutex_;
};

class futex_lock
{
public:
	futex_lock() noexcept : futex_(0)
	{
	}

	futex_lock(const futex_lock &) = delete;
	futex_lock &operator=(const futex_lock &) = delete;

	void lock()
	{
		lock(NoBackoff{});
	}

	template <typename Backoff>
	void lock(Backoff backoff)
	{
		for (std::uint32_t value = 0; !futex_.compare_exchange_strong(
			     value, 1, std::memory_order_acquire, std::memory_order_relaxed);
		     value = 0) {
			if (backoff()) {
				if (value == 2
				    || futex_.exchange(2, std::memory_order_acquire)) {
					do
						futex_wait(futex_, 2);
					while (futex_.exchange(2, std::memory_order_acquire));
				}
				break;
			}
		}
	}

	void unlock()
	{
		if (futex_.fetch_sub(1, std::memory_order_release) != 1) {
			futex_.store(0, std::memory_order_relaxed);
			futex_wake(futex_, 1);
		}
	}

private:
	friend class futex_cond_var;

	std::atomic<std::uint32_t> futex_;
};

//
// Lock Guard
//

template <typename LockType>
class lock_guard
{
public:
	lock_guard(LockType &a_lock) : lock_ptr_(&a_lock), owns_lock_(false)
	{
		lock();
	}

	template <typename Backoff>
	lock_guard(LockType &a_lock, Backoff backoff) : lock_ptr_(&a_lock), owns_lock_(false)
	{
		lock(backoff);
	}

	lock_guard(LockType &a_lock, std::adopt_lock_t) noexcept
		: lock_ptr_(&a_lock), owns_lock_(true)
	{
	}

	lock_guard(LockType &a_lock, std::defer_lock_t) noexcept
		: lock_ptr_(&a_lock), owns_lock_(false)
	{
	}

	lock_guard(const lock_guard &) = delete;
	lock_guard &operator=(const lock_guard &) = delete;

	~lock_guard()
	{
		if (owns_lock_)
			lock_ptr_->unlock();
	}

	void lock()
	{
		lock_ptr_->lock();
		owns_lock_ = true;
	}

	template <typename Backoff>
	void lock(Backoff backoff)
	{
		lock_ptr_->lock(backoff);
		owns_lock_ = true;
	}

	void unlock()
	{
		lock_ptr_->unlock();
		owns_lock_ = false;
	}

	LockType *get()
	{
		return lock_ptr_;
	}

	bool owns_lock()
	{
		return owns_lock_;
	}

private:
	LockType *lock_ptr_;
	bool owns_lock_;
};

//
// Condition Variables
//

class posix_cond_var
{
public:
	posix_cond_var() noexcept : condition_(PTHREAD_COND_INITIALIZER)
	{
	}

	posix_cond_var(const posix_cond_var &) = delete;
	posix_cond_var &operator=(const posix_cond_var &) = delete;

	~posix_cond_var() noexcept
	{
		pthread_cond_destroy(&condition_);
	}

	void wait(std::unique_lock<posix_mutex> &ulock)
	{
		int ret = pthread_cond_wait(&condition_, &ulock.mutex()->mutex_);
		if (ret)
			throw std::system_error(
				ret, std::system_category(), "pthread_cond_wait()");
	}

	void notify_one()
	{
		int ret = pthread_cond_signal(&condition_);
		if (ret)
			throw std::system_error(
				ret, std::system_category(), "pthread_cond_signal()");
	}

	void notify_all()
	{
		int ret = pthread_cond_broadcast(&condition_);
		if (ret)
			throw std::system_error(
				ret, std::system_category(), "pthread_cond_broadcast()");
	}

private:
	pthread_cond_t condition_;
};

class futex_cond_var
{
public:
	futex_cond_var() noexcept : futex_(0), count_(0), owner_(nullptr)
	{
	}

	futex_cond_var(const futex_cond_var &) = delete;
	futex_cond_var &operator=(const futex_cond_var &) = delete;

	void wait(lock_guard<futex_lock> &guard)
	{
		futex_lock *owner = guard.get();
		if (owner_ != nullptr && owner_ != owner)
			throw std::invalid_argument(
				"different locks used for the same condition variable.");
		owner_.store(owner, std::memory_order_relaxed);

		count_.fetch_add(1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acq_rel);
		std::uint32_t value = futex_.load(std::memory_order_relaxed);

		owner->unlock();

		futex_wait(futex_, value);

		count_.fetch_sub(1, std::memory_order_relaxed);
		while (owner->futex_.exchange(2, std::memory_order_acquire))
			futex_wait(owner->futex_, 2);
	}

	void notify_one()
	{
		futex_.fetch_add(1, std::memory_order_acquire);
		if (count_.load(std::memory_order_relaxed))
			futex_wake(futex_, 1);
	}

	void notify_all()
	{
		futex_.fetch_add(1, std::memory_order_acquire);
		if (count_.load(std::memory_order_relaxed)) {
			futex_lock *owner = owner_.load(std::memory_order_relaxed);
			if (owner)
				futex_requeue(futex_,
					      1,
					      std::numeric_limits<int>::max(),
					      owner->futex_);
		}
	}

private:
	std::atomic<std::uint32_t> futex_;
	std::atomic<std::uint32_t> count_;
	std::atomic<futex_lock *> owner_;
};

//
// Synchronization Traits
//

class std_synch
{
public:
	using lock_type = std::mutex;
	using cond_var_type = std::condition_variable;
	using lock_owner_type = std::unique_lock<std::mutex>;
};

class posix_synch
{
public:
	using lock_type = posix_mutex;
	using cond_var_type = posix_cond_var;
	using lock_owner_type = std::unique_lock<posix_mutex>;
};

class futex_synch
{
public:
	using lock_type = futex_lock;
	using cond_var_type = futex_cond_var;
	using lock_owner_type = lock_guard<futex_lock>;
};

#if __linux__
using default_synch = futex_synch;
#else
using default_synch = std_synch;
#endif

} // namespace evenk

#endif // !EVENK_SYNCH_H_
