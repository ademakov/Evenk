#include "evenk/bounded_queue.h"
#include "evenk/synch_queue.h"

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
	while (queue.wait_pop(data, backoff...) == queue_op_status::success) {
		++count;
	}
}

template <typename Queue, typename... Backoff>
void
produce(Queue &queue, int count, Backoff... backoff)
{
	std::string data = "this is a test string";
	for (int i = 0; i < count; i++) {
		queue.push(data, backoff...);
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
	queue.close();

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

	synch_queue<std::string, std_synch> std_queue;
	synch_queue<std::string, posix_synch> posix_queue;

	BENCH1(std_queue);
	BENCH1(posix_queue);

#if __linux__
	{
		synch_queue<std::string, futex_synch> futex_queue;
		BENCH1(futex_queue);
	}
	{
		synch_queue<std::string, futex_synch> futex_queue;
		linear_backoff<cpu_cycle, 100000, 100> linear_cycle_backoff;
		BENCH2(futex_queue, linear_cycle_backoff);
	}
	{
		synch_queue<std::string, futex_synch> futex_queue;
		linear_backoff<cpu_relax, 1000, 1> linear_relax_backoff;
		BENCH2(futex_queue, linear_relax_backoff);
	}
	{
		synch_queue<std::string, futex_synch> futex_queue;
		yield_backoff yield_backoff;
		BENCH2(futex_queue, yield_backoff);
	}
#endif

	bounded_queue<std::string> a_bounded_queue(1024);
	BENCH1(a_bounded_queue);

	bounded_queue<std::string, bq_synch_slot<std_synch>> bounded_std_synch_queue(1024);
	BENCH1(bounded_std_synch_queue);

	{
		bounded_queue<std::string, bq_synch_slot<std_synch>> bounded_std_synch_queue(
			1024);
		linear_backoff<cpu_cycle, 100000, 100> linear_cycle_backoff;
		BENCH2(bounded_std_synch_queue, linear_cycle_backoff);
	}
	{
		bounded_queue<std::string, bq_synch_slot<std_synch>> bounded_std_synch_queue(
			1024);
		linear_backoff<cpu_relax, 1000, 1> linear_relax_backoff;
		BENCH2(bounded_std_synch_queue, linear_relax_backoff);
	}
	{
		bounded_queue<std::string, bq_synch_slot<std_synch>> bounded_std_synch_queue(
			1024);
		yield_backoff yield_backoff;
		BENCH2(bounded_std_synch_queue, yield_backoff);
	}

#if __linux__
	{
		bounded_queue<std::string, bq_synch_slot<futex_synch>>
			bounded_futex_synch_queue(1024);
		BENCH1(bounded_futex_synch_queue);
	}
	{
		bounded_queue<std::string, bq_synch_slot<futex_synch>>
			bounded_futex_synch_queue(1024);
		linear_backoff<cpu_cycle, 100000, 100> linear_cycle_backoff;
		BENCH2(bounded_futex_synch_queue, linear_cycle_backoff);
	}
	{
		bounded_queue<std::string, bq_synch_slot<futex_synch>>
			bounded_futex_synch_queue(1024);
		linear_backoff<cpu_relax, 1000, 1> linear_relax_backoff;
		BENCH2(bounded_futex_synch_queue, linear_relax_backoff);
	}
	{
		bounded_queue<std::string, bq_synch_slot<futex_synch>>
			bounded_futex_synch_queue(1024);
		yield_backoff yield_backoff;
		BENCH2(bounded_futex_synch_queue, yield_backoff);
	}
	{
		bounded_queue<std::string, bq_futex_slot> bounded_futex_queue(1024);
		BENCH1(bounded_futex_queue);
	}
	{
		bounded_queue<std::string, bq_futex_slot> bounded_futex_queue(1024);
		linear_backoff<cpu_cycle, 100000, 100> linear_cycle_backoff;
		BENCH2(bounded_futex_queue, linear_cycle_backoff);
	}
	{
		bounded_queue<std::string, bq_futex_slot> bounded_futex_queue(1024);
		linear_backoff<cpu_relax, 1000, 1> linear_relax_backoff;
		BENCH2(bounded_futex_queue, linear_relax_backoff);
	}
	{
		bounded_queue<std::string, bq_futex_slot> bounded_futex_queue(1024);
		yield_backoff yield_backoff;
		BENCH2(bounded_futex_queue, yield_backoff);
	}
#endif

	bounded_queue<std::string, bq_yield_slot> bounded_yield_queue(1024);
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
