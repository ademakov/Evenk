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
#include "synch.h"
#include "task.h"
#include "thread.h"

namespace evenk {

template <template <typename> class Queue,
	  std::size_t S = 2 * fptr_size,
	  typename A = std::allocator<char>>
class thread_pool : non_copyable
{
public:
	using allocator_type = A;

	static constexpr std::size_t task_size = S;
	using task_type = task<void, task_size, allocator_type>;

	using queue_type = Queue<task_type>;

	using cpuset_type = thread::cpuset_type;

	template <typename... QueueArgs>
	thread_pool(std::size_t size, QueueArgs... queue_args) : queue_(queue_args...)
	{
		pool_.reserve(size);
		for (std::size_t i = 0; i < size; i++)
			pool_.emplace_back(&thread_pool<Queue, S, A>::work, this);
	}

	template <typename... QueueArgs>
	thread_pool(std::size_t size, const allocator_type &alloc, QueueArgs... queue_args)
		: queue_(queue_args...), allocator_(alloc)
	{
		pool_.reserve(size);
		for (std::size_t i = 0; i < size; i++)
			pool_.emplace_back(&thread_pool<Queue>::work, this);
	}

	~thread_pool()
	{
		stop();
		wait();
	}

	std::size_t size() noexcept
	{
		return pool_.size();
	}

	void affinity(std::size_t thread, const cpuset_type &cpuset)
	{
		pool_[thread].affinity(cpuset);
	}

	cpuset_type affinity(std::size_t thread)
	{
		return pool_[thread].affinity();
	}

	template <typename Callable>
	void submit(Callable &&callable)
	{
		queue_.push(task_type(std::forward<Callable>(callable), allocator_));
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

private:
	static constexpr std::uint8_t stop_flag = 1;
	static constexpr std::uint8_t wait_flag = 2;

	void work()
	{
		while ((flags_.load(std::memory_order_relaxed) & stop_flag) == 0) {
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

	void close(std::uint8_t flag)
	{
		if (flags_.fetch_or(flag, std::memory_order_relaxed) == 0)
			queue_.close();
	}

	std::vector<thread> pool_;
	queue_type queue_;
	allocator_type allocator_;

	std::atomic<std::uint8_t> flags_ = ATOMIC_VAR_INIT(0);

	default_synch::lock_type join_lock_;
	bool join_done_ = false;
};

} // namespace evenk

#endif // !EVENK_THREAD_POOL_H_
