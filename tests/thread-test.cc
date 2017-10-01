#include <evenk/thread.h>

#include <iostream>

int
main()
{
	evenk::thread thread;

	auto affinity = thread.affinity();
	for (std::size_t cpu = 0; cpu < affinity.size(); cpu++)
		std::cout << cpu << " ";
	std::cout << "\n";

	return 0;
}
