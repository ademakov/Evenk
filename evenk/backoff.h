//
// Busy-Waiting Backoff Utilities
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

struct CPUCycle
{
	void operator()(std::uint32_t n)
	{
		while (n--)
			std::atomic_signal_fence(std::memory_order_relaxed);
	}
};

struct CPURelax
{
	void operator()(std::uint32_t n)
	{
		while (n--)
			::_mm_pause();
	}
};

struct NanoSleep
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

class NoBackoff
{
public:
	bool operator()()
	{
		return true;
	}
};

class YieldBackoff
{
public:
	bool operator()()
	{
		std::this_thread::yield();
		return false;
	}
};

template <typename Pause>
class LinearBackoff
{
public:
	LinearBackoff(std::uint32_t ceiling) noexcept : ceiling_{ceiling}, backoff_{0}
	{
	}

	bool operator()()
	{
		if (backoff_ >= ceiling_) {
			pause_(ceiling_);
			return true;
		} else {
			pause_(backoff_++);
			return false;
		}
	}

private:
	const std::uint32_t ceiling_;
	std::uint32_t backoff_;
	Pause pause_;
};

template <typename Pause>
class ExponentialBackoff
{
public:
	ExponentialBackoff(std::uint32_t ceiling) noexcept : ceiling_{ceiling}, backoff_{0}
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
class ProportionalBackoff
{
public:
	ProportionalBackoff(std::uint32_t backoff) noexcept : backoff_{backoff}
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

template <typename FirstBackoff, typename SecondBackoff>
class CompositeBackoff
{
public:
	CompositeBackoff(FirstBackoff a, SecondBackoff b) noexcept
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
