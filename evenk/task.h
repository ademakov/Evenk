//
// Executable Tasks
//
// Copyright (c) 2017-2018  Aleksey Demakov
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
// This is achieved by enforcing move-semantics while std::function may use
// copy-semantics. At any given time, as a task goes from a client thread to
// a thread pool's task queue and then to a worker thread, there is only one
// current owner of the task.
//
// Memory allocation should be avoided for task move operations. There is no
// memory allocation at all for small trivially-copyable target types (such
// as function pointers, references to function objects, etc). And for other
// target types memory should only be allocated when the task is constructed
// from a target callable object and deallocated when the task is destructed.
//
// A task itself is a callable object without arguments. It is non-copyable.
// However it is noexcept-movable.
//
// Also std::function implementations are usually optimized to efficiently
// store member function pointers so they reserve fixed-size extra room for
// this data type. For larger objects they have to use dynamically allocated
// memory. But for tasks it is possible to specify the reserved memory size.
// So it is possible to avoid allocation for larger objects. But by default
// the reserved memory size is even smaller than for std::function. It is
// just enough for a non-member function pointer or a reference to a more
// complex type.
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
// Also before invocation a trivial_task requires a manual check of its
// validity. Invoking invalid trivial_task leads to invalid memory access.
// In contrast to this a task (and std::function) automatically handles
// such cases by gently throwing std::bad_function_call().
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

// A utility to adjust the size of reserved memory for tasks.
static constexpr std::size_t
task_memory_size(std::size_t size)
{
	static_assert((fptr_align & (fptr_align - 1)) == 0,
		      "function pointer alignment is not a power of two");
	size = (size + fptr_align - 1) & ~(fptr_align - 1);
	return std::max(size, fptr_size);
}

//
// Some template meta-programming stuff that reproduces C++11 std::result_of
// or C++17 std::invoke_result in a compiler independent way. It is relatively
// easy to do here as there is no need to support any arguments.
//

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

template <typename F>
struct task_result : task_result_base<void, F>
{
};

//
// Helpers to manage different kinds of tasks uniformly.
//

template <typename Target, typename Alloc>
struct task_adapter_small
{
	Target *get(void *memory)
	{
		return static_cast<Target *>(memory);
	}

	void move(void *memory, void *other_memory)
	{
		auto ptr = static_cast<Target *>(memory);
		auto other_ptr = static_cast<Target *>(other_memory);
		new (ptr) Target(std::move(*other_ptr));
		other_ptr->~Target();
	}

	void allocate(void *, Alloc &) {}

	void deallocate(void *, Alloc &) {}
};

template <typename Target, typename Alloc>
struct task_adapter_large
{
	Target *get(void *memory)
	{
		void **ptrptr = static_cast<void **>(memory);
		return static_cast<Target *>(*ptrptr);
	}

	void move(void *memory, void *other_memory)
	{
		void **ptrptr = static_cast<void **>(memory);
		void **other_ptrptr = static_cast<void **>(other_memory);
		*ptrptr = *other_ptrptr;
		*other_ptrptr = nullptr;
	}

	void allocate(void *memory, Alloc &alloc)
	{
		auto ptrptr = static_cast<char **>(memory);
		*ptrptr = alloc.allocate(sizeof(Target));
	}

	void deallocate(void *memory, Alloc &alloc)
	{
		auto ptrptr = static_cast<char **>(memory);
		alloc.deallocate(*ptrptr, sizeof(Target));
	}
};

template <typename T, std::size_t S, typename A>
using task_adapter = typename std::
	conditional<sizeof(T) <= S, task_adapter_small<T, A>, task_adapter_large<T, A>>::type;

} // namespace detail

template <typename R, std::size_t S = fptr_size>
class trivial_task
{
public:
	using result_type = R;

	static constexpr std::size_t memory_size = detail::task_memory_size(S);

	constexpr trivial_task() noexcept = default;
	constexpr trivial_task(std::nullptr_t) noexcept {}

	template <typename Callable>
	trivial_task(Callable &&callable)
		: invoke_(&invoke<typename std::decay<Callable>::type>)
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
		static_assert(std::is_trivially_destructible<target_type>::value,
			      "a trivial_task target is not trivially destructible");
		static_assert(sizeof(target_type) <= sizeof(memory_),
			      "a trivial_task target size limit is exceeded");

		new (&memory_) target_type(std::forward<Callable>(callable));
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

	explicit operator bool() const noexcept
	{
		return invoke_ != nullptr;
	}

	result_type operator()()
	{
		return (*invoke_)(&memory_);
	}

protected:
	using memory_type = char[memory_size];
	using invoke_type = result_type (*)(void *);

	memory_type memory_ = {0};
	invoke_type invoke_ = nullptr;

	constexpr trivial_task(std::nullptr_t, invoke_type invoke) noexcept : invoke_(invoke) {}

private:
	template <typename Target>
	static result_type invoke(void *memory)
	{
		return (*static_cast<Target *>(memory))();
	}
};

template <typename R, std::size_t S = fptr_size, typename A = std::allocator<char>>
class task : private trivial_task<R, S>
{
	using base = trivial_task<R, S>;

public:
	using result_type = R;
	using allocator_type = A;

	using base::memory_size;

	constexpr task() noexcept : base(nullptr, invalid_invoke) {}
	constexpr task(std::nullptr_t) noexcept : base(nullptr, invalid_invoke) {}

	template <typename Callable>
	task(Callable &&callable, const allocator_type &alloc = allocator_type())
		: base(nullptr, &invoke<typename std::decay<Callable>::type>), wrapper_(alloc)
	{
		using target_type = typename std::decay<Callable>::type;
		using target_result_type = typename detail::task_result<target_type>::type;
		static_assert(
			std::is_void<result_type>::value ||
				std::is_same<target_result_type, result_type>::value ||
				std::is_convertible<target_result_type, result_type>::value,
			"a task target result type mismatch");

#if EVENK_TASK_DEBUG
		printf("sizeof(memory_) = %zu, sizeof(callable_type) = %zu, task size: %zu\n",
		       sizeof(base::memory_),
		       sizeof(target_type),
		       sizeof(*this));
#endif

		detail::task_adapter<target_type, memory_size, allocator_type> adapter;
		adapter.allocate(base::memory_, wrapper_);
		new (adapter.get(base::memory_)) target_type(std::forward<Callable>(callable));

		wrapper_.helper_ = &helper<target_type>;
	}

	~task() noexcept
	{
		if (operator bool())
			wrapper_.helper_(this, nullptr);
	}

	task(task &&other) noexcept : base(nullptr, other.invoke_)
	{
		other.wrapper_.helper_(this, &other);
		std::swap(other.wrapper_.helper_, wrapper_.helper_);
		other.invoke_ = invalid_invoke;
	}

	task &operator=(task &&other) noexcept
	{
		task(std::move(other)).swap(*this);
		return *this;
	}

	void swap(task &other) noexcept
	{
		std::swap(*this, other);
	}

	explicit operator bool() const noexcept
	{
		return wrapper_.helper_ != nullptr;
	}

	using base::operator();

private:
	using helper_type = void (*)(task *, task *);

	// Use empty-base optimization trick to store the allocator instance.
	struct helper_wrapper : allocator_type
	{
		helper_type helper_ = nullptr;

		helper_wrapper() = default;

		helper_wrapper(const allocator_type &allocator)
			: allocator_type(allocator)
		{
		}
	};

	helper_wrapper wrapper_;

	static result_type invalid_invoke(void *)
	{
		throw std::bad_function_call();
	}

	template <typename Target>
	static result_type invoke(void *memory)
	{
		detail::task_adapter<Target, memory_size, allocator_type> adapter;
		return (*adapter.get(memory))();
	}

	template <typename Target>
	static void helper(task *this_task, task *other_task)
	{
		detail::task_adapter<Target, memory_size, allocator_type> adapter;
		if (other_task != nullptr) {
			adapter.move(this_task->memory_, other_task->memory_);
		} else {
			adapter.get(this_task->memory_)->~Target();
			adapter.deallocate(this_task->memory_, this_task->wrapper_);
		}
	}
};

} // namespace evenk

#endif // !EVENK_TASK_H_
