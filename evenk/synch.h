//
// Synchronization Primitives
//
// Copyright (c) 2015-2017  Aleksey Demakov
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
#include <thread>

#include <pthread.h>

#include "backoff.h"
#include "basic.h"
#include "futex.h"

namespace evenk {

//
// Mutexes
//

class posix_mutex : non_copyable
{
public:
	using native_handle_type = pthread_mutex_t *;

	constexpr posix_mutex() noexcept = default;

	~posix_mutex() noexcept
	{
		pthread_mutex_destroy(&mutex_);
	}

	void lock()
	{
		int rc = pthread_mutex_lock(&mutex_);
		if (rc)
			throw_system_error(rc, "pthread_mutex_lock()");
	}

	bool try_lock() noexcept
	{
		return pthread_mutex_trylock(&mutex_) == 0;
	}

	void unlock() noexcept
	{
		pthread_mutex_unlock(&mutex_);
	}

	native_handle_type native_handle() noexcept
	{
		return &mutex_;
	}

private:
	pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
};

class futex_lock : non_copyable
{
public:
	using native_handle_type = futex_t &;

	constexpr futex_lock() noexcept = default;

	void lock() noexcept
	{
		lock(no_backoff{});
	}

	template <typename Backoff>
	void lock(Backoff backoff) noexcept
	{
		std::uint32_t value = 0;
		while (!futex_.compare_exchange_strong(
			value, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
			if (backoff()) {
				if (value == 2
				    || futex_.exchange(2, std::memory_order_acquire)) {
					do
						futex_wait(futex_, 2);
					while (futex_.exchange(2, std::memory_order_acquire));
				}
				break;
			}
			value = 0;
		}
	}

	bool try_lock() noexcept
	{
		std::uint32_t value = 0;
		return futex_.compare_exchange_strong(
			value, 1, std::memory_order_acquire, std::memory_order_relaxed);
	}

	void unlock() noexcept
	{
		if (futex_.fetch_sub(1, std::memory_order_release) != 1) {
			futex_.store(0, std::memory_order_relaxed);
			futex_wake(futex_, 1);
		}
	}

	native_handle_type native_handle() noexcept
	{
		return futex_;
	}

private:
	futex_t futex_ = ATOMIC_VAR_INIT(0);
};

//
// Lock Guard
//

template <typename Lock>
class lock_guard : non_copyable
{
public:
	using mutex_type = Lock;

	lock_guard(mutex_type &mutex) : mutex_(&mutex), owns_lock_(false)
	{
		lock();
	}

	template <typename Backoff>
	lock_guard(mutex_type &mutex, Backoff backoff) : mutex_(&mutex), owns_lock_(false)
	{
		lock(backoff);
	}

	lock_guard(mutex_type &mutex, std::adopt_lock_t) noexcept
		: mutex_(&mutex), owns_lock_(true)
	{
	}

	lock_guard(mutex_type &mutex, std::defer_lock_t) noexcept
		: mutex_(&mutex), owns_lock_(false)
	{
	}

	lock_guard(mutex_type &mutex, std::try_to_lock_t) noexcept
		: mutex_(&mutex), owns_lock_(false)
	{
		try_lock();
	}

	~lock_guard() noexcept
	{
		if (owns_lock_)
			mutex_->unlock();
	}

	void lock()
	{
		if (owns_lock_)
			throw_system_error(int(std::errc::resource_deadlock_would_occur));
		mutex_->lock();
		owns_lock_ = true;
	}

	template <typename Backoff>
	void lock(Backoff backoff)
	{
		if (owns_lock_)
			throw_system_error(int(std::errc::resource_deadlock_would_occur));
		mutex_->lock(backoff);
		owns_lock_ = true;
	}

	bool try_lock()
	{
		if (owns_lock_)
			throw_system_error(int(std::errc::resource_deadlock_would_occur));
		owns_lock_ = mutex_->try_lock();
		return owns_lock_;
	}

	void unlock()
	{
		if (!owns_lock_)
			throw_system_error(int(std::errc::operation_not_permitted));
		mutex_->unlock();
		owns_lock_ = false;
	}

	mutex_type *mutex() noexcept
	{
		return mutex_;
	}

	bool owns_lock() const noexcept
	{
		return owns_lock_;
	}

private:
	mutex_type *mutex_;
	bool owns_lock_;
};

//
// Condition Variables
//

class posix_cond_var : non_copyable
{
public:
	using native_handle_type = pthread_cond_t *;

	constexpr posix_cond_var() noexcept = default;

	~posix_cond_var() noexcept
	{
		pthread_cond_destroy(&cond_);
	}

	void wait(std::unique_lock<posix_mutex> &lock) noexcept
	{
		int rc = pthread_cond_wait(&cond_, lock.mutex()->native_handle());
		if (rc)
			throw_system_error(rc, "pthread_cond_wait()");
	}

	void notify_one() noexcept
	{
		pthread_cond_signal(&cond_);
	}

	void notify_all() noexcept
	{
		pthread_cond_broadcast(&cond_);
	}

	native_handle_type native_handle() noexcept
	{
		return &cond_;
	}

private:
	pthread_cond_t cond_ = PTHREAD_COND_INITIALIZER;
};

class futex_cond_var : non_copyable
{
public:
	constexpr futex_cond_var() noexcept = default;

	void wait(lock_guard<futex_lock> &guard) noexcept
	{
		futex_lock *owner = guard.mutex();
		if (owner_ != nullptr && owner_ != owner)
#if 0
			throw std::invalid_argument(
				"different locks used for the same condition variable.");
#else
			std::terminate();
#endif
			owner_.store(owner, std::memory_order_relaxed);

		count_.fetch_add(1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acq_rel);
		std::uint32_t value = futex_.load(std::memory_order_relaxed);

		owner->unlock();

		futex_wait(futex_, value);

		futex_t &owner_futex = owner->native_handle();
		count_.fetch_sub(1, std::memory_order_relaxed);
		while (owner_futex.exchange(2, std::memory_order_acquire))
			futex_wait(owner_futex, 2);
	}

	void notify_one() noexcept
	{
		futex_.fetch_add(1, std::memory_order_acquire);
		if (count_.load(std::memory_order_relaxed))
			futex_wake(futex_, 1);
	}

	void notify_all() noexcept
	{
		futex_.fetch_add(1, std::memory_order_acquire);
		if (count_.load(std::memory_order_relaxed)) {
			futex_lock *owner = owner_.load(std::memory_order_relaxed);
			if (owner) {
				futex_requeue(futex_,
					      1,
					      std::numeric_limits<int>::max(),
					      owner->native_handle());
			}
		}
	}

private:
	futex_t futex_ = ATOMIC_VAR_INIT(0);
	futex_t count_ = ATOMIC_VAR_INIT(0);
	std::atomic<futex_lock *> owner_ = ATOMIC_VAR_INIT(nullptr);
};

//
// Synchronization Traits
//

struct std_synch
{
	using lock_type = std::mutex;
	using cond_var_type = std::condition_variable;
	using lock_owner_type = std::unique_lock<std::mutex>;
};

struct posix_synch
{
	using lock_type = posix_mutex;
	using cond_var_type = posix_cond_var;
	using lock_owner_type = std::unique_lock<posix_mutex>;
};

struct futex_synch
{
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
