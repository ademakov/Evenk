#include <evenk/task.h>

#include <cstdio>
#include <experimental/memory_resource>

#include <signal.h>
#include <setjmp.h>

int
test()
{
    printf("%s\n", __PRETTY_FUNCTION__);
    return 42;
}

int
testN(int n)
{
    printf("%s\n", __PRETTY_FUNCTION__);
    return n;
}

struct Test
{
    void operator()() const
    {
	printf("%s\n", __PRETTY_FUNCTION__);
    }
};

struct Test24
{
    Test24() = default;

    Test24(Test24 &&other) = default;
    Test24 &operator=(Test24 &&other) = default;

    Test24(const Test24 &other) = delete;
    Test24 &operator=(const Test24 &other) = delete;

    void operator()() const
    {
	printf("%s\n", __PRETTY_FUNCTION__);
    }

    char dummy[24] = {0};
};

struct Test48
{
    Test48() = default;

    Test48(Test48 &&other) = default;
    Test48 &operator=(Test48 &&other) = default;

    void operator()() const
    {
	printf("%s\n", __PRETTY_FUNCTION__);
    }

    char dummy[48] = {0};
};

struct TestD
{
    bool gone = false;

    TestD() {}

    TestD(TestD &&other) {
	gone = other.gone;
	other.gone = true;
    }

    ~TestD()
    {
	if (!gone)
	    printf("%s\n", __PRETTY_FUNCTION__);
    }

    void operator()() const
    {
	printf("%s\n", __PRETTY_FUNCTION__);
    }
};

jmp_buf jmp_target;
void signal_handler(int sig)
{
	longjmp(jmp_target, sig);
}

int
main()
{
	printf("trivial_task tests\n"
	       "==================\n\n");

	{
		auto task = evenk::trivial_task<int>(test);
		auto result = task();
		printf("function result: %d\n", result);
	}
	printf("\n");

	{
		auto func = std::bind(testN, 42);
		auto task = evenk::trivial_task<int>(std::ref(func));
		auto result = task();
		printf("binding result: %d\n", result);
	}
	printf("\n");

	{
		auto task = evenk::trivial_task<int>([] {
			printf("%s\n", __PRETTY_FUNCTION__);
			return 42;
		});
		auto result = task();
		printf("lambda result: %d\n", result);
	}
	printf("\n");

	{
		auto task = evenk::trivial_task<void>(Test());
		task();
	}
	printf("\n");

	{
		auto task = evenk::trivial_task<void, 24>(Test24());
		task();
	}
	printf("\n");

	{
		auto task = evenk::trivial_task<void, 48>(Test48());
		task();
	}
	printf("\n");

	{
		Test48 test;
		auto task = evenk::trivial_task<void>(std::ref(test));
		task();
	}
	printf("\n");

	printf("task tests\n"
	       "==========\n\n");

	{
		auto task = evenk::task<int>(test);
		auto result = task();
		printf("function result: %d\n", result);
	}
	printf("\n");

	{
		auto task = evenk::task<int>(std::bind(testN, 42));
		auto result = task();
		printf("binding result: %d\n", result);
	}
	printf("\n");

	{
		auto task = evenk::task<int, 8, std::experimental::pmr::polymorphic_allocator<char>>(std::bind(testN, 42));
		auto result = task();
		printf("pmr binding result: %d\n", result);
	}
	printf("\n");

	{
		auto task = evenk::task<void>(Test48());
		task();
	}
	printf("\n");

	{
		TestD test;
		auto task = evenk::task<void>(std::move(test));
		task();
	}
	printf("\n");

	printf("default ctor tests\n"
	       "==================\n\n");

	{
		auto task = evenk::trivial_task<void>();
		printf("default trivial_task is callable: %d\n", bool(task));

		auto saved_handler = signal(SIGSEGV, signal_handler);
		auto jmp_result = setjmp(jmp_target);
		if (jmp_result) {
			printf("signal %d caught on its call (as expected)\n", jmp_result);
		} else {
			task();
		}
		signal(SIGSEGV, saved_handler);
	}
	printf("\n");

	{
		auto task = evenk::task<void>();
		printf("default task is callable: %d\n", bool(task));

		try {
			task();
		} catch (std::bad_function_call&) {
			printf("exception caught on its call (as expected)\n");
		}
	}
	printf("\n");

	return 0;
}

