#include <evenk/synch_queue.h>
#include <evenk/thread_pool.h>

template<typename T>
using queue = evenk::synch_queue<T>;

int
main()
{
	evenk::thread_pool<queue> pool(8);
	return 0;
}
