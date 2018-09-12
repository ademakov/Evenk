//
// Thread Pool
//
// Copyright (c) 2017  Aleksey Demakov
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

#ifndef EVENK_THREAD_POOL_H_
#define EVENK_THREAD_POOL_H_

#include <atomic>

#include "basic.h"
#include "conqueue.h"
#include "synch.h"
#include "task.h"
#include "thread.h"

namespace evenk {

class thread_pool_base : non_copyable
{
public:
	thread_pool_base() noexcept = default;

	virtual ~thread_pool_base() noexcept
	{
		stop();
		wait();
	}

	std::size_t size() const noexcept
	{
		return pool_.size();
	}

	thread &operator[](std::size_t index)
	{
		return pool_[index];
	}

	bool is_stopped() const noexcept
	{
		return (flags_.load(std::memory_order_relaxed) & stop_flag);
	}

	void stop()
	{
		close(stop_flag);
	}

	void wait()
	{
		close(wait_flag);

		default_synch::lock_owner_type guard(join_lock_);
		if (!join_done_) {
			for (std::size_t i = 0; i < pool_.size(); i++)
				pool_[i].join();
			join_done_ = true;
		}
	}

protected:
	virtual void work() = 0;
	virtual void shutdown() = 0;

	void activate(std::size_t size)
	{
		if (pool_.size())
			throw std::logic_error("thread_pool is already active");

		pool_.reserve(size);
		for (std::size_t i = 0; i < size; i++)
			pool_.emplace_back(&thread_pool_base::work, this);
	}

private:
	static constexpr std::uint8_t stop_flag = 1;
	static constexpr std::uint8_t wait_flag = 2;

	std::vector<thread> pool_;

	std::atomic<std::uint8_t> flags_ = ATOMIC_VAR_INIT(0);

	default_synch::lock_type join_lock_;
	bool join_done_ = false;

	void close(std::uint8_t flag)
	{
		if (flags_.fetch_or(flag, std::memory_order_relaxed) == 0)
			shutdown();
	}
};

template <template <typename> class Queue,
	  std::size_t S = 2 * fptr_size,
	  typename A = std::allocator<char>>
class thread_pool final : public thread_pool_base
{
public:
	using allocator_type = A;

	static constexpr std::size_t task_size = S;
	using task_type = task<void, task_size, allocator_type>;

	using queue_type = Queue<task_type>;

	template <typename... QueueArgs>
	thread_pool(std::size_t size, QueueArgs... queue_args)
		: thread_pool_base(), queue_(queue_args...)
	{
		activate(size);
	}

	template <typename... QueueArgs>
	thread_pool(std::size_t size, const allocator_type &alloc, QueueArgs... queue_args)
		: thread_pool_base(), queue_(queue_args...), alloc_(alloc)
	{
		activate(size);
	}

	template <typename Callable>
	void submit(Callable &&callable)
	{
		queue_.push(task_type(std::forward<Callable>(callable), alloc_));
	}

private:
	queue_type queue_;
	allocator_type alloc_;

	virtual void work() override
	{
		while (!is_stopped()) {
			task_type task;

			auto status = queue_.wait_pop(task);
			if (status != queue_op_status::success) {
				if (status == queue_op_status::closed)
					break;
				continue;
			}

			task();
		}
	}

	virtual void shutdown() override
	{
		queue_.close();
	}
};

} // namespace evenk

#endif // !EVENK_THREAD_POOL_H_
