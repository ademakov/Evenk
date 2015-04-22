#include "evenk/bounded_queue.h"
#include "evenk/concurrency.h"

#include <iostream>
#include <chrono>
#include <string>
#include <cstring>
#include <vector>

namespace evc = ev::concurrency;

template <typename Queue, typename... Backoff>
void consume(Queue& queue, size_t& count, Backoff... backoff) {
  std::string data;
  while (queue.Dequeue(data, backoff...)) {
    ++count;
  }
}

template <typename Queue, typename... Backoff>
void produce(Queue& queue, int count, Backoff... backoff) {
  std::string data = "this is a test string";
  for (int i = 0; i < count; i++) {
    queue.Enqueue(data, backoff...);
  }
}

template <typename Queue, typename... Backoff>
void bench(unsigned nthreads, const std::string& name, Queue& queue,
           Backoff... backoff) {
  std::vector<size_t> counts(nthreads);
  std::vector<std::thread> threads(nthreads);

  auto start = std::chrono::steady_clock::now();

  for (size_t i = 0; i < threads.size(); i++)
    threads[i] = std::thread(consume<Queue, Backoff...>, std::ref(queue),
                             std::ref(counts[i]), backoff...);

#define TOTAL (250 * 1000)

  produce(queue, TOTAL, backoff...);
  queue.Finish();

  for (auto& t : threads) t.join();

  auto end = std::chrono::steady_clock::now();
  std::chrono::duration<double> diff = end - start;

  size_t total = 0;
  for (auto& c : counts) total += c;

  std::cout << name << ": duration=" << diff.count() << ", count=" << total
            << "\n";
  if (total != TOTAL) std::cout << "FAIL!!!\n";

  for (auto& c : counts) std::cout << " " << c;
  std::cout << '\n';
}

void bench(unsigned nthreads) {
  std::cout << "Threads: " << nthreads << "\n";

#define BENCH1(queue) bench(nthreads, #queue, queue)
#define BENCH2(queue, backoff) \
  bench(nthreads, #queue " " #backoff, queue, backoff)

  evc::Queue<std::string, evc::StdSynch> std_queue;
  evc::Queue<std::string, evc::PosixSynch> posix_queue;

  BENCH1(std_queue);
  BENCH1(posix_queue);

#if __linux__
  {
    evc::Queue<std::string, evc::FutexSynch> futex_queue;
    BENCH1(futex_queue);
  }
  {
    evc::Queue<std::string, evc::FutexSynch> futex_queue;
    evc::LinearBackoff<evc::CPUCycle> linear_cycle_backoff(100000);
    BENCH2(futex_queue, linear_cycle_backoff);
  }
  {
    evc::Queue<std::string, evc::FutexSynch> futex_queue;
    evc::LinearBackoff<evc::CPURelax> linear_relax_backoff(100000);
    BENCH2(futex_queue, linear_relax_backoff);
  }
  {
    evc::Queue<std::string, evc::FutexSynch> futex_queue;
    evc::YieldBackoff yield_backoff;
    BENCH2(futex_queue, yield_backoff);
  }
#endif

  evc::BoundedQueue<std::string> bounded_queue(1024);
  BENCH1(bounded_queue);

  evc::BoundedQueue<std::string, evc::BoundedQueueSynchWait<evc::StdSynch>>
      bounded_std_synch_queue(1024);
  BENCH1(bounded_std_synch_queue);

  {
    evc::BoundedQueue<std::string, evc::BoundedQueueSynchWait<evc::StdSynch>>
        bounded_std_synch_queue(1024);
    evc::LinearBackoff<evc::CPUCycle> linear_cycle_backoff(100000);
    BENCH2(bounded_std_synch_queue, linear_cycle_backoff);
  }
  {
    evc::BoundedQueue<std::string, evc::BoundedQueueSynchWait<evc::StdSynch>>
        bounded_std_synch_queue(1024);
    evc::LinearBackoff<evc::CPURelax> linear_relax_backoff(100000);
    BENCH2(bounded_std_synch_queue, linear_relax_backoff);
  }
  {
    evc::BoundedQueue<std::string, evc::BoundedQueueSynchWait<evc::StdSynch>>
        bounded_std_synch_queue(1024);
    evc::YieldBackoff yield_backoff;
    BENCH2(bounded_std_synch_queue, yield_backoff);
  }

#if __linux__
  {
    evc::BoundedQueue<std::string, evc::BoundedQueueSynchWait<evc::FutexSynch>>
        bounded_futex_synch_queue(1024);
    BENCH1(bounded_futex_synch_queue);
  }
  {
    evc::BoundedQueue<std::string, evc::BoundedQueueSynchWait<evc::FutexSynch>>
        bounded_futex_synch_queue(1024);
    evc::LinearBackoff<evc::CPUCycle> linear_cycle_backoff(100000);
    BENCH2(bounded_futex_synch_queue, linear_cycle_backoff);
  }
  {
    evc::BoundedQueue<std::string, evc::BoundedQueueSynchWait<evc::FutexSynch>>
        bounded_futex_synch_queue(1024);
    evc::LinearBackoff<evc::CPURelax> linear_relax_backoff(100000);
    BENCH2(bounded_futex_synch_queue, linear_relax_backoff);
  }
  {
    evc::BoundedQueue<std::string, evc::BoundedQueueSynchWait<evc::FutexSynch>>
        bounded_futex_synch_queue(1024);
    evc::YieldBackoff yield_backoff;
    BENCH2(bounded_futex_synch_queue, yield_backoff);
  }
  {
    evc::BoundedQueue<std::string, evc::BoundedQueueFutexWait>
        bounded_futex_queue(1024);
    BENCH1(bounded_futex_queue);
  }
  {
    evc::BoundedQueue<std::string, evc::BoundedQueueFutexWait>
        bounded_futex_queue(1024);
    evc::LinearBackoff<evc::CPUCycle> linear_cycle_backoff(100000);
    BENCH2(bounded_futex_queue, linear_cycle_backoff);
  }
  {
    evc::BoundedQueue<std::string, evc::BoundedQueueFutexWait>
        bounded_futex_queue(1024);
    evc::LinearBackoff<evc::CPURelax> linear_relax_backoff(100000);
    BENCH2(bounded_futex_queue, linear_relax_backoff);
  }
  {
    evc::BoundedQueue<std::string, evc::BoundedQueueFutexWait>
        bounded_futex_queue(1024);
    evc::YieldBackoff yield_backoff;
    BENCH2(bounded_futex_queue, yield_backoff);
  }
#endif

  evc::BoundedQueue<std::string, evc::BoundedQueueYieldWait>
      bounded_yield_queue(1024);
  BENCH1(bounded_yield_queue);

  std::cout << "\n";
}

int main() {
  unsigned n = std::thread::hardware_concurrency();
  for (unsigned i = 1; i <= n; i += i) bench(i);
  return 0;
}
