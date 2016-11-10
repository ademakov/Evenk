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

inline namespace detail {

enum bq_status : std::uint32_t {
	bq_normal = 0,
	bq_waiting = 1,
	bq_invalid = 2,
	bq_closed = 3,
};

constexpr std::uint32_t bq_status_bits = 2;
constexpr std::uint32_t bq_ticket_step = 1 << bq_status_bits;
constexpr std::uint32_t bq_status_mask = bq_ticket_step - 1;
constexpr std::uint32_t bq_ticket_mask = ~bq_status_mask;

} // namespace detail

class bq_slot : protected std::atomic<std::uint32_t>
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

	std::uint32_t wait_and_load(std::uint32_t)
	{
		return load();
	}

	void store_and_wake(std::uint32_t value)
	{
		base::store(value, std::memory_order_release);
	}

	void wake()
	{
	}
};

class bq_yield_slot : public bq_slot
{
public:
	std::uint32_t wait_and_load(std::uint32_t)
	{
		std::this_thread::yield();
		return load();
	}
};

class bq_futex_slot : public bq_slot
{
public:
	std::uint32_t wait_and_load(std::uint32_t value)
	{
		std::uint32_t old_value = value;
		std::uint32_t new_value = value | bq_waiting;
		if (compare_exchange_strong(old_value,
					    new_value,
					    std::memory_order_relaxed,
					    std::memory_order_relaxed)
		    || old_value == new_value)
			futex_wait(*this, value);
		return load();
	}

	void store_and_wake(std::uint32_t value)
	{
		value = exchange(value, std::memory_order_release);
		if ((value & bq_status_mask) == bq_waiting)
			wake();
	}

	void wake()
	{
		futex_wake(*this, INT32_MAX);
	}
};

template <typename Synch = default_synch>
class bq_synch_slot : public bq_slot
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

template <typename Value, typename Ticket = bq_slot>
class bounded_queue : non_copyable
{
public:
#if 0
	static_assert(std::is_nothrow_default_constructible<Value>::value,
		      "bounded_queue requires values with nothrow default constructor");
	static_assert(std::is_nothrow_copy_assignable<Value>::value,
			"bounded_queue requires values with nothrow copy assignment");
	static_assert(std::is_nothrow_move_assignable<Value>::value,
		      "bounded_queue requires values with nothrow move assignment");
#endif

	bounded_queue(std::uint32_t size)
		: ring_{nullptr}, mask_{size - 1}, closed_{false}, head_{0}, tail_{0}
	{
		if (size == 0 || (size & mask_) != 0)
			throw std::invalid_argument(
				"bounded_queue size must be a power of two");

		void *ring;
		if (::posix_memalign(&ring, cache_line_size, size * sizeof(ring_slot)))
			throw std::bad_alloc();

		ring_ = new (ring) ring_slot[size];
		for (std::uint32_t i = 0; i < size; i++)
			ring_[i].initialize(i << bq_status_bits);
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

	void wait_tail(ring_slot &slot, std::uint64_t tail)
	{
		std::uint32_t current_ticket = slot.load();
		std::uint32_t required_ticket = tail << bq_status_bits;
		while ((current_ticket & bq_ticket_mask) != required_ticket) {
			current_ticket = slot.wait_and_load(current_ticket);
		}
	}

	template <typename Backoff>
	void wait_tail(ring_slot &slot, std::uint64_t tail, Backoff backoff)
	{
		bool waiting = false;
		std::uint32_t current_ticket = slot.load();
		std::uint32_t required_ticket = tail << bq_status_bits;
		while ((current_ticket & bq_ticket_mask) != required_ticket) {
			if (waiting) {
				current_ticket = slot.wait_and_load(current_ticket);
			} else {
				waiting = backoff();
				current_ticket = slot.load();
			}
		}
	}

	bool wait_head(ring_slot &slot, std::uint64_t head)
	{
		std::uint32_t current_ticket = slot.load();
		std::uint32_t required_ticket = head << bq_status_bits;
		while ((current_ticket & bq_ticket_mask) != required_ticket) {
			if (is_closed()) {
				std::uint64_t tail = tail_.load(std::memory_order_seq_cst);
				if (head >= tail)
					return false;
			}
			current_ticket = slot.wait_and_load(current_ticket);
		}
		return true;
	}

	template <typename Backoff>
	bool wait_head(ring_slot &slot, std::uint64_t head, Backoff backoff)
	{
		bool waiting = false;
		std::uint32_t current_ticket = slot.load();
		std::uint32_t required_ticket = head << bq_status_bits;
		while ((current_ticket & bq_ticket_mask) != required_ticket) {
			if (is_closed()) {
				std::uint64_t tail = tail_.load(std::memory_order_seq_cst);
				if (head >= tail)
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

	void wake_head(ring_slot &slot, std::uint32_t next_head)
	{
		slot.store_and_wake(next_head << bq_status_bits);
	}

	void wake_tail(ring_slot &slot, std::uint32_t next_tail)
	{
		slot.store_and_wake(next_tail << bq_status_bits);
	}

	ring_slot *ring_;
	const std::uint32_t mask_;

	std::atomic<bool> closed_;

	alignas(cache_line_size) std::atomic<std::uint64_t> head_;
	alignas(cache_line_size) std::atomic<std::uint64_t> tail_;
};

template <typename ValueType>
using default_bounded_queue = bounded_queue<ValueType, bq_slot>;

} // namespace evenk

#endif // !EVENK_BOUNDED_QUEUE_H_
