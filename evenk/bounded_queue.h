//
// Fast Bounded Concurrent Queue
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

// Status flags of a bounded queue slot.
enum bounded_queue_status : std::uint32_t {
	// the slot is empty
	bounded_queue_empty = 0,
	// the slot contains a value
	bounded_queue_valid = 1,
	// the slot is closed
	bounded_queue_closed = 2,
	// the slot contains invalid value
	bounded_queue_failed = 4,
	// there is a waiting thread on the slot (if futex-based)
	bounded_queue_waiting = 8,
};

// The bit mask that selects value status.
constexpr std::uint32_t bounded_queue_status_mask
	= bounded_queue_valid | bounded_queue_closed | bounded_queue_failed;

// The bit mask that selects ticket number.
constexpr std::uint32_t bounded_queue_ticket_mask
	= ~(bounded_queue_status_mask | bounded_queue_waiting);

// The minimum queue size that allows combined slot status and ticket encoding.
constexpr std::uint32_t bounded_queue_min_size = 16;

} // namespace detail

class bq_slot : protected std::atomic<std::uint32_t>
{
public:
	using base = std::atomic<std::uint32_t>;

	void open(std::uint32_t index)
	{
		store(index, std::memory_order_relaxed);
	}

	void close(std::uint32_t index)
	{
		std::uint32_t value = base::load(std::memory_order_relaxed);
		for (;;) {
			std::uint32_t round_value = value - index;
			round_value |= bounded_queue_closed;

			if (compare_exchange_weak(value,
						  round_value + index,
						  std::memory_order_relaxed,
						  std::memory_order_relaxed)) {
				break;
			}
		}
	}

	std::uint32_t load() const
	{
		return base::load(std::memory_order_acquire);
	}

	std::uint32_t wait_and_load(std::uint32_t /*index*/, std::uint32_t /*value*/)
	{
		return base::load(std::memory_order_relaxed);
	}

	void store_and_wake(std::uint32_t /*index*/, std::uint32_t value)
	{
		store(value, std::memory_order_release);
	}
};

class bq_yield_slot : public bq_slot
{
public:
	std::uint32_t wait_and_load(std::uint32_t /*index*/, std::uint32_t /*value*/)
	{
		std::this_thread::yield();
		return base::load(std::memory_order_relaxed);
	}
};

class bq_futex_slot : public bq_slot
{
public:
	void close(std::uint32_t index)
	{
		std::uint32_t value = base::load(std::memory_order_relaxed);
		for (;;) {
			std::uint32_t round_value = value - index;
			round_value |= bounded_queue_closed;

			if (compare_exchange_weak(value,
						  round_value + index,
						  std::memory_order_relaxed,
						  std::memory_order_relaxed)) {
				if ((round_value & bounded_queue_waiting) != 0)
					futex_wake(*this, INT32_MAX);
				break;
			}
		}
	}

	std::uint32_t wait_and_load(std::uint32_t index, std::uint32_t value)
	{
		std::uint32_t wait_value = value - index;
		wait_value |= bounded_queue_waiting;
		wait_value += index;

		if (compare_exchange_strong(value,
					    wait_value,
					    std::memory_order_relaxed,
					    std::memory_order_relaxed)
		    || value == wait_value) {
			futex_wait(*this, wait_value);
			value = base::load(std::memory_order_relaxed);
		}
		return value;
	}

	void store_and_wake(std::uint32_t index, std::uint32_t value)
	{
		value = exchange(value, std::memory_order_release);
		if (((value - index) & bounded_queue_waiting) != 0)
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

	void close(std::uint32_t index)
	{
		lock_owner_type guard(lock_);
		std::uint32_t value = base::load(std::memory_order_relaxed);
		if (((value - index) & bounded_queue_closed) == 0)
			fetch_add(bounded_queue_closed, std::memory_order_relaxed);
		cond_.notify_all();
	}

	std::uint32_t wait_and_load(std::uint32_t /*index*/, std::uint32_t value)
	{
		lock_owner_type guard(lock_);
		std::uint32_t current_value = base::load(std::memory_order_relaxed);
		if (current_value == value) {
			cond_.wait(guard);
			current_value = base::load(std::memory_order_relaxed);
		}
		return current_value;
	}

	void store_and_wake(std::uint32_t /*index*/, std::uint32_t value)
	{
		lock_owner_type guard(lock_);
		base::store(value, std::memory_order_relaxed);
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

	static_assert(std::is_nothrow_default_constructible<Value>::value,
		      "bounded_queue requires values with nothrow default constructor");

	bounded_queue(std::uint32_t size) : ring_{nullptr}, mask_{size - 1}
	{
		if (size < bounded_queue_min_size)
			throw std::invalid_argument("bounded_queue size must be at least 16");
		if ((size & mask_) != 0)
			throw std::invalid_argument(
				"bounded_queue size must be a power of two");

		void *ring;
		if (::posix_memalign(&ring, cache_line_size, size * sizeof(ring_slot)))
			throw std::bad_alloc();

		ring_ = new (ring) ring_slot[size];
		for (std::uint32_t i = 0; i < size; i++)
			ring_[i].open(i);
	}

	bounded_queue(bounded_queue &&other) noexcept : ring_{other.ring_}, mask_{other.mask_}
	{
		other.ring_ = nullptr;
	}

	~bounded_queue()
	{
		destroy();
	}

	//
	// State operations
	//

	void close() noexcept
	{
		const auto tail = tail_.fetch_add(mask_ + 1, std::memory_order_relaxed);
		for (std::uint32_t i = 0; i < (mask_ + 1); i++) {
			std::uint32_t index = (tail + i) & mask_;
			ring_[index].close(index);
		}
	}

	bool is_closed() const noexcept
	{
		return (ring_[0].load() & bounded_queue_closed) != 0;
	}

	bool is_empty() const noexcept
	{
		// Assume this is called with no concurrent push operations.
		auto tail = tail_.load(std::memory_order_acquire);
		auto head = head_.load(std::memory_order_relaxed);
		return int32_t(tail - head) <= 0;
	}

	bool is_full() const noexcept
	{
		// Assume this is called with no concurrent pop operations.
		auto head = head_.load(std::memory_order_acquire);
		auto tail = tail_.load(std::memory_order_relaxed);
		return int32_t(tail - head) > mask_;
	}

	static bool is_lock_free() noexcept
	{
		return false;
	}

	//
	// Basic operations
	//

	template <typename... Backoff>
	void push(const value_type &value, Backoff... backoff)
	{
		auto status = wait_push(value, std::forward<Backoff>(backoff)...);
		if (status != queue_op_status::success)
			throw status;
	}

	template <typename... Backoff>
	void push(value_type &&value, Backoff... backoff)
	{
		auto status = wait_push(std::move(value), std::forward<Backoff>(backoff)...);
		if (status != queue_op_status::success)
			throw status;
	}

	template <typename... Backoff>
	value_type value_pop(Backoff... backoff)
	{
		value_type value;
		auto status = wait_pop(value, std::forward<Backoff>(backoff)...);
		if (status != queue_op_status::success)
			throw status;
		return std::move(value);
	}

	//
	// Waiting operations
	//

	template <typename... Backoff>
	queue_op_status wait_push(const value_type &value, Backoff... backoff)
	{
		auto tail = tail_.fetch_add(1, std::memory_order_relaxed);
		auto index = tail & mask_;
		ring_slot &slot = ring_[index];

		auto status = wait_tail(slot, index, tail, std::forward<Backoff>(backoff)...);
		if (status != queue_op_status::success)
			return status;
		put_value(slot, index, tail, value);
		return queue_op_status::success;
	}

	template <typename... Backoff>
	queue_op_status wait_push(value_type &&value, Backoff... backoff)
	{
		auto tail = tail_.fetch_add(1, std::memory_order_relaxed);
		auto index = tail & mask_;
		ring_slot &slot = ring_[index];

		auto status = wait_tail(slot, index, tail, std::forward<Backoff>(backoff)...);
		if (status != queue_op_status::success)
			return status;
		put_value(slot, index, tail, value);
		return queue_op_status::success;
	}

	template <typename... Backoff>
	queue_op_status wait_pop(value_type &value, Backoff... backoff)
	{
		for (;;) {
			auto head = head_.fetch_add(1, std::memory_order_relaxed);
			auto index = head & mask_;
			ring_slot &slot = ring_[index];

			auto status = wait_head(
				slot, index, head, std::forward<Backoff>(backoff)...);
			if (status != queue_op_status::success) {
				if (status == queue_op_status::empty)
					continue;
				return status;
			}

			get_value(slot, index, head, value);
			return queue_op_status::success;
		}
	}

#if 0
	//
	// Non-waiting operations
	//

	template <typename... Backoff>
	queue_op_status try_push(const value_type &value, Backoff... backoff)
	{
	}

	template <typename... Backoff>
	queue_op_status try_push(value_type &&value, Backoff... backoff)
	{
	}

	template <typename... Backoff>
	queue_op_status try_pop(value_type &value, Backoff... backoff)
	{
	}
#endif

#if 0 && ENABLE_QUEUE_NONBLOCKING_OPS
	//
	// Non-blocking operations
	//

	queue_op_status nonblocking_pop(value_type &value)
	{
		std::uint32_t head = head_.load(std::memory_order_relaxed);
		ring_slot &slot = ring_[head & mask_];

		std::uint32_t current_ticket = slot.load();
		std::uint32_t required_ticket = (head + 1) << bq_status_bits;
		if ((current_ticket & bq_ticket_mask) != required_ticket)
			return queue_op_status::empty;

		if (!head_.compare_exchange_strong(head, head + 1, std::memory_order_relaxed))
			return queue_op_status::busy;

		get_value(slot, head, value);
		return queue_op_status::success;
	}
#endif

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

	queue_op_status wait_tail(ring_slot &slot, std::uint32_t index, std::uint32_t base)
	{
		auto ticket = slot.load();
		for (;;) {
			auto diff = ticket - base;
			if ((diff & bounded_queue_ticket_mask) == 0)
				return queue_op_status::success;
			if ((diff & bounded_queue_closed) != 0)
				return queue_op_status::closed;
			ticket = slot.wait_and_load(index, ticket);
		}
	}

	template <typename Backoff>
	queue_op_status
	wait_tail(ring_slot &slot, std::uint32_t index, std::uint32_t base, Backoff backoff)
	{
		bool waiting = false;
		auto ticket = slot.load();
		for (;;) {
			auto diff = ticket - base;
			if ((diff & bounded_queue_ticket_mask) == 0)
				return queue_op_status::success;
			if ((diff & bounded_queue_closed) != 0)
				return queue_op_status::closed;
			if (waiting) {
				ticket = slot.wait_and_load(index, ticket);
			} else {
				waiting = backoff();
				ticket = slot.load();
			}
		}
	}

	queue_op_status wait_head(ring_slot &slot, std::uint32_t index, std::uint32_t base)
	{
		auto ticket = slot.load();
		for (;;) {
			auto diff = ticket - base;
			auto status = diff & bounded_queue_status_mask;
			if (status != 0) {
				if ((diff & bounded_queue_ticket_mask) == 0) {
					if ((diff & bounded_queue_valid) != 0)
						return queue_op_status::success;
					if ((diff & bounded_queue_failed) != 0)
						return queue_op_status::empty;
				}
				if ((diff & bounded_queue_closed) != 0)
					return queue_op_status::closed;
			}
			ticket = slot.wait_and_load(index, ticket);
		}
	}

	template <typename Backoff>
	queue_op_status
	wait_head(ring_slot &slot, std::uint32_t index, std::uint32_t base, Backoff backoff)
	{
		bool waiting = false;
		auto ticket = slot.load();
		for (;;) {
			std::uint32_t diff = ticket - base;
			auto status = diff & bounded_queue_status_mask;
			if (status != 0) {
				if ((diff & bounded_queue_ticket_mask) == 0) {
					if ((diff & bounded_queue_valid) != 0)
						return queue_op_status::success;
					if ((diff & bounded_queue_failed) != 0)
						return queue_op_status::empty;
				}
				if ((diff & bounded_queue_closed) != 0)
					return queue_op_status::closed;
			}
			if (waiting) {
				ticket = slot.wait_and_load(index, ticket);
			} else {
				waiting = backoff();
				ticket = slot.load();
			}
		}
	}

	template <typename V = value_type,
		  typename std::enable_if_t<std::is_nothrow_copy_assignable<V>::value>
			  * = nullptr>
	void put_value(ring_slot &slot,
		       std::uint32_t index,
		       std::uint32_t tail,
		       const value_type &value) noexcept
	{
		slot.value = value;
		slot.store_and_wake(index, tail + bounded_queue_valid);
	}

	template <typename V = value_type,
		  typename std::enable_if_t<not std::is_nothrow_copy_assignable<V>::value>
			  * = nullptr>
	void put_value(ring_slot &slot,
		       std::uint32_t index,
		       std::uint32_t tail,
		       const value_type &value)
	{
		try {
			slot.value = value;
			slot.store_and_wake(index, tail + bounded_queue_valid);
		} catch (...) {
			slot.store_and_wake(index, tail + bounded_queue_failed);
			throw;
		}
	}

	template <typename V = value_type,
		  typename std::enable_if_t<std::is_nothrow_move_assignable<V>::value>
			  * = nullptr>
	void put_value(ring_slot &slot,
		       std::uint32_t index,
		       std::uint32_t tail,
		       value_type &&value) noexcept
	{
		slot.value = std::move(value);
		slot.store_and_wake(index, tail + bounded_queue_valid);
	}

	template <typename V = value_type,
		  typename std::enable_if_t<not std::is_nothrow_move_assignable<V>::value>
			  * = nullptr>
	void
	put_value(ring_slot &slot, std::uint32_t index, std::uint32_t tail, value_type &&value)
	{
		try {
			slot.value = std::move(value);
			slot.store_and_wake(index, tail + bounded_queue_valid);
		} catch (...) {
			slot.store_and_wake(index, tail + bounded_queue_failed);
			throw;
		}
	}

	template <typename V = value_type,
		  typename std::enable_if_t<std::is_nothrow_move_assignable<V>::value>
			  * = nullptr>
	void get_value(ring_slot &slot,
		       std::uint32_t index,
		       std::uint32_t head,
		       value_type &value) noexcept
	{
		value = std::move(slot.value);
		slot.store_and_wake(index, head + mask_ + 1);
	}

	template <typename V = value_type,
		  typename std::enable_if_t<not std::is_nothrow_move_assignable<V>::value>
			  * = nullptr>
	void
	get_value(ring_slot &slot, std::uint32_t index, std::uint32_t head, value_type &value)
	{
		try {
			value = std::move(slot.value);
			slot.store_and_wake(index, head + mask_ + 1);
		} catch (...) {
			slot.store_and_wake(index, head + mask_ + 1);
			throw;
		}
	}

	ring_slot *ring_;
	const std::uint32_t mask_;

	alignas(cache_line_size) std::atomic<std::uint32_t> head_ = {0};
	alignas(cache_line_size) std::atomic<std::uint32_t> tail_ = {0};
};

template <typename ValueType>
using default_bounded_queue = bounded_queue<ValueType, bq_slot>;

} // namespace evenk

#endif // !EVENK_BOUNDED_QUEUE_H_
