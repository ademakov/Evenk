#include "evenk/spinlock.h"
#include "evenk/synch.h"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

std::mutex mutex;
evenk::posix_mutex posix_mutex;
evenk::spin_lock spin_lock;
evenk::tatas_lock tatas_lock;
evenk::ticket_lock ticket_lock;
evenk::futex_lock futex_lock;

evenk::no_backoff no_backoff;
evenk::yield_backoff yield_backoff;

evenk::const_backoff<evenk::cpu_cycle, 40> const_cycle_backoff;
evenk::linear_backoff<evenk::cpu_cycle, 100, 20> linear_cycle_backoff;
evenk::exponential_backoff<evenk::cpu_cycle, 40> exponential_cycle_backoff;
evenk::proportional_backoff<evenk::cpu_cycle, 20u> proportional_cycle_backoff;

evenk::const_backoff<evenk::cpu_relax, 1> const_relax_backoff;
evenk::const_backoff<evenk::cpu_relax, 2> const_relax_x2_backoff;
evenk::const_backoff<evenk::cpu_relax, 4> const_relax_x4_backoff;
evenk::const_backoff<evenk::cpu_relax, 6> const_relax_x6_backoff;
evenk::const_backoff<evenk::cpu_relax, 8> const_relax_x8_backoff;
evenk::linear_backoff<evenk::cpu_relax, 10, 2> linear_relax_backoff;
evenk::exponential_backoff<evenk::cpu_relax, 5> exponential_relax_backoff;
evenk::proportional_backoff<evenk::cpu_relax, 1u> proportional_relax_backoff;

evenk::composite_backoff<evenk::linear_backoff<evenk::cpu_cycle, 100, 20>, evenk::yield_backoff>
	cycle_yield_backoff(linear_cycle_backoff, yield_backoff);
evenk::composite_backoff<evenk::linear_backoff<evenk::cpu_relax, 10, 2>, evenk::yield_backoff>
	relax_yield_backoff(linear_relax_backoff, yield_backoff);

template <typename Lock, typename... Backoff>
void
spin(int &count, Lock &lock, Backoff... backoff)
{
	for (int i = 0; i < 100 * 1000; ++i) {
		lock.lock(backoff...);
		evenk::cpu_cycle{}(5000);
		++count;
		lock.unlock();
		evenk::cpu_cycle{}(5000);
	}
}

template <typename Lock, typename... Backoff>
void
bench(unsigned nthreads, std::string const &name, Lock &lock, Backoff... backoff)
{
	int count = 0;

	std::vector<std::thread> v;
	v.reserve(nthreads);

	auto start = std::chrono::steady_clock::now();

	for (unsigned i = 0; i < nthreads; ++i)
		v.emplace_back(
			spin<Lock, Backoff...>, std::ref(count), std::ref(lock), backoff...);
	for (auto &t : v)
		t.join();

	auto end = std::chrono::steady_clock::now();
	std::chrono::duration<double> diff = end - start;

	std::cout << name << ": count=" << count << ", duration=" << diff.count() << "\n";
}

void
bench(unsigned nthreads, unsigned hardware_nthreads)
{
	std::cout << "Threads: " << nthreads << "\n";

#define BENCH1(lock) bench(nthreads, #lock, lock)
#define BENCH2(lock, backoff) bench(nthreads, #lock " " #backoff, lock, backoff)

	BENCH1(mutex);
	BENCH1(posix_mutex);

#if __linux__
	BENCH2(futex_lock, no_backoff);
	BENCH2(futex_lock, linear_cycle_backoff);
	BENCH2(futex_lock, exponential_cycle_backoff);
	BENCH2(futex_lock, linear_relax_backoff);
	BENCH2(futex_lock, exponential_relax_backoff);
#endif

	BENCH2(spin_lock, no_backoff);
	BENCH2(spin_lock, const_cycle_backoff);
	BENCH2(spin_lock, linear_cycle_backoff);
	BENCH2(spin_lock, exponential_cycle_backoff);
	BENCH2(spin_lock, const_relax_backoff);
	BENCH2(spin_lock, const_relax_x2_backoff);
	BENCH2(spin_lock, const_relax_x4_backoff);
	BENCH2(spin_lock, const_relax_x6_backoff);
	BENCH2(spin_lock, const_relax_x8_backoff);
	BENCH2(spin_lock, linear_relax_backoff);
	BENCH2(spin_lock, exponential_relax_backoff);
	BENCH2(spin_lock, yield_backoff);
	BENCH2(spin_lock, cycle_yield_backoff);
	BENCH2(spin_lock, relax_yield_backoff);

	BENCH2(tatas_lock, no_backoff);
	BENCH2(tatas_lock, const_cycle_backoff);
	BENCH2(tatas_lock, linear_cycle_backoff);
	BENCH2(tatas_lock, exponential_cycle_backoff);
	BENCH2(tatas_lock, const_relax_backoff);
	BENCH2(tatas_lock, const_relax_x2_backoff);
	BENCH2(tatas_lock, const_relax_x4_backoff);
	BENCH2(tatas_lock, const_relax_x6_backoff);
	BENCH2(tatas_lock, const_relax_x8_backoff);
	BENCH2(tatas_lock, linear_relax_backoff);
	BENCH2(tatas_lock, exponential_relax_backoff);
	BENCH2(tatas_lock, yield_backoff);
	BENCH2(tatas_lock, cycle_yield_backoff);
	BENCH2(tatas_lock, relax_yield_backoff);

	if (nthreads < hardware_nthreads || hardware_nthreads <= 8) {
		BENCH2(ticket_lock, no_backoff);
		BENCH2(ticket_lock, const_cycle_backoff);
		BENCH2(ticket_lock, linear_cycle_backoff);
		BENCH2(ticket_lock, exponential_cycle_backoff);
		BENCH2(ticket_lock, proportional_cycle_backoff);
		BENCH2(ticket_lock, const_relax_backoff);
		BENCH2(ticket_lock, const_relax_x2_backoff);
		BENCH2(ticket_lock, const_relax_x4_backoff);
		BENCH2(ticket_lock, const_relax_x6_backoff);
		BENCH2(ticket_lock, const_relax_x8_backoff);
		BENCH2(ticket_lock, linear_relax_backoff);
		BENCH2(ticket_lock, exponential_relax_backoff);
		BENCH2(ticket_lock, proportional_relax_backoff);
		BENCH2(ticket_lock, yield_backoff);
		BENCH2(ticket_lock, cycle_yield_backoff);
		BENCH2(ticket_lock, relax_yield_backoff);
	} else {
		BENCH2(ticket_lock, yield_backoff);
		BENCH2(ticket_lock, cycle_yield_backoff);
		BENCH2(ticket_lock, relax_yield_backoff);
	}

	std::cout << "\n";
}

int
main()
{
	unsigned n = std::thread::hardware_concurrency();
	for (unsigned i = 1; i <= n; i += std::min(i, 8u))
		bench(i, n);
	return 0;
}
