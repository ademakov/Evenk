//
// Synchronization Primitives
//
// Copyright (c) 2015  Aleksey Demakov
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

class StdMutex : public std::mutex
{
public:
	void Lock()
	{
		lock();
	}
	void Unlock()
	{
		unlock();
	}
};

class PosixMutex
{
public:
	PosixMutex() noexcept : mutex_(PTHREAD_MUTEX_INITIALIZER)
	{
	}

	PosixMutex(const PosixMutex &) = delete;
	PosixMutex &operator=(const PosixMutex &) = delete;

	~PosixMutex() noexcept
	{
		pthread_mutex_destroy(&mutex_);
	}

	void Lock()
	{
		int ret = pthread_mutex_lock(&mutex_);
		if (ret)
			throw std::system_error(
				ret, std::system_category(), "pthread_mutex_lock()");
	}

	void Unlock()
	{
		int ret = pthread_mutex_unlock(&mutex_);
		if (ret)
			throw std::system_error(
				ret, std::system_category(), "pthread_mutex_unlock()");
	}

private:
	friend class PosixCondVar;

	pthread_mutex_t mutex_;
};

class FutexLock
{
public:
	FutexLock() noexcept : futex_(0)
	{
	}

	FutexLock(const FutexLock &) = delete;
	FutexLock &operator=(const FutexLock &) = delete;

	void Lock()
	{
		Lock(NoBackoff{});
	}

	template <typename Backoff>
	void Lock(Backoff backoff)
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

	void Unlock()
	{
		if (futex_.fetch_sub(1, std::memory_order_release) != 1) {
			futex_.store(0, std::memory_order_relaxed);
			futex_wake(futex_, 1);
		}
	}

private:
	friend class FutexCondVar;

	std::atomic<std::uint32_t> futex_;
};

//
// Lock Guard
//

template <typename LockType>
class LockGuard
{
public:
	LockGuard(LockType &lock) : lock_ptr_(&lock), owns_lock_(false)
	{
		Lock();
	}

	template <typename Backoff>
	LockGuard(LockType &lock, Backoff backoff) : lock_ptr_(&lock), owns_lock_(false)
	{
		Lock(backoff);
	}

	LockGuard(LockType &lock, std::adopt_lock_t) noexcept
		: lock_ptr_(&lock), owns_lock_(true)
	{
	}

	LockGuard(LockType &lock, std::defer_lock_t) noexcept
		: lock_ptr_(&lock), owns_lock_(false)
	{
	}

	LockGuard(const LockGuard &) = delete;
	LockGuard &operator=(const LockGuard &) = delete;

	~LockGuard()
	{
		if (owns_lock_)
			lock_ptr_->Unlock();
	}

	void Lock()
	{
		lock_ptr_->Lock();
		owns_lock_ = true;
	}

	template <typename Backoff>
	void Lock(Backoff backoff)
	{
		lock_ptr_->Lock(backoff);
		owns_lock_ = true;
	}

	void Unlock()
	{
		lock_ptr_->Unlock();
		owns_lock_ = false;
	}

	LockType *GetLockPtr()
	{
		return lock_ptr_;
	}

	bool OwnsLock()
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

class StdCondVar : public std::condition_variable
{
public:
	void Wait(LockGuard<StdMutex> &guard)
	{
		std::unique_lock<std::mutex> lock(*guard.GetLockPtr(), std::adopt_lock);
		wait(lock);
		lock.release();
	}

	void NotifyOne()
	{
		notify_one();
	}
	void NotifyAll()
	{
		notify_all();
	}
};

class PosixCondVar
{
public:
	PosixCondVar() noexcept : condition_(PTHREAD_COND_INITIALIZER)
	{
	}

	PosixCondVar(const PosixCondVar &) = delete;
	PosixCondVar &operator=(const PosixCondVar &) = delete;

	~PosixCondVar() noexcept
	{
		pthread_cond_destroy(&condition_);
	}

	void Wait(LockGuard<PosixMutex> &guard)
	{
		int ret = pthread_cond_wait(&condition_, &guard.GetLockPtr()->mutex_);
		if (ret)
			throw std::system_error(
				ret, std::system_category(), "pthread_cond_wait()");
	}

	void NotifyOne()
	{
		int ret = pthread_cond_signal(&condition_);
		if (ret)
			throw std::system_error(
				ret, std::system_category(), "pthread_cond_signal()");
	}

	void NotifyAll()
	{
		int ret = pthread_cond_broadcast(&condition_);
		if (ret)
			throw std::system_error(
				ret, std::system_category(), "pthread_cond_broadcast()");
	}

private:
	pthread_cond_t condition_;
};

class FutexCondVar
{
public:
	FutexCondVar() noexcept : futex_(0), count_(0), owner_(nullptr)
	{
	}

	FutexCondVar(const FutexCondVar &) = delete;
	FutexCondVar &operator=(const FutexCondVar &) = delete;

	void Wait(LockGuard<FutexLock> &guard)
	{
		FutexLock *owner = guard.GetLockPtr();
		if (owner_ != nullptr && owner_ != owner)
			throw std::invalid_argument(
				"different locks used for the same condition variable.");
		owner_.store(owner, std::memory_order_relaxed);

		count_.fetch_add(1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acq_rel);
		std::uint32_t value = futex_.load(std::memory_order_relaxed);

		owner->Unlock();

		futex_wait(futex_, value);

		count_.fetch_sub(1, std::memory_order_relaxed);
		while (owner->futex_.exchange(2, std::memory_order_acquire))
			futex_wait(owner->futex_, 2);
	}

	void NotifyOne()
	{
		futex_.fetch_add(1, std::memory_order_acquire);
		if (count_.load(std::memory_order_relaxed))
			futex_wake(futex_, 1);
	}

	void NotifyAll()
	{
		futex_.fetch_add(1, std::memory_order_acquire);
		if (count_.load(std::memory_order_relaxed)) {
			FutexLock *owner = owner_.load(std::memory_order_relaxed);
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
	std::atomic<FutexLock *> owner_;
};

//
// Synchronization Traits
//

class StdSynch
{
public:
	using LockType = StdMutex;
	using CondVarType = StdCondVar;
};

class PosixSynch
{
public:
	using LockType = PosixMutex;
	using CondVarType = PosixCondVar;
};

class FutexSynch
{
public:
	using LockType = FutexLock;
	using CondVarType = FutexCondVar;
};

#if __linux__
using DefaultSynch = FutexSynch;
#else
using DefaultSynch = StdSynch;
#endif

} // namespace evenk

#endif // !EVENK_SYNCH_H_
