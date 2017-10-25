#include <evenk/synch_queue.h>
#include <evenk/thread_pool.h>

template <typename T>
using queue = evenk::synch_queue<T>;

int
main()
{
	static constexpr std::uint32_t expected = 100 * 1000;
	std::atomic<std::uint32_t> counter = ATOMIC_VAR_INIT(0);

	evenk::thread_pool<queue> pool(8);
	for (std::uint32_t i = 0; i < expected; i++)
		pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
	pool.wait();

	std::uint32_t actual = counter.load(std::memory_order_relaxed);
	printf("%u %s\n", actual, actual == expected ? "Okay" : "FAIL");

	return 0;
}
