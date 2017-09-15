//
// Simple Concurrent Queue
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

#ifndef EVENK_SYNCH_QUEUE_H_
#define EVENK_SYNCH_QUEUE_H_

#include <deque>

#include "conqueue.h"
#include "synch.h"

namespace evenk {

template <typename Value, typename Synch = default_synch, typename Sequence = std::deque<Value>>
class synch_queue : non_copyable
{
public:
	using value_type = Value;
	using reference = value_type &;
	using const_reference = const value_type &;

	using lock_type = typename Synch::lock_type;
	using cond_var_type = typename Synch::cond_var_type;
	using lock_owner_type = typename Synch::lock_owner_type;

	using sequence_type = Sequence;

	synch_queue() noexcept : closed_(false)
	{
	}

	synch_queue(synch_queue &&other) noexcept : closed_(other.closed_)
	{
		std::swap(queue_, other.queue_);
	}

	//
	// State operations
	//

	void close() noexcept
	{
		lock_owner_type guard(lock_);
		closed_ = true;
		cond_.notify_all();
	}

	bool is_closed() const noexcept
	{
		lock_owner_type guard(lock_);
		return closed_;
	}

	bool is_empty() const noexcept
	{
		lock_owner_type guard(lock_);
		return queue_.empty();
	}

	bool is_full() const noexcept
	{
		return false;
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
		return try_push(value, std::forward<Backoff>(backoff)...);
	}

	template <typename... Backoff>
	queue_op_status wait_push(value_type &&value, Backoff... backoff)
	{
		return try_push(std::move(value), std::forward<Backoff>(backoff)...);
	}

	template <typename... Backoff>
	queue_op_status wait_pop(value_type &value, Backoff... backoff)
	{
		lock_owner_type guard(lock_, std::forward<Backoff>(backoff)...);
		auto status = locked_pop(value);
		while (status == queue_op_status::empty) {
			cond_.wait(guard);
			status = locked_pop(value);
		}
		return status;
	}

	//
	// Non-waiting operations
	//

	template <typename... Backoff>
	queue_op_status try_push(const value_type &value, Backoff... backoff)
	{
		lock_owner_type guard(lock_, std::forward<Backoff>(backoff)...);
		return locked_push(value);
	}

	template <typename... Backoff>
	queue_op_status try_push(value_type &&value, Backoff... backoff)
	{
		lock_owner_type guard(lock_, std::forward<Backoff>(backoff)...);
		return locked_push(std::move(value));
	}

	template <typename... Backoff>
	queue_op_status try_pop(value_type &value, Backoff... backoff)
	{
		lock_owner_type guard(lock_, std::forward<Backoff>(backoff)...);
		return locked_pop(value);
	}

#if ENABLE_QUEUE_NONBLOCKING_OPS
	//
	// Non-blocking operations
	//

	queue_op_status nonblocking_push(const value_type &value)
	{
		lock_owner_type guard(lock_, std::try_to_lock);
		if (!guard.owns_lock())
			return queue_op_status::busy;
		return locked_push(value);
	}

	queue_op_status nonblocking_push(value_type &&value)
	{
		lock_owner_type guard(lock_, std::try_to_lock);
		if (!guard.owns_lock())
			return queue_op_status::busy;
		return locked_push(std::move(value));
	}

	queue_op_status nonblocking_pop(value_type &value)
	{
		lock_owner_type guard(lock_, std::try_to_lock);
		if (!guard.owns_lock())
			return queue_op_status::busy;
		return locked_pop(value);
	}
#endif // ENABLE_QUEUE_NONBLOCKING_OPS

private:
	queue_op_status locked_push(const value_type &value)
	{
		if (closed_)
			return queue_op_status::closed;

		queue_.push_back(value);
		cond_.notify_one();
		return queue_op_status::success;
	}

	queue_op_status locked_push(value_type &&value)
	{
		if (closed_)
			return queue_op_status::closed;

		queue_.push_back(std::move(value));
		cond_.notify_one();
		return queue_op_status::success;
	}

	queue_op_status locked_pop(value_type &value)
	{
		if (queue_.empty()) {
			if (closed_)
				return queue_op_status::closed;
			return queue_op_status::empty;
		}

		value = std::move(queue_.front());
		queue_.pop_front();
		return queue_op_status::success;
	}

	bool closed_;
	mutable lock_type lock_;
	cond_var_type cond_;
	sequence_type queue_;
};

} // namespace evenk

#endif // !EVENK_SYNCH_QUEUE_H_
