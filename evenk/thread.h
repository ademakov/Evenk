//
// Single Thread with CPU Affinity Support
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

#ifndef EVENK_THREAD_H_
#define EVENK_THREAD_H_

#include "config.h"

#include <thread>
#include <utility>
#include <vector>

#include <pthread.h>
#ifdef HAVE_SCHED_H
#include <sched.h>
#endif

#include "basic.h"

namespace evenk {

class thread : public std::thread
{
public:
	using cpuset_type = std::vector<bool>;

	thread() noexcept = default;

	template <class Func, class... Args>
	explicit thread(Func &&f, Args &&... args)
		: std::thread(std::forward<Func>(f), std::forward<Args>(args)...)
	{
	}

	// Move from evenk::thread
	thread(thread &&other) noexcept
	{
		std::thread::swap(other);
	}
	thread &operator=(thread &&other) noexcept
	{
		std::thread::swap(other);
		return *this;
	}

	// Move from std::thread
	thread(std::thread &&other) noexcept
	{
		std::thread::swap(other);
	}
	thread &operator=(std::thread &&other) noexcept
	{
		std::thread::swap(other);
		return *this;
	}

#if HAVE_PTHREAD_SETAFFINITY_NP

	void affinity(const cpuset_type &cpuset)
	{
		auto handle = native_handle();
		if (!joinable())
			throw_system_error(EINVAL, "affinity");

		int cpu_num = cpuset.size();
		// TODO: use CPU_ALLOC instead
		if (cpu_num > CPU_SETSIZE)
			cpu_num = CPU_SETSIZE;

		cpu_set_t native_cpuset;
		CPU_ZERO(&native_cpuset);
		for (int cpu = 0; cpu < cpu_num; cpu++) {
			if (cpuset[cpu])
				CPU_SET(cpu, &native_cpuset);
		}

		int rc = pthread_setaffinity_np(handle, sizeof native_cpuset, &native_cpuset);
		if (rc != 0)
			throw_system_error(rc, "pthread_setaffinity_np");
	}

	cpuset_type affinity()
	{
		auto handle = native_handle();
		if (!joinable())
			throw_system_error(EINVAL, "affinity");

		int cpu_num = std::thread::hardware_concurrency();
		// TODO: use CPU_ALLOC instead
		if (cpu_num > CPU_SETSIZE)
			cpu_num = CPU_SETSIZE;

		cpu_set_t native_cpuset;
		int rc = pthread_getaffinity_np(handle, sizeof native_cpuset, &native_cpuset);
		if (rc != 0)
			throw_system_error(rc, "pthread_getaffinity_np");

		cpuset_type cpuset;
		for (int cpu = 0; cpu < cpu_num; cpu++)
			cpuset.push_back(CPU_ISSET(cpu, &native_cpuset));

		return cpuset;
	}

#else // HAVE_PTHREAD_SETAFFINITY_NP

	void affinity(const cpuset_type &)
	{
		if (!joinable())
			throw_system_error(EINVAL, "affinity");
	}

	cpuset_type affinity()
	{
		if (!joinable())
			throw_system_error(EINVAL, "affinity");
		return cpuset_type();
	}

#endif // !HAVE_PTHREAD_SETAFFINITY_NP
};

} // namespace evenk

#endif // !EVENK_THREAD_H_
