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
#include <type_traits>

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
	using value_type = Value;
	using reference = value_type &;
	using const_reference = const value_type &;

#if 0
	static_assert(std::is_nothrow_default_constructible<Value>::value,
		      "bounded_queue requires values with nothrow default constructor");
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
	void push(value_type &&value, Backoff... backoff)
	{
		const std::uint64_t tail = tail_.fetch_add(1, std::memory_order_relaxed);
		ring_slot &slot = ring_[tail & mask_];
		wait_tail(slot, tail, std::forward<Backoff>(backoff)...);
		put_value(slot, tail, std::move(value));
	}

	template <typename... Backoff>
	void push(const value_type &value, Backoff... backoff)
	{
		const std::uint64_t tail = tail_.fetch_add(1, std::memory_order_relaxed);
		ring_slot &slot = ring_[tail & mask_];
		wait_tail(slot, tail, std::forward<Backoff>(backoff)...);
		put_value(slot, tail, value);
	}

	template <typename... Backoff>
	queue_op_status wait_pop(value_type &value, Backoff... backoff)
	{
		const std::uint64_t head = head_.fetch_add(1, std::memory_order_relaxed);
		ring_slot &slot = ring_[head & mask_];
		for (;;) {
			bq_status status
				= wait_head(slot, head + 1, std::forward<Backoff>(backoff)...);
			if (status == bq_closed)
				return queue_op_status::closed;
			status = get_value(slot, head, status, value);
			if (status == bq_normal)
				return queue_op_status::success;
		}
	}

private:
	struct alignas(cache_line_size) ring_slot : public Ticket
	{
		value_type value;
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

	bq_status wait_head(ring_slot &slot, std::uint64_t head)
	{
		std::uint32_t current_ticket = slot.load();
		std::uint32_t required_ticket = head << bq_status_bits;
		while ((current_ticket & bq_ticket_mask) != required_ticket) {
			if (is_closed()) {
				std::uint64_t tail = tail_.load(std::memory_order_seq_cst);
				if (head >= tail)
					return bq_closed;
			}
			current_ticket = slot.wait_and_load(current_ticket);
		}
		return bq_status(current_ticket & bq_status_mask);
	}

	template <typename Backoff>
	bq_status wait_head(ring_slot &slot, std::uint64_t head, Backoff backoff)
	{
		bool waiting = false;
		std::uint32_t current_ticket = slot.load();
		std::uint32_t required_ticket = head << bq_status_bits;
		while ((current_ticket & bq_ticket_mask) != required_ticket) {
			if (is_closed()) {
				std::uint64_t tail = tail_.load(std::memory_order_seq_cst);
				if (head >= tail)
					return bq_closed;
			}
			if (waiting) {
				current_ticket = slot.wait_and_load(current_ticket);
			} else {
				waiting = backoff();
				current_ticket = slot.load();
			}
		}
		return bq_status(current_ticket & bq_status_mask);
	}

	void wake_tail(ring_slot &slot, std::uint32_t tail)
	{
		slot.store_and_wake((tail + 1) << bq_status_bits);
	}

	void wake_tail(ring_slot &slot, std::uint32_t tail, bq_status status)
	{
		slot.store_and_wake(((tail + 1) << bq_status_bits) | status);
	}

	void wake_head(ring_slot &slot, std::uint32_t head)
	{
		slot.store_and_wake((head + mask_ + 1) << bq_status_bits);
	}

	template <typename V = value_type,
		  typename std::enable_if<std::is_nothrow_copy_assignable<V>::value>::type
			  * = nullptr>
	void put_value(ring_slot &slot, std::uint64_t tail, const value_type &value)
	{
		slot.value = value;
		wake_tail(slot, tail);
	}

	template <typename V = value_type,
		  typename std::enable_if<not std::is_nothrow_copy_assignable<V>::value>::type
			  * = nullptr>
	void put_value(ring_slot &slot, std::uint64_t tail, const value_type &value)
	{
		try {
			slot.value = value;
		} catch (...) {
			wake_tail(slot, tail, bq_invalid);
			throw;
		}
		wake_tail(slot, tail);
	}

	template <typename V = value_type,
		  typename std::enable_if<std::is_nothrow_move_assignable<V>::value>::type
			  * = nullptr>
	void put_value(ring_slot &slot, std::uint64_t tail, value_type &&value)
	{
		slot.value = std::move(value);
		wake_tail(slot, tail);
	}

	template <typename V = value_type,
		  typename std::enable_if<not std::is_nothrow_move_assignable<V>::value>::type
			  * = nullptr>
	void put_value(ring_slot &slot, std::uint64_t tail, value_type &&value)
	{
		try {
			slot.value = std::move(value);
		} catch (...) {
			wake_tail(slot, tail, bq_invalid);
			throw;
		}
		wake_tail(slot, tail);
	}

	template <typename V = value_type,
		  typename std::enable_if<std::is_nothrow_move_assignable<V>::value>::type
			  * = nullptr>
	bq_status get_value(ring_slot &slot, std::uint64_t head, bq_status, value_type &value)
	{
		value = std::move(slot.value);
		wake_head(slot, head);
		return bq_normal;
	}

	template <typename V = value_type,
		  typename std::enable_if<not std::is_nothrow_move_assignable<V>::value>::type
			  * = nullptr>
	bq_status
	get_value(ring_slot &slot, std::uint64_t head, bq_status status, value_type &value)
	{
		if (status == bq_invalid)
			return status;

		try {
			value = std::move(slot.value);
		} catch (...) {
			wake_head(slot, head);
			throw;
		}
		wake_head(slot, head);
		return bq_normal;
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
