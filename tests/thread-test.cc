#include <evenk/synch.h>
#include <evenk/thread.h>

#include <iostream>

evenk::default_synch::lock_type lock;
evenk::default_synch::cond_var_type cond;

void
thread_routine()
{
	evenk::default_synch::lock_owner_type guard(lock);
	cond.notify_one(); // notify that the thread started
	cond.wait(guard);  // wait for test finish
}

void
print_affinity(const evenk::thread::cpuset_type &affinity)
{
	for (std::size_t cpu = 0; cpu < affinity.size(); cpu++)
		std::cout << cpu << " ";
	std::cout << "\n";
}

int
main()
{
	evenk::default_synch::lock_owner_type guard(lock);
	evenk::thread thread(thread_routine);
	cond.wait(guard); // wait until the thread starts
	guard.unlock();

	{
		auto affinity = thread.affinity();
		print_affinity(affinity);

		for (std::size_t i = 0; i < affinity.size(); i++)
			affinity[i] = false;
		thread.affinity(affinity);
	}

	{
		auto affinity = thread.affinity();
		print_affinity(affinity);
	}

	guard.lock();
	cond.notify_one(); // notify about test finish
	guard.unlock();

	thread.join();

	return 0;
}
