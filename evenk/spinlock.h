//
// Spin Locks
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

#ifndef EVENK_SPINLOCK_H_
#define EVENK_SPINLOCK_H_

#include <atomic>
#include <cstdint>

#include "backoff.h"
#include "basic.h"

namespace evenk {

class spin_lock : non_copyable
{
public:
	void lock() noexcept
	{
		lock(no_backoff{});
	}

	template <typename Backoff>
	void lock(Backoff backoff) noexcept
	{
		while (lock_.test_and_set(std::memory_order_acquire))
			backoff();
	}

	bool try_lock() noexcept
	{
		return !lock_.test_and_set(std::memory_order_acquire);
	}

	void unlock() noexcept
	{
		lock_.clear(std::memory_order_release);
	}

private:
	std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};

class tatas_lock : non_copyable
{
public:
	void lock() noexcept
	{
		lock(no_backoff{});
	}

	template <typename Backoff>
	void lock(Backoff backoff) noexcept
	{
		while (lock_.exchange(true, std::memory_order_acquire)) {
			do
				backoff();
			while (lock_.load(std::memory_order_relaxed));
		}
	}

	bool try_lock() noexcept
	{
		return !lock_.exchange(true, std::memory_order_acquire);
	}

	void unlock() noexcept
	{
		lock_.store(false, std::memory_order_release);
	}

private:
	std::atomic<bool> lock_ = ATOMIC_VAR_INIT(false);
};

class ticket_lock : non_copyable
{
public:
	void lock() noexcept
	{
		lock(no_backoff{});
	}

	template <typename Backoff>
	void lock(Backoff backoff) noexcept
	{
		base_type tail = tail_.fetch_add(1, std::memory_order_relaxed);
		for (;;) {
			base_type head = head_.load(std::memory_order_acquire);
			if (tail == head)
				break;
			proportional_adapter(backoff, static_cast<base_type>(tail - head));
		}
	}

	bool try_lock() noexcept
	{
		base_type head = head_.load(std::memory_order_acquire);
		base_type tail = tail_.load(std::memory_order_relaxed);
		return head == tail
		       && tail_.compare_exchange_strong(
				  tail, tail + 1, std::memory_order_relaxed);
	}

	void unlock() noexcept
	{
		head_.fetch_add(1, std::memory_order_release);
	}

private:
	using base_type = std::uint16_t;

	std::atomic<base_type> head_ = ATOMIC_VAR_INIT(0);
	std::atomic<base_type> tail_ = ATOMIC_VAR_INIT(0);
};

} // namespace evenk

#endif // !EVENK_SPINLOCK_H_
