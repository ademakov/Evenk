//
// Busy-Waiting Backoff Utilities
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

#ifndef EVENK_BACKOFF_H_
#define EVENK_BACKOFF_H_

#include <atomic>
#include <cstdint>
#include <thread>

#include <emmintrin.h>
#include <time.h>

namespace evenk {

//
// Pause routines for busy waiting.
//

struct cpu_cycle
{
	void operator()(std::uint32_t n) noexcept
	{
		while (n--)
			std::atomic_signal_fence(std::memory_order_relaxed);
	}
};

struct cpu_relax
{
	void operator()(std::uint32_t n) noexcept
	{
		while (n--)
			::_mm_pause();
	}
};

struct nanosleep
{
	void operator()(std::uint32_t n) noexcept
	{
		::timespec ts = {.tv_sec = 0, .tv_nsec = n};
		::nanosleep(&ts, NULL);
	}
};

//
// Back-off policies for busy waiting.
//

//
// The return value for operator() is true if backoff ceiling is reached and
// false otherwise.
//

struct no_backoff
{
	bool operator()() noexcept
	{
		return true;
	}
};

struct yield_backoff
{
	bool operator()() noexcept
	{
		std::this_thread::yield();
		return false;
	}
};

template <typename Pause, std::uint32_t count>
struct const_backoff : Pause
{
	bool operator()() noexcept
	{
		Pause::operator()(count);
		return false;
	}
};

template <typename Pause, std::uint32_t ceiling, std::uint32_t step = 1>
class linear_backoff : Pause
{
public:
	bool operator()() noexcept
	{
		Pause::operator()(count_);
		count_ += step;
		if (count_ > ceiling) {
			count_ = ceiling;
			return true;
		}
		return false;
	}

private:
	std::uint32_t count_ = 0;
};

template <typename Pause, std::uint32_t ceiling>
class exponential_backoff : Pause
{
public:
	bool operator()() noexcept
	{
		Pause::operator()(count_);
		count_ += count_ + 1;
		if (count_ > ceiling) {
			count_ = ceiling;
			return true;
		}
		return false;
	}

private:
	std::uint32_t count_ = 0;
};

template <typename Pause, std::uint32_t count>
struct proportional_backoff : Pause
{
	bool operator()(std::uint32_t factor) noexcept
	{
		Pause::operator()(count *factor);
		return false;
	}
};

template <typename Backoff>
bool
proportional_adapter(Backoff &backoff, std::uint32_t) noexcept
{
	return backoff();
}

template <typename Pause, std::uint32_t count>
bool
proportional_adapter(proportional_backoff<Pause, count> &backoff, std::uint32_t factor) noexcept
{
	return backoff(factor);
}

template <typename FirstBackoff, typename SecondBackoff>
class composite_backoff : FirstBackoff, SecondBackoff
{
public:
	composite_backoff(FirstBackoff a, SecondBackoff b) noexcept
		: FirstBackoff(a), SecondBackoff(b), use_second_{false}
	{
	}

	bool operator()() noexcept
	{
		if (use_second_)
			return SecondBackoff::operator()();
		use_second_ = FirstBackoff::operator()();
		return false;
	}

private:
	bool use_second_;
};

} // namespace evenk

#endif // !EVENK_BACKOFF_H_
