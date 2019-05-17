#define EVENK_SHARED_TICKET_TESTING 1

#include "evenk/spinlock.h"
#include "evenk/thread.h"

#include <iostream>

static constexpr std::size_t test_count = 10 * 1000 * 1000;

static constexpr std::size_t table_size = 8;

static constexpr std::size_t thread_num = 8;
static_assert(thread_num < 16, "for testing purposes shared_ticket_lock is intentionally restricted to at most 15 threads.");

struct entry
{
	alignas(64) std::int_fast32_t value;
} table[table_size];

evenk::shared_ticket_lock table_lock;

void
thread_routine(std::size_t thread_idx)
{
	for (std::size_t i = 1; i <= test_count; i++) {
		table_lock.lock_shared();
		const std::int_fast32_t value = table[0].value;
		for (std::size_t j = 1; j < table_size; j++) {
			if (value != table[j].value)
				return;
		}
		table_lock.unlock_shared();

		table_lock.lock();
		if ((i % 1000000) == 0)
			std::cout << "thread #" << thread_idx << " " << i << "\n";
		for (std::size_t j = 0; j < table_size; j++)
			table[j].value++;
		table_lock.unlock();
	}
}

int
main()
{
	std::size_t hw_threads = std::thread::hardware_concurrency();
	if (thread_num > hw_threads) {
		std::cout << "WARNING: the test runs extremely slow if the number of CPU cores is below " << thread_num
			  << " while your machine appears to have just " << hw_threads << ".\n";
	}

	evenk::thread thread_array[thread_num];
	for (std::size_t i = 0; i < thread_num; i++)
		thread_array[i] = evenk::thread(thread_routine, i);
	for (std::size_t i = 0; i < thread_num; i++)
		thread_array[i].join();

	for (std::size_t j = 0; j < table_size; j++) {
		if (table[j].value != test_count * thread_num) {
			std::cout << "FAILED\n";
			return 1;
		}
		std::cout << "entry #" << j << ": table[j].value=" << table[j].value << ": ok\n";
	}

	std::cout << "passed\n";
	return 0;
}
