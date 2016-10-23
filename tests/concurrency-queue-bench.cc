#include "evenk/bounded_queue.h"
#include "evenk/queue.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace evenk;

template <typename Queue, typename... Backoff>
void
consume(Queue &queue, size_t &count, Backoff... backoff)
{
	std::string data;
	while (queue.dequeue(data, backoff...)) {
		++count;
	}
}

template <typename Queue, typename... Backoff>
void
produce(Queue &queue, int count, Backoff... backoff)
{
	std::string data = "this is a test string";
	for (int i = 0; i < count; i++) {
		queue.enqueue(data, backoff...);
	}
}

template <typename Queue, typename... Backoff>
void
bench(unsigned nthreads, const std::string &name, Queue &queue, Backoff... backoff)
{
	std::vector<size_t> counts(nthreads);
	std::vector<std::thread> threads(nthreads);

	auto start = std::chrono::steady_clock::now();

	for (size_t i = 0; i < threads.size(); i++)
		threads[i] = std::thread(consume<Queue, Backoff...>,
					 std::ref(queue),
					 std::ref(counts[i]),
					 backoff...);

#define TOTAL (250 * 1000)

	produce(queue, TOTAL, backoff...);
	queue.finish();

	for (auto &t : threads)
		t.join();

	auto end = std::chrono::steady_clock::now();
	std::chrono::duration<double> diff = end - start;

	size_t total = 0;
	for (auto &c : counts)
		total += c;

	std::cout << name << ": duration=" << diff.count() << ", count=" << total << "\n";
	if (total != TOTAL)
		std::cout << "FAIL!!!\n";

	for (auto &c : counts)
		std::cout << " " << c;
	std::cout << '\n';
}

void
bench(unsigned nthreads)
{
	std::cout << "Threads: " << nthreads << "\n";

#define BENCH1(queue) bench(nthreads, #queue, queue)
#define BENCH2(queue, backoff) bench(nthreads, #queue " " #backoff, queue, backoff)

	queue<std::string, StdSynch> std_queue;
	queue<std::string, PosixSynch> posix_queue;

	BENCH1(std_queue);
	BENCH1(posix_queue);

#if __linux__
	{
		queue<std::string, FutexSynch> futex_queue;
		BENCH1(futex_queue);
	}
	{
		queue<std::string, FutexSynch> futex_queue;
		LinearBackoff<CPUCycle> linear_cycle_backoff(100000);
		BENCH2(futex_queue, linear_cycle_backoff);
	}
	{
		queue<std::string, FutexSynch> futex_queue;
		LinearBackoff<CPURelax> linear_relax_backoff(100000);
		BENCH2(futex_queue, linear_relax_backoff);
	}
	{
		queue<std::string, FutexSynch> futex_queue;
		YieldBackoff yield_backoff;
		BENCH2(futex_queue, yield_backoff);
	}
#endif

	bounded_queue<std::string> a_bounded_queue(1024);
	BENCH1(a_bounded_queue);

	bounded_queue<std::string, bounded_queue_synch<StdSynch>> bounded_std_synch_queue(1024);
	BENCH1(bounded_std_synch_queue);

	{
		bounded_queue<std::string, bounded_queue_synch<StdSynch>>
			bounded_std_synch_queue(1024);
		LinearBackoff<CPUCycle> linear_cycle_backoff(100000);
		BENCH2(bounded_std_synch_queue, linear_cycle_backoff);
	}
	{
		bounded_queue<std::string, bounded_queue_synch<StdSynch>>
			bounded_std_synch_queue(1024);
		LinearBackoff<CPURelax> linear_relax_backoff(100000);
		BENCH2(bounded_std_synch_queue, linear_relax_backoff);
	}
	{
		bounded_queue<std::string, bounded_queue_synch<StdSynch>>
			bounded_std_synch_queue(1024);
		YieldBackoff yield_backoff;
		BENCH2(bounded_std_synch_queue, yield_backoff);
	}

#if __linux__
	{
		bounded_queue<std::string, bounded_queue_synch<FutexSynch>>
			bounded_futex_synch_queue(1024);
		BENCH1(bounded_futex_synch_queue);
	}
	{
		bounded_queue<std::string, bounded_queue_synch<FutexSynch>>
			bounded_futex_synch_queue(1024);
		LinearBackoff<CPUCycle> linear_cycle_backoff(100000);
		BENCH2(bounded_futex_synch_queue, linear_cycle_backoff);
	}
	{
		bounded_queue<std::string, bounded_queue_synch<FutexSynch>>
			bounded_futex_synch_queue(1024);
		LinearBackoff<CPURelax> linear_relax_backoff(100000);
		BENCH2(bounded_futex_synch_queue, linear_relax_backoff);
	}
	{
		bounded_queue<std::string, bounded_queue_synch<FutexSynch>>
			bounded_futex_synch_queue(1024);
		YieldBackoff yield_backoff;
		BENCH2(bounded_futex_synch_queue, yield_backoff);
	}
	{
		bounded_queue<std::string, bounded_queue_futex> bounded_futex_queue(1024);
		BENCH1(bounded_futex_queue);
	}
	{
		bounded_queue<std::string, bounded_queue_futex> bounded_futex_queue(1024);
		LinearBackoff<CPUCycle> linear_cycle_backoff(100000);
		BENCH2(bounded_futex_queue, linear_cycle_backoff);
	}
	{
		bounded_queue<std::string, bounded_queue_futex> bounded_futex_queue(1024);
		LinearBackoff<CPURelax> linear_relax_backoff(100000);
		BENCH2(bounded_futex_queue, linear_relax_backoff);
	}
	{
		bounded_queue<std::string, bounded_queue_futex> bounded_futex_queue(1024);
		YieldBackoff yield_backoff;
		BENCH2(bounded_futex_queue, yield_backoff);
	}
#endif

	bounded_queue<std::string, bounded_queue_yield> bounded_yield_queue(1024);
	BENCH1(bounded_yield_queue);

	std::cout << "\n";
}

int
main()
{
	unsigned n = std::thread::hardware_concurrency();
	for (unsigned i = 1; i <= n; i += i)
		bench(i);
	return 0;
}
