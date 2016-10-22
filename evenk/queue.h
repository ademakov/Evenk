//
// Simple Concurrent Queue
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

#ifndef EVENK_QUEUE_H_
#define EVENK_QUEUE_H_

#include <deque>

#include "evenk/synch.h"

namespace evenk {
namespace concurrency {

template <typename ValueType,
	  typename SynchPolicy = DefaultSynch,
	  typename Sequence = std::deque<ValueType>>
class Queue
{
public:
	Queue() noexcept : finish_(false)
	{
	}

	Queue(Queue &&other) noexcept : finish_(other.finish_)
	{
		std::swap(queue_, other.queue_);
	}

	bool Empty() const
	{
		LockGuard<LockType> guard(lock_);
		return queue_.empty();
	}

	bool Finished() const
	{
		return finish_;
	}

	void Finish()
	{
		LockGuard<LockType> guard(lock_);
		finish_ = true;
		cond_.NotifyAll();
	}

	template <typename... Backoff>
	void Enqueue(ValueType &&data, Backoff... backoff)
	{
		LockGuard<LockType> guard(lock_, std::forward<Backoff>(backoff)...);
		queue_.push_back(std::move(data));
		cond_.NotifyOne();
	}

	template <typename... Backoff>
	void Enqueue(const ValueType &data, Backoff... backoff)
	{
		LockGuard<LockType> guard(lock_, std::forward<Backoff>(backoff)...);
		queue_.push_back(data);
		cond_.NotifyOne();
	}

	template <typename... Backoff>
	bool Dequeue(ValueType &data, Backoff... backoff)
	{
		LockGuard<LockType> guard(lock_, std::forward<Backoff>(backoff)...);
		while (queue_.empty()) {
			if (Finished())
				return false;
			cond_.Wait(guard);
		}
		data = std::move(queue_.front());
		queue_.pop_front();
		return true;
	}

private:
	using LockType = typename SynchPolicy::LockType;
	using CondVarType = typename SynchPolicy::CondVarType;

	bool finish_;
	LockType lock_;
	CondVarType cond_;
	Sequence queue_;
};

} // namespace concurrency
} // namespace evenk

#endif // !EVENK_QUEUE_H_
