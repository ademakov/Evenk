#include "evenk/spinlock.h"
#include "evenk/concurrency.h"

#include <iostream>
#include <chrono>
#include <string>
#include <vector>

namespace evc = ev::concurrency;

evc::StdMutex mutex;
evc::PosixMutex posix_mutex;
evc::SpinLock spin_lock;
evc::TicketLock ticket_lock;
evc::FutexLock futex_lock;

evc::NoBackoff no_backoff;
evc::YieldBackoff yield_backoff;

evc::LinearBackoff<evc::CPUCycle> linear_cycle_backoff(40);
evc::ExponentialBackoff<evc::CPUCycle> exponential_cycle_backoff(40);
evc::ProportionalBackoff<evc::CPUCycle> proportional_cycle_backoff(40);

evc::LinearBackoff<evc::CPURelax> linear_relax_backoff(5);
evc::ExponentialBackoff<evc::CPURelax> exponential_relax_backoff(5);
evc::ProportionalBackoff<evc::CPURelax> proportional_relax_backoff(5);

evc::LinearBackoff<evc::NanoSleep> linear_sleep_backoff(10);
evc::ExponentialBackoff<evc::NanoSleep> exponential_sleep_backoff(10);
evc::ProportionalBackoff<evc::NanoSleep> proportional_sleep_backoff(10);

evc::CompositeBackoff<evc::LinearBackoff<evc::CPUCycle>, evc::YieldBackoff>
    cycle_yield_backoff(linear_cycle_backoff, yield_backoff);
evc::CompositeBackoff<evc::LinearBackoff<evc::CPURelax>, evc::YieldBackoff>
    relax_yield_backoff(linear_relax_backoff, yield_backoff);

template <typename Lock, typename... Backoff>
void spin(int &count, Lock &lock, Backoff... backoff) {
  for (int i = 0; i < 100 * 1000; ++i) {
    lock.Lock(backoff...);
    evc::CPUCycle{}(1000);
    ++count;
    lock.Unlock();
    evc::CPUCycle{}(5000);
  }
}

template <typename Lock, typename... Backoff>
void bench(unsigned nthreads, std::string const &name, Lock &lock,
           Backoff... backoff) {
  int count = 0;

  std::vector<std::thread> v;
  v.reserve(nthreads);

  auto start = std::chrono::steady_clock::now();

  for (unsigned i = 0; i < nthreads; ++i)
    v.emplace_back(spin<Lock, Backoff...>, std::ref(count), std::ref(lock),
                   backoff...);
  for (auto &t : v)
    t.join();

  auto end = std::chrono::steady_clock::now();
  std::chrono::duration<double> diff = end - start;

  std::cout << name << ": count=" << count << ", duration=" << diff.count()
            << "\n";
}

void bench(unsigned nthreads) {
  std::cout << "Threads: " << nthreads << "\n";

#define BENCH1(lock) bench(nthreads, #lock, lock)
#define BENCH2(lock, backoff) bench(nthreads, #lock " " #backoff, lock, backoff)

  BENCH1(mutex);
  BENCH1(posix_mutex);

#if __linux__
  BENCH2(futex_lock, no_backoff);
#endif

#if __linux__
  BENCH2(futex_lock, linear_cycle_backoff);
  BENCH2(futex_lock, exponential_cycle_backoff);
  BENCH2(futex_lock, proportional_cycle_backoff);
  BENCH2(futex_lock, linear_relax_backoff);
  BENCH2(futex_lock, exponential_relax_backoff);
  BENCH2(futex_lock, proportional_relax_backoff);
#endif

  BENCH2(spin_lock, no_backoff);
  BENCH2(spin_lock, linear_cycle_backoff);
  BENCH2(spin_lock, exponential_cycle_backoff);
  BENCH2(spin_lock, proportional_cycle_backoff);
  BENCH2(spin_lock, linear_relax_backoff);
  BENCH2(spin_lock, exponential_relax_backoff);
  BENCH2(spin_lock, proportional_relax_backoff);
  BENCH2(spin_lock, yield_backoff);
  BENCH2(spin_lock, cycle_yield_backoff);
  BENCH2(spin_lock, relax_yield_backoff);
  BENCH2(spin_lock, linear_sleep_backoff);
  BENCH2(spin_lock, exponential_sleep_backoff);
  BENCH2(spin_lock, proportional_sleep_backoff);

  BENCH2(ticket_lock, no_backoff);
  BENCH2(ticket_lock, linear_cycle_backoff);
  BENCH2(ticket_lock, exponential_cycle_backoff);
  BENCH2(ticket_lock, proportional_cycle_backoff);
  BENCH2(ticket_lock, linear_relax_backoff);
  BENCH2(ticket_lock, exponential_relax_backoff);
  BENCH2(ticket_lock, proportional_relax_backoff);
  BENCH2(ticket_lock, yield_backoff);
  BENCH2(ticket_lock, cycle_yield_backoff);
  BENCH2(ticket_lock, relax_yield_backoff);
  // BENCH2(ticket_lock, linear_sleep_backoff);
  // BENCH2(ticket_lock, exponential_sleep_backoff);
  // BENCH2(ticket_lock, proportional_sleep_backoff);

  std::cout << "\n";
}

int main() {
  unsigned n = std::thread::hardware_concurrency();
  for (unsigned i = 1; i <= n; i += i)
    bench(i);
  return 0;
}
