#include <evenk/thread.h>
#include <evenk/synch.h>

#include <iostream>

evenk::default_synch::lock_type lock;
evenk::default_synch::cond_var_type cond;

void
test()
{
	evenk::default_synch::lock_owner_type guard(lock);
	cond.notify_one();
	cond.wait(guard);
}

int
main()
{
	evenk::default_synch::lock_owner_type guard(lock);
	evenk::thread thread(test);
	cond.wait(guard); // wait until the thread starts
	guard.unlock();

	auto affinity = thread.affinity();
	for (std::size_t cpu = 0; cpu < affinity.size(); cpu++)
		std::cout << cpu << " ";
	std::cout << "\n";

	guard.lock();
	cond.notify_one();
	guard.unlock();

	thread.join();

	return 0;
}
