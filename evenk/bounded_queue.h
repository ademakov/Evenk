//
// Fast Bounded Concurrent Queue
//
// Copyright (c) 2015-2018  Aleksey Demakov
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
namespace bounded_queue {

// The type used to count ring slots.
typedef std::uint32_t count_t;
// The type used to mark ring slots and as a futex too.
typedef std::uint32_t token_t;

namespace detail {

// Status flags for ring slots.
enum status_t : token_t {
	// the slot contains a value
	status_valid = 1,
	// the slot is closed
	status_closed = 2,
	// the slot contains invalid value
	status_invalid = 4,
	// there is a waiting thread on the slot (if futex-based)
	status_waiting = 8,
};

// The bit mask that selects value status.
constexpr token_t status_mask = status_valid | status_closed | status_invalid;

// The bit mask that selects ticket number.
constexpr token_t ticket_mask = ~(status_mask | status_waiting);

// The minimum queue size that allows combined slot status and ticket encoding.
constexpr count_t min_size = 16;

// Single-threaded slot counter.
class counter
{
public:
	count_t load() const
	{
		return count_;
	}

	count_t fetch_add(count_t addend)
	{
		count_t count = count_;
		count_ += addend;
		return count;
	}

private:
	count_t count_ = 0;
};

// Multi-threaded slot counter.
class atomic_counter
{
public:
	count_t load() const
	{
		return count_.load(std::memory_order_relaxed);
	}

	count_t fetch_add(count_t addend)
	{
		return count_.fetch_add(addend, std::memory_order_relaxed);
	}

private:
	std::atomic<count_t> count_ = {0};
};

} // namespace detail

class spin : protected std::atomic<token_t>
{
public:
	using base = std::atomic<token_t>;

	void init(token_t token)
	{
		store(token, std::memory_order_relaxed);
	}

	void close()
	{
		fetch_or(detail::status_closed, std::memory_order_relaxed);
	}

	token_t load() const
	{
		return base::load(std::memory_order_acquire);
	}

	token_t wait(token_t /*token*/)
	{
		return base::load(std::memory_order_relaxed);
	}

	void wake(token_t token)
	{
		store(token, std::memory_order_release);
	}
};

class yield : public spin
{
public:
	token_t wait(token_t /*token*/)
	{
		std::this_thread::yield();
		return base::load(std::memory_order_relaxed);
	}
};

class futex : public spin
{
public:
	void close()
	{
		token_t t = base::load(std::memory_order_relaxed);
		while (!compare_exchange_weak(t, t | detail::status_closed,
					      std::memory_order_relaxed,
					      std::memory_order_relaxed)) {
		}
		if ((t & detail::status_waiting) != 0)
			futex_wake(*this, INT32_MAX);
	}

	token_t wait(token_t token)
	{
		token_t t = token;
		token_t w = token | detail::status_waiting;
		if (compare_exchange_strong(t, w,
					    std::memory_order_relaxed,
					    std::memory_order_relaxed)
		    || t == w) {
			futex_wait(*this, w);
			t = base::load(std::memory_order_relaxed);
		}
		return t;
	}

	void wake(token_t token)
	{
		token = exchange(token, std::memory_order_release);
		if ((token & detail::status_waiting) != 0)
			futex_wake(*this, INT32_MAX);
	}
};

template <typename Synch = default_synch>
class synch : public spin
{
public:
	using lock_type = typename Synch::lock_type;
	using cond_var_type = typename Synch::cond_var_type;
	using lock_owner_type = typename Synch::lock_owner_type;

	void close()
	{
		lock_owner_type guard(lock_);
		fetch_or(detail::status_closed, std::memory_order_relaxed);
		cond_.notify_all();
	}

	token_t wait(token_t token)
	{
		lock_owner_type guard(lock_);
		token_t t = base::load(std::memory_order_relaxed);
		if (t == token) {
			cond_.wait(guard);
			t = base::load(std::memory_order_relaxed);
		}
		return t;
	}

	void wake(token_t token)
	{
		lock_owner_type guard(lock_);
		store(token, std::memory_order_relaxed);
		cond_.notify_all();
	}

private:
	lock_type lock_;
	cond_var_type cond_;
};

template <typename Value, typename Slot, typename ProducerCounter, typename ConsumerCounter>
class ring : non_copyable
{
public:
	using value_type = Value;
	using reference = value_type &;
	using const_reference = const value_type &;

	static_assert(std::is_nothrow_default_constructible<Value>::value,
		      "bounded_queue requires values with nothrow default constructor");

	ring(count_t size) : ring_{nullptr}, mask_{size - 1}
	{
		if (size < detail::min_size)
			throw std::invalid_argument("bounded_queue size must be at least 16");
		if ((size & mask_) != 0)
			throw std::invalid_argument(
				"bounded_queue size must be a power of two");

		void *ring;
		if (::posix_memalign(&ring, cache_line_size, size * sizeof(ring_slot)))
			throw std::bad_alloc();

		ring_ = new (ring) ring_slot[size];
		for (count_t i = 0; i < size; i++)
			ring_[i].init(i & detail::ticket_mask);
	}

	ring(ring &&other) noexcept : ring_{other.ring_}, mask_{other.mask_}
	{
		other.ring_ = nullptr;
	}

	~ring()
	{
		destroy();
	}

	//
	// State operations
	//

	void close() noexcept
	{
		count_t count = tail_.fetch_add(mask_ + 1);
		for (count_t i = 0; i < (mask_ + 1); i++) {
			ring_[(count++ & mask_)].close();
		}
	}

	bool is_closed() const noexcept
	{
		return (ring_[0].load() & detail::status_closed) != 0;
	}

	bool is_empty() const noexcept
	{
		// Assume this is called with no concurrent push operations.
		count_t tail = tail_.load(std::memory_order_acquire);
		count_t head = head_.load(std::memory_order_relaxed);
		return std::make_signed_t<count_t>(tail - head) <= 0;
	}

	bool is_full() const noexcept
	{
		// Assume this is called with no concurrent pop operations.
		count_t head = head_.load(std::memory_order_acquire);
		count_t tail = tail_.load(std::memory_order_relaxed);
		return std::make_signed_t<count_t>(tail - head) > mask_;
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
	void push(value_type &&value, Backoff &&... backoff)
	{
		auto status = wait_push(std::move(value), std::forward<Backoff>(backoff)...);
		if (status != queue_op_status::success)
			throw status;
	}

	template <typename... Backoff>
	value_type value_pop(Backoff &&... backoff)
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
	queue_op_status wait_push(const value_type &value, Backoff &&... backoff)
	{
		const count_t count = tail_.fetch_add(1);
		const token_t token = count & detail::ticket_mask;

		ring_slot &slot = ring_[count & mask_];
		auto status = wait_tail(slot, token, std::forward<Backoff>(backoff)...);
		if (status != queue_op_status::success)
			return status;

		put_value(slot, token, value);
		return queue_op_status::success;
	}

	template <typename... Backoff>
	queue_op_status wait_push(value_type &&value, Backoff &&... backoff)
	{
		const count_t count = tail_.fetch_add(1);
		const token_t token = count & detail::ticket_mask;

		ring_slot &slot = ring_[count & mask_];
		auto status = wait_tail(slot, token, std::forward<Backoff>(backoff)...);
		if (status != queue_op_status::success)
			return status;

		put_value(slot, token, std::move(value));
		return queue_op_status::success;
	}

	template <typename... Backoff>
	queue_op_status wait_pop(value_type &value, Backoff &&... backoff)
	{
		for (;;) {
			const count_t count = head_.fetch_add(1);
			const token_t token = count & detail::ticket_mask;

			ring_slot &slot = ring_[count & mask_];
			auto status = wait_head(slot, token, std::forward<Backoff>(backoff)...);
			if (status != queue_op_status::success) {
				if (status == queue_op_status::empty)
					continue;
				return status;
			}

			get_value(slot, token, value);
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
	struct alignas(cache_line_size) ring_slot : public Slot
	{
		value_type value;
	};

	void destroy()
	{
		if (ring_ != nullptr) {
			count_t size = mask_ + 1;
			for (count_t i = 0; i < size; i++)
				ring_[i].~ring_slot();
			std::free(ring_);
		}
	}

	queue_op_status wait_tail(ring_slot &slot, token_t token)
	{
		token_t t = slot.load();
		while ((t & detail::ticket_mask) != token) {
			if ((t & detail::status_closed) != 0)
				return queue_op_status::closed;
			t = slot.wait(t);
		}
		return queue_op_status::success;
	}

	template <typename Backoff>
	queue_op_status wait_tail(ring_slot &slot, token_t token, Backoff backoff)
	{
		bool waiting = false;
		token_t t = slot.load();
		while ((t & detail::ticket_mask) != token) {
			if ((t & detail::status_closed) != 0)
				return queue_op_status::closed;
			if (waiting) {
				t = slot.wait(t);
				continue;
			}
			waiting = backoff();
			t = slot.load();
		}
		return queue_op_status::success;
	}

	queue_op_status wait_head(ring_slot &slot, token_t token)
	{
		token_t t = slot.load();
		for (;;) {
			if ((t & detail::status_mask) != 0) {
				if ((t & detail::ticket_mask) == token) {
					if ((t & detail::status_valid) != 0)
						return queue_op_status::success;
					if ((t & detail::status_invalid) != 0)
						return queue_op_status::empty;
				}
				if ((t & detail::status_closed) != 0)
					return queue_op_status::closed;
			}
			t = slot.wait(t);
		}
	}

	template <typename Backoff>
	queue_op_status
	wait_head(ring_slot &slot, token_t token, Backoff backoff)
	{
		bool waiting = false;
		token_t t = slot.load();
		for (;;) {
			if ((t & detail::status_mask) != 0) {
				if ((t & detail::ticket_mask) == token) {
					if ((t & detail::status_valid) != 0)
						return queue_op_status::success;
					if ((t & detail::status_invalid) != 0)
						return queue_op_status::empty;
				}
				if ((t & detail::status_closed) != 0)
					return queue_op_status::closed;
			}
			if (waiting) {
				t = slot.wait(t);
				continue;
			}
			waiting = backoff();
			t = slot.load();
		}
	}

	template <typename V = value_type,
		  typename std::enable_if_t<std::is_nothrow_copy_assignable<V>::value>
			  * = nullptr>
	void put_value(ring_slot &slot, count_t token, const value_type &value) noexcept
	{
		slot.value = value;
		slot.wake(token + detail::status_valid);
	}

	template <typename V = value_type,
		  typename std::enable_if_t<not std::is_nothrow_copy_assignable<V>::value>
			  * = nullptr>
	void put_value(ring_slot &slot, token_t token, const value_type &value)
	{
		try {
			slot.value = value;
			slot.wake(token | detail::status_valid);
		} catch (...) {
			slot.wake(token | detail::status_invalid);
			throw;
		}
	}

	template <typename V = value_type,
		  typename std::enable_if_t<std::is_nothrow_move_assignable<V>::value>
			  * = nullptr>
	void put_value(ring_slot &slot, token_t token, value_type &&value) noexcept
	{
		slot.value = std::move(value);
		slot.wake(token | detail::status_valid);
	}

	template <typename V = value_type,
		  typename std::enable_if_t<not std::is_nothrow_move_assignable<V>::value>
			  * = nullptr>
	void put_value(ring_slot &slot, token_t token, value_type &&value)
	{
		try {
			slot.value = std::move(value);
			slot.wake(token | detail::status_valid);
		} catch (...) {
			slot.wake(token | detail::status_invalid);
			throw;
		}
	}

	template <typename V = value_type,
		  typename std::enable_if_t<std::is_nothrow_move_assignable<V>::value>
			  * = nullptr>
	void get_value(ring_slot &slot, token_t token, value_type &value) noexcept
	{
		value = std::move(slot.value);
		slot.wake(token + mask_ + 1);
	}

	template <typename V = value_type,
		  typename std::enable_if_t<not std::is_nothrow_move_assignable<V>::value>
			  * = nullptr>
	void get_value(ring_slot &slot, token_t token, value_type &value)
	{
		try {
			value = std::move(slot.value);
			slot.wake(token + mask_ + 1);
		} catch (...) {
			slot.wake(token + mask_ + 1);
			throw;
		}
	}

	ring_slot *ring_;
	const count_t mask_;

	alignas(cache_line_size) ConsumerCounter head_;
	alignas(cache_line_size) ProducerCounter tail_;
};

// A single-producer single-consumer queue.
template <typename Value, typename Slot = spin>
using spsc = ring<Value, Slot, detail::counter, detail::counter>;

// A single-producer multi-consumer queue.
template <typename Value, typename Slot = spin>
using spmc = ring<Value, Slot, detail::counter, detail::atomic_counter>;

// A multi-producer single-consumer queue.
template <typename Value, typename Slot = spin>
using mpsc = ring<Value, Slot, detail::atomic_counter, detail::counter>;

// A multi-producer multi-consumer queue.
template <typename Value, typename Slot = spin>
using mpmc = ring<Value, Slot, detail::atomic_counter, detail::atomic_counter>;

} // namespace bounded_queue
} // namespace evenk

#endif // !EVENK_BOUNDED_QUEUE_H_
