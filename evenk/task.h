//
// Executable Tasks
//
// Copyright (c) 2017  Aleksey Demakov
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "config.h"

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#ifndef EVENK_TASK_H_
#define EVENK_TASK_H_

//
// A task is a very restricted version of std::function used together with
// thread pools. It tries to reduce the need for dynamic memory allocation
// and synchronization as much as possible.
//
// This is achieved by relying on move-semantics while std::function relies
// on copy-semantics. As a task goes from a client thread to the task queue
// in a thread pool and from the queue to a worker thread at any given time
// there is only one task owner.
//
// Memory allocation should be avoided for move operations. So normally the
// memory allocator is only used for task construction and destruction. But
// small target types (function pointers or references to function objects)
// avoid memory allocation completely.
//
// A task itself is a callable object without arguments. It is non-copyable.
// However it is noexcept-movable.
//
// Also std::function implementations are usually optimized to efficiently
// store member function pointers so they reserve extra room for this data
// type. As tasks take no arguments it makes little sense to support member
// functions (at the very least they require a this-pointer argument).
//
// The task allocator might be polymorphic (pmr). In such a case it moves
// along with the task data.
//
// Note that as of C++17 standard the allocator support was removed from
// std::function and it was never implemented in the e.g. gcc libstdc++
// library. So in this respect tasks provide an extended feature.
//
// An even more restricted task variation is also available: trivial_task.
// This kind of tasks have a fixed maximum size and do not use allocators
// at all. Also trivial_task can only handle trivially-copyable types. On
// other hand they provide minimal overhead. In principle it is possible
// to use them with other types (non-trivial or exceeding the maximum size)
// if the function object is passed by reference. In this case the object
// in question must be kept alive separately until the task is in use.
//
// Examples:
//
//   void test() { ... }
//   void  testN(int n) { ... }
//   struct testBig {
//     char data[48] = {0};
//     void operator()() { ... }
//   };
//   ...
//   // A simple task usage
//   auto task1 = evenk::task<void>(test);
//   task1();
//
//   // A trivial task usage, has less overhead than the above
//   auto task2 = evenk::trivial_task<void>(test);
//   task2();
//
//   // A large task usage, looks identically but uses allocation internally
//   auto task3 = evenk::task<void>(testBig());
//   task3();
//
//   // A large trivial task will not compile as is
//   //auto task4 = evenk::trivial_task<void>(testBig());
//   //task4();
//
//   // It requires either increased internal storage
//   auto task5 = evenk::trivial_task<void, 48>(testBig());
//   task5();
//   // ...  or using a reference
//   testBig test6;
//   auto task6 = evenk::trivial_task<void>(std::ref(test6));
//   task6();
//
//   // The result of std::bind works smoothly with tasks
//   auto task7 = evenk::task<void>(std::bind(testN, 42));
//   task7();
//
//   // It is not trivially-copyable so a trivial_task will not compile
//   //auto task8 = evenk::trivial_task<void, 48>(std::bind(testN, 42));
//   //task8();
//   // ... but using a reference still works
//   auto test9 = std::bind(testN, 42);
//   auto task9 = evenk::trivial_task<int>(std::ref(test9));
//   task9();
//

namespace evenk {

static constexpr std::size_t fptr_size = sizeof(void (*)());
static constexpr std::size_t fptr_align = alignof(void (*)());

namespace detail {

template <typename F>
auto task_invoke(F &&f) -> decltype(std::forward<F>(f)());

template <typename V, typename F>
struct task_result_base
{
};

template <typename F>
struct task_result_base<decltype(void(task_invoke(std::declval<F>()))), F>
{
	using type = decltype(task_invoke(std::declval<F>()));
};

template <class F>
struct task_result : task_result_base<void, F>
{
};

template <bool>
struct task_adapter;

template <>
struct task_adapter<true>
{
	template <typename Target>
	Target *get(void *memory)
	{
		return static_cast<Target *>(memory);
	}

	template <typename Target, typename Alloc>
	void allocate(void *, Alloc &)
	{
	}

	template <typename Target, typename Alloc>
	void deallocate(void *, Alloc &)
	{
	}
};

template <>
struct task_adapter<false>
{
	template <typename Target>
	Target *get(void *memory)
	{
		void **ptrptr = static_cast<void **>(memory);
		return static_cast<Target *>(*ptrptr);
	}

	template <typename Target, typename Alloc>
	void allocate(void *memory, Alloc &alloc)
	{
		auto ptrptr = static_cast<char **>(memory);
		*ptrptr = alloc.allocate(sizeof(Target));
	}

	template <typename Target, typename Alloc>
	void deallocate(void *memory, Alloc &alloc)
	{
		auto ptrptr = static_cast<char **>(memory);
		alloc.deallocate(*ptrptr, sizeof(Target));
	}
};

} // namespace detail

template <typename R, std::size_t S = fptr_size>
class trivial_task
{
public:
	using result_type = R;

	static constexpr std::size_t memory_size = S;

	constexpr trivial_task() noexcept = default;
	constexpr trivial_task(std::nullptr_t) noexcept {}

	template <typename Callable>
	trivial_task(Callable &&callable)
	{
		using target_type = typename std::decay<Callable>::type;
		using target_result_type = typename detail::task_result<target_type>::type;
		static_assert(
			std::is_void<result_type>::value ||
				std::is_same<target_result_type, result_type>::value ||
				std::is_convertible<target_result_type, result_type>::value,
			"a trivial_task target result type mismatch");
		static_assert(std::is_trivially_copyable<target_type>::value,
			      "a trivial_task target is not trivially copyable");
		static_assert(sizeof(target_type) <= sizeof(memory_),
			      "a trivial_task target size limit is exceeded");

		new (&memory_) target_type(std::forward<Callable>(callable));
		invoke_ = &invoke<target_type>;
	}

	trivial_task(trivial_task &&other) noexcept
	{
		other.swap(*this);
	}

	trivial_task &operator=(trivial_task &&other) noexcept
	{
		trivial_task(std::move(other)).swap(*this);
		return *this;
	}

	void swap(trivial_task &other) noexcept
	{
		std::swap(invoke_, other.invoke_);
		std::swap(memory_, other.memory_);
	}

	result_type operator()()
	{
		if (invoke_ == nullptr)
			throw std::bad_function_call();
		return (*invoke_)(&memory_);
	}

	explicit operator bool() const noexcept
	{
		return invoke_ != nullptr;
	}

protected:
	using memory_type = char[memory_size];
	using invoke_type = result_type (*)(void *);

	memory_type memory_ = {0};
	invoke_type invoke_ = nullptr;

private:
	template <typename Target>
	static result_type invoke(void *memory)
	{
		return (*static_cast<Target *>(memory))();
	}
};

template <typename R, std::size_t S = fptr_size, typename A = std::allocator<char>>
class task : private trivial_task<R, std::max(S, fptr_size)>
{
	using base = trivial_task<R, S>;

public:
	using result_type = R;
	using allocator_type = A;

	using base::memory_size;

	constexpr task() noexcept = default;
	constexpr task(std::nullptr_t) noexcept {}

	template <typename Callable>
	task(Callable &&callable, const allocator_type &alloc = allocator_type())
		: wrapper_(alloc)
	{
		using target_type = typename std::decay<Callable>::type;
		using target_result_type = typename detail::task_result<target_type>::type;
		static_assert(
			std::is_void<result_type>::value ||
				std::is_same<target_result_type, result_type>::value ||
				std::is_convertible<target_result_type, result_type>::value,
			"a task target result type mismatch");

#if 0
		printf("sizeof(memory_) = %zu, sizeof(callable_type) = %zu, task size: %zu\n",
		       sizeof(base::memory_),
		       sizeof(target_type),
		       sizeof(*this));
#endif

		detail::task_adapter<sizeof(target_type) <= memory_size> adapter;
		adapter.template allocate<target_type>(base::memory_, wrapper_);
		new (adapter.template get<void>(base::memory_))
			target_type(std::forward<Callable>(callable));

		base::invoke_ = &invoke<target_type>;
		wrapper_.destroy_ = &destroy<target_type>;
	}

	~task() noexcept
	{
		if (base::operator bool())
			wrapper_.destroy_(base::memory_, wrapper_);
	}

	task(task &&other) noexcept
	{
		other.swap(*this);
	}

	task &operator=(task &&other) noexcept
	{
		task(std::move(other)).swap(*this);
		return *this;
	}

	void swap(task &other) noexcept
	{
		base::swap(other);
		std::swap(wrapper_, other.wrapper_);
	}

	using base::operator();
	using base::operator bool;

private:
	using destroy_type = void (*)(void *, allocator_type &);

	// Use empty-base optimization trick to store the allocator instance.
	struct data_destroy_wrapper : allocator_type
	{
		destroy_type destroy_ = nullptr;

		data_destroy_wrapper() = default;

		data_destroy_wrapper(const allocator_type &allacator)
			: allocator_type(allacator)
		{
		}
	};

	data_destroy_wrapper wrapper_;

	template <typename Target>
	static result_type invoke(void *memory)
	{
		detail::task_adapter<sizeof(Target) <= memory_size> adapter;
		return (*adapter.template get<Target>(memory))();
	}

	template <typename Target>
	static void destroy(void *memory, allocator_type &alloc)
	{
		detail::task_adapter<sizeof(Target) <= memory_size> adapter;
		adapter.template get<Target>(memory)->~Target();
		adapter.template deallocate<Target>(memory, alloc);
	}
};

} // namespace evenk

#endif // !EVENK_TASK_H_
