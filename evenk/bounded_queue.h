//
// Fast Bounded Concurrent Queue
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

#ifndef EVENK_BOUNDED_QUEUE_H_
#define EVENK_BOUNDED_QUEUE_H_

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#include "backoff.h"
#include "basic.h"
#include "conqueue.h"
#include "futex.h"
#include "synch.h"

namespace evenk {

class bounded_queue_ticket : protected std::atomic<std::uint32_t>
{
public:
	using base = std::atomic<std::uint32_t>;

	void initialize(std::uint32_t value)
	{
		base::store(value, std::memory_order_relaxed);
	}

	std::uint32_t load() const
	{
		return base::load(std::memory_order_acquire);
	}

	void store(std::uint32_t value)
	{
		base::store(value, std::memory_order_release);
	}
};

class bounded_queue_busywait : public bounded_queue_ticket
{
public:
	std::uint32_t wait_and_load(std::uint32_t)
	{
		return load();
	}

	void store_and_wake(std::uint32_t value)
	{
		store(value);
	}

	void wake()
	{
	}
};

class bounded_queue_yield : public bounded_queue_ticket
{
public:
	std::uint32_t wait_and_load(std::uint32_t)
	{
		std::this_thread::yield();
		return load();
	}

	void store_and_wake(std::uint32_t value)
	{
		store(value);
	}

	void wake()
	{
	}
};

class bounded_queue_futex : public bounded_queue_ticket
{
public:
	std::uint32_t wait_and_load(std::uint32_t value)
	{
		wait_count_.fetch_add(1, std::memory_order_relaxed);
		// FIXME: Presuming a futex syscall is a full memory fence
		// on its own. The threads that load the wait_count_ field
		// must see it incremented  as long as there is any chance
		// the current thread might be sleeping on the futex. On the
		// other hand within the futex system call, if the current
		// thread is not sleeping yet, it should be able to observe
		// a possible futex value update from other threads.
		//
		// If for some architecture (ARM? POWER?) this is not true,
		// then an explicit memory fence should be added here.
		futex_wait(*this, value);
		wait_count_.fetch_sub(1, std::memory_order_relaxed);
		return load();
	}

	void store_and_wake(std::uint32_t value)
	{
		store(value);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (wait_count_.load(std::memory_order_relaxed))
			wake();
	}

	void wake()
	{
		futex_wake(*this, INT32_MAX);
	}

private:
	std::atomic<std::uint32_t> wait_count_ = ATOMIC_VAR_INIT(0);
};

template <typename Synch = default_synch>
class bounded_queue_synch : public bounded_queue_ticket
{
public:
	using lock_type = typename Synch::lock_type;
	using cond_var_type = typename Synch::cond_var_type;
	using lock_owner_type = typename Synch::lock_owner_type;

	std::uint32_t wait_and_load(std::uint32_t value)
	{
		lock_owner_type guard(lock_);
		std::uint32_t current_value = base::load(std::memory_order_relaxed);
		if (current_value == value) {
			cond_.wait(guard);
			current_value = base::load(std::memory_order_relaxed);
		}
		return current_value;
	}

	void store_and_wake(std::uint32_t value)
	{
		lock_owner_type guard(lock_);
		base::store(value, std::memory_order_relaxed);
		cond_.notify_all();
	}

	void wake()
	{
		lock_owner_type guard(lock_);
		cond_.notify_all();
	}

private:
	lock_type lock_;
	cond_var_type cond_;
};

template <typename Value, typename Ticket = bounded_queue_busywait>
class bounded_queue : non_copyable
{
public:
	bounded_queue(std::uint32_t size)
		: ring_{nullptr}, mask_{size - 1}, closed_{false}, head_{0}, tail_{0}
	{
		if (size == 0 || (size & mask_) != 0)
			throw std::invalid_argument("BoundedQueue size must be a power of two");

		void *ring;
		if (::posix_memalign(&ring, cache_line_size, size * sizeof(ring_slot)))
			throw std::bad_alloc();

		ring_ = new (ring) ring_slot[size];
		for (std::uint32_t i = 0; i < size; i++)
			ring_[i].initialize(i);
	}

	bounded_queue(bounded_queue &&other) noexcept
		: ring_{other.ring_}, mask_{other.mask_}, closed_{false}, head_{0}, tail_{0}
	{
		other.ring_ = nullptr;
	}

	~bounded_queue()
	{
		destroy();
	}

	void close()
	{
		closed_.store(true, std::memory_order_relaxed);
		for (std::uint32_t i = 0; i < mask_ + 1; i++)
			ring_[i].wake();
	}

	bool is_closed() const
	{
		return closed_.load(std::memory_order_relaxed);
	}

	bool is_empty() const
	{
		int64_t head = head_.load(std::memory_order_relaxed);
		int64_t tail = tail_.load(std::memory_order_relaxed);
		return (tail <= head);
	}

	template <typename... Backoff>
	void push(Value &&value, Backoff... backoff)
	{
		const std::uint64_t tail = tail_.fetch_add(1, std::memory_order_seq_cst);
		ring_slot &slot = ring_[tail & mask_];
		wait_tail(slot, tail, std::forward<Backoff>(backoff)...);
		slot.value = std::move(value);
		wake_head(slot, tail + 1);
	}

	template <typename... Backoff>
	void push(const Value &value, Backoff... backoff)
	{
		const std::uint64_t tail = tail_.fetch_add(1, std::memory_order_seq_cst);
		ring_slot &slot = ring_[tail & mask_];
		wait_tail(slot, tail, std::forward<Backoff>(backoff)...);
		slot.value = value;
		wake_head(slot, tail + 1);
	}

	template <typename... Backoff>
	queue_op_status wait_pop(Value &value, Backoff... backoff)
	{
		const std::uint64_t head = head_.fetch_add(1, std::memory_order_relaxed);
		ring_slot &slot = ring_[head & mask_];
		if (!wait_head(slot, head + 1, std::forward<Backoff>(backoff)...))
			return queue_op_status::closed;
		value = std::move(slot.value);
		wake_tail(slot, head + mask_ + 1);
		return queue_op_status::success;
	}

private:
	struct alignas(cache_line_size) ring_slot : public Ticket
	{
		Value value;
	};

	void destroy()
	{
		if (ring_ != nullptr) {
			std::uint32_t size = mask_ + 1;
			for (std::uint32_t i = 0; i < size; i++)
				ring_[i].~ring_slot();
			std::free(ring_);
		}
	}

	void wait_tail(ring_slot &slot, std::uint64_t required_ticket)
	{
		std::uint32_t current_ticket = slot.load();
		while (current_ticket != std::uint32_t(required_ticket)) {
			current_ticket = slot.wait_and_load(current_ticket);
		}
	}

	template <typename Backoff>
	void wait_tail(ring_slot &slot, std::uint64_t required_ticket, Backoff backoff)
	{
		bool waiting = false;
		std::uint32_t current_ticket = slot.load();
		while (current_ticket != std::uint32_t(required_ticket)) {
			if (waiting) {
				current_ticket = slot.wait_and_load(current_ticket);
			} else {
				waiting = backoff();
				current_ticket = slot.load();
			}
		}
	}

	bool wait_head(ring_slot &slot, std::uint64_t required_ticket)
	{
		std::uint32_t current_ticket = slot.load();
		while (current_ticket != std::uint32_t(required_ticket)) {
			if (is_closed()) {
				std::uint64_t tail = tail_.load(std::memory_order_seq_cst);
				if (required_ticket >= tail)
					return false;
			}
			current_ticket = slot.wait_and_load(current_ticket);
		}
		return true;
	}

	template <typename Backoff>
	bool wait_head(ring_slot &slot, std::uint64_t required_ticket, Backoff backoff)
	{
		bool waiting = false;
		std::uint32_t current_ticket = slot.load();
		while (current_ticket != std::uint32_t(required_ticket)) {
			if (is_closed()) {
				std::uint64_t tail = tail_.load(std::memory_order_seq_cst);
				if (required_ticket >= tail)
					return false;
			}
			if (waiting) {
				current_ticket = slot.wait_and_load(current_ticket);
			} else {
				waiting = backoff();
				current_ticket = slot.load();
			}
		}
		return true;
	}

	void wake_head(ring_slot &slot, std::uint32_t new_ticket)
	{
		slot.store_and_wake(new_ticket);
	}

	void wake_tail(ring_slot &slot, std::uint32_t new_ticket)
	{
		slot.store_and_wake(new_ticket);
	}

	ring_slot *ring_;
	const std::uint32_t mask_;

	std::atomic<bool> closed_;

	alignas(cache_line_size) std::atomic<std::uint64_t> head_;
	alignas(cache_line_size) std::atomic<std::uint64_t> tail_;
};

template <typename ValueType>
using default_bounded_queue = bounded_queue<ValueType, bounded_queue_busywait>;

} // namespace evenk

#endif // !EVENK_BOUNDED_QUEUE_H_
