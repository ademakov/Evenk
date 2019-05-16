#include <evenk/synch.h>
#include <evenk/thread.h>

#include <iostream>

evenk::default_synch::lock_type lock;
evenk::default_synch::cond_var_type cond;

void
thread_routine()
{
	evenk::default_synch::lock_owner_type guard(lock);
	std::cout << "The created thread notifies the main thread.\n";
	cond.notify_one(); // notify that the thread started
	std::cout << "The created thread waits for a notification from the main thread.\n";
	cond.wait(guard);  // wait for test finish
	std::cout << "The created thread gets a notification and exits.\n";
}

void
print_affinity(const evenk::thread::cpuset_type &affinity)
{
	std::cout << "CPU affinity info ";
	if (affinity.size() == 0) {
		std::cout << "is not available";
	} else {
		std::cout << ':' << affinity.size() << " CPUs:";
		for (std::size_t cpu = 0; cpu < affinity.size(); cpu++)
			if (affinity[cpu])
				std::cout << " " << cpu;
	}
	std::cout << "\n";
}

int
main()
{
	evenk::default_synch::lock_owner_type guard(lock);
	std::cout << "The main thread creates a new thread and waits for a notification from it.\n";
	evenk::thread thread(thread_routine);
	cond.wait(guard); // wait until the thread starts
	std::cout << "The main thread gets a notification.\n";
	guard.unlock();

	{
		auto affinity = thread.affinity();
		print_affinity(affinity);

		for (std::size_t cpu = 0; cpu < affinity.size(); cpu += 2)
			affinity[cpu] = false;
		thread.affinity(affinity);
	}

	{
		auto affinity = thread.affinity();
		print_affinity(affinity);
	}

	guard.lock();
	std::cout << "The main thread notifies the created thread.\n";
	cond.notify_one(); // notify about test finish
	guard.unlock();

	thread.join();
	std::cout << "The main thread joins with the created thread and exits.\n";

	return 0;
}
