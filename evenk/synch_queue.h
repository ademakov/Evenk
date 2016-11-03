//
// Simple Concurrent Queue
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

#ifndef EVENK_SYNCH_QUEUE_H_
#define EVENK_SYNCH_QUEUE_H_

#include <deque>

#include "synch.h"

namespace evenk {

template <typename Value, typename Synch = default_synch, typename Sequence = std::deque<Value>>
class queue
{
public:
	using lock_type = typename Synch::lock_type;
	using cond_var_type = typename Synch::cond_var_type;
	using lock_owner_type = typename Synch::lock_owner_type;

	using sequence_type = Sequence;

	queue() noexcept : finish_(false)
	{
	}

	queue(queue &&other) noexcept : finish_(other.finish_)
	{
		std::swap(queue_, other.queue_);
	}

	bool empty() const
	{
		lock_owner_type guard(lock_);
		return queue_.empty();
	}

	bool finished() const
	{
		return finish_;
	}

	void finish()
	{
		lock_owner_type guard(lock_);
		finish_ = true;
		cond_.notify_all();
	}

	template <typename... Backoff>
	void enqueue(Value &&data, Backoff... backoff)
	{
		lock_owner_type guard(lock_, std::forward<Backoff>(backoff)...);
		queue_.push_back(std::move(data));
		cond_.notify_one();
	}

	template <typename... Backoff>
	void enqueue(const Value &data, Backoff... backoff)
	{
		lock_owner_type guard(lock_, std::forward<Backoff>(backoff)...);
		queue_.push_back(data);
		cond_.notify_one();
	}

	template <typename... Backoff>
	bool dequeue(Value &data, Backoff... backoff)
	{
		lock_owner_type guard(lock_, std::forward<Backoff>(backoff)...);
		while (queue_.empty()) {
			if (finished())
				return false;
			cond_.wait(guard);
		}
		data = std::move(queue_.front());
		queue_.pop_front();
		return true;
	}

private:
	bool finish_;
	lock_type lock_;
	cond_var_type cond_;
	sequence_type queue_;
};

} // namespace evenk

#endif // !EVENK_SYNCH_QUEUE_H_
