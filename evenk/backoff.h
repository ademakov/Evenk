//
// Busy-Waiting Backoff Utilities
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

#ifndef EVENK_BACKOFF_H_
#define EVENK_BACKOFF_H_

#include <atomic>
#include <cstdint>
#include <thread>

#include <emmintrin.h>
#include <time.h>

namespace evenk {

//
// Delays for busy waiting.
//

struct cpu_cycle
{
	void operator()(std::uint32_t n)
	{
		while (n--)
			std::atomic_signal_fence(std::memory_order_relaxed);
	}
};

struct cpu_relax
{
	void operator()(std::uint32_t n)
	{
		while (n--)
			::_mm_pause();
	}
};

struct nanosleep
{
	void operator()(std::uint32_t n)
	{
		::timespec ts = {.tv_sec = 0, .tv_nsec = n};
		::nanosleep(&ts, NULL);
	}
};

//
// Back-off policies for busy waiting.
//

class no_backoff
{
public:
	bool operator()()
	{
		return true;
	}
};

class yield_backoff
{
public:
	bool operator()()
	{
		std::this_thread::yield();
		return false;
	}
};

template <typename Pause>
class const_backoff
{
public:
	const_backoff(std::uint32_t backoff) noexcept : backoff_{backoff}
	{
	}

	bool operator()(std::uint32_t factor = 1) noexcept
	{
		pause_(backoff_ * factor);
		return false;
	}

private:
	std::uint32_t backoff_;
	Pause pause_;
};

template <typename Pause>
class linear_backoff
{
public:
	linear_backoff(std::uint32_t ceiling, std::uint32_t step) noexcept
		: ceiling_{ceiling}, step_{step}, backoff_{0}
	{
	}

	bool operator()()
	{
		if (backoff_ >= ceiling_) {
			pause_(ceiling_);
			return true;
		} else {
			pause_(backoff_);
			backoff_ += step_;
			return false;
		}
	}

private:
	const std::uint32_t ceiling_;
	const std::uint32_t step_;
	std::uint32_t backoff_;
	Pause pause_;
};

template <typename Pause>
class exponential_backoff
{
public:
	exponential_backoff(std::uint32_t ceiling) noexcept : ceiling_{ceiling}, backoff_{0}
	{
	}

	bool operator()()
	{
		if (backoff_ >= ceiling_) {
			pause_(ceiling_);
			return true;
		} else {
			pause_(backoff_);
			backoff_ += backoff_ + 1;
			return false;
		}
	}

private:
	const std::uint32_t ceiling_;
	std::uint32_t backoff_;
	Pause pause_;
};

template <typename Pause>
class proportional_backoff
{
public:
	proportional_backoff(std::uint32_t backoff) noexcept : backoff_{backoff}
	{
	}

	bool operator()(std::uint32_t factor) noexcept
	{
		pause_(backoff_ * factor);
		return false;
	}

private:
	std::uint32_t backoff_;
	Pause pause_;
};

template <typename Backoff>
bool
proportional_adapter(Backoff &backoff, std::uint32_t)
{
	return backoff();
}

template <typename Pause>
bool
proportional_adapter(proportional_backoff<Pause> &backoff, std::uint32_t factor)
{
	return backoff(factor);
}

template <typename FirstBackoff, typename SecondBackoff>
class composite_backoff
{
public:
	composite_backoff(FirstBackoff a, SecondBackoff b) noexcept
		: first_(a), second_(b), use_second_{false}
	{
	}

	bool operator()() noexcept
	{
		if (use_second_)
			return second_();
		use_second_ = first_();
		return false;
	}

private:
	FirstBackoff first_;
	SecondBackoff second_;
	bool use_second_;
};

} // namespace evenk

#endif // !EVENK_BACKOFF_H_
