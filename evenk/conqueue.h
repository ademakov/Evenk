//
// Concurrent Queue Basics and Utilities
//
// Copyright (c) 2016-2017  Aleksey Demakov
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

#ifndef EVENK_CONQUEUE_H_
#define EVENK_CONQUEUE_H_

//
// The code in this file is based on the following proposals:
//    http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2016/p0260r0.html
//    http://www.open-std.org/JTC1/SC22/WG21/docs/papers/2017/p0260r1.html
//
// The second one removes nonblocking methods but it is not clear if this
// a permanent or temporary decision.
//

#include <iterator>
#include <utility>

#define ENABLE_QUEUE_NONBLOCKING_OPS 1

namespace evenk {

#if ENABLE_QUEUE_NONBLOCKING_OPS
enum class queue_op_status { success = 0, empty, full, closed, busy };
#else
enum class queue_op_status { success = 0, empty, full, closed };
#endif

template <typename Value>
class queue_base
{
public:
	using value_type = Value;
	using reference = value_type &;
	using const_reference = const value_type &;

	virtual ~queue_base() noexcept {};

	// State operations
	virtual void close() noexcept = 0;
	virtual bool is_closed() const noexcept = 0;
	virtual bool is_empty() const noexcept = 0;
	virtual bool is_full() const noexcept = 0;
	virtual bool is_lock_free() const noexcept = 0;

	// Basic operations
	virtual void push(const value_type &) = 0;
	virtual void push(value_type &&) = 0;
	virtual value_type value_pop() = 0;

	// Waiting operations
	virtual queue_op_status wait_push(const value_type &) = 0;
	virtual queue_op_status wait_push(value_type &&) = 0;
	virtual queue_op_status wait_pop(value_type &) = 0;

	// Non-waiting operations
	virtual queue_op_status try_push(const value_type &e) = 0;
	virtual queue_op_status try_push(value_type &&) = 0;
	virtual queue_op_status try_pop(value_type &) = 0;

#if ENABLE_QUEUE_NONBLOCKING_OPS
	// Non-blocking operations
	virtual queue_op_status nonblocking_push(const value_type &) = 0;
	virtual queue_op_status nonblocking_push(value_type &&) = 0;
	virtual queue_op_status nonblocking_pop(value_type &) = 0;
#endif
};

template <typename Queue>
class queue_wrapper : public virtual queue_base<typename Queue::value_type>
{
public:
	using queue_type = Queue;

	using value_type = typename queue_type::value_type;
	using reference = value_type &;
	using const_reference = const value_type &;

	queue_wrapper(queue_type *queue) noexcept : queue_(queue)
	{
	}
	queue_wrapper(queue_type &queue) noexcept : queue_(&queue)
	{
	}

	virtual ~queue_wrapper() noexcept
	{
	}

	// State operations
	virtual void close() noexcept override
	{
		queue_->close();
	}
	virtual bool is_closed() const noexcept override
	{
		return queue_->is_closed();
	}
	virtual bool is_empty() const noexcept override
	{
		return queue_->is_empty();
	}
	virtual bool is_full() const noexcept override
	{
		return queue_->is_full();
	}
	virtual bool is_lock_free() const noexcept override
	{
		return queue_->is_lock_free();
	}

	// Basic operations
	virtual void push(const value_type &value) override
	{
		queue_->push(value);
	}
	virtual void push(value_type &&value) override
	{
		queue_->push(std::move(value));
	}
	virtual value_type value_pop() override
	{
		return queue_->value_pop();
	}

	// Waiting operations
	virtual queue_op_status wait_push(const value_type &value) override
	{
		return queue_->wait_push(value);
	}
	virtual queue_op_status wait_push(value_type &&value) override
	{
		return queue_->wait_push(std::move(value));
	}
	virtual queue_op_status wait_pop(value_type &value) override
	{
		return queue_->wait_pop(value);
	}

	// Non-waiting operations
	virtual queue_op_status try_push(const value_type &value) override
	{
		return queue_->try_push(value);
	}
	virtual queue_op_status try_push(value_type &&value) override
	{
		return queue_->try_push(std::move(value));
	}
	virtual queue_op_status try_pop(value_type &value) override
	{
		return queue_->try_pop(value);
	}

#if ENABLE_QUEUE_NONBLOCKING_OPS
	// Non-blocking operations
	virtual queue_op_status nonblocking_push(const value_type &value) override
	{
		return queue_->nonblocking_push(value);
	}
	virtual queue_op_status nonblocking_push(value_type &&value) override
	{
		return queue_->nonblocking_push(std::move(value));
	}
	virtual queue_op_status nonblocking_pop(value_type &value) override
	{
		return queue_->nonblocking_pop(value);
	}
#endif // ENABLE_QUEUE_NONBLOCKING_OPS

private:
	queue_type *queue_;
};

namespace detail {

//
// A concurrent queue input iterator. Unlike the underlying queue a given
// iterator instance is not concurrent itself. However distinct iterator
// instances for the same queue can be used from different threads and so
// provide concurrent access.
//

template <typename Queue>
class queue_input_iterator
{
public:
	using queue_type = Queue;

	using iterator_category = std::input_iterator_tag;

	using value_type = typename queue_type::value_type;
	using difference_type = void;
	using pointer = const value_type *;
	using reference = const value_type &;

	static_assert(std::is_nothrow_default_constructible<value_type>::value,
		      "value_type must be nothrow-default-constructible");
	static_assert(std::is_nothrow_destructible<value_type>::value,
		      "value_type must be nothrow-destructible");

	queue_input_iterator(queue_type &queue) noexcept : queue_(&queue)
	{
		pop_value();
	}

	constexpr queue_input_iterator() noexcept = default;
	queue_input_iterator(const queue_input_iterator &other) = default;
	queue_input_iterator &operator=(const queue_input_iterator &other) = default;

	queue_input_iterator &operator++()
	{
		pop_value();
		return *this;
	}
	queue_input_iterator operator++(int)
	{
		queue_input_iterator other(*this);
		pop_value();
		return other;
	}

	pointer operator->() const noexcept
	{
		return &value_;
	}
	reference operator*() const noexcept
	{
		return value_;
	}

	bool operator==(const queue_input_iterator &rhs) const noexcept
	{
		return queue_ == rhs.queue_;
	}
	bool operator!=(const queue_input_iterator &rhs) const noexcept
	{
		return queue_ != rhs.queue_;
	}

private:
	void pop_value()
	{
		auto status = queue_->wait_pop(value_);
		if (status == queue_op_status::closed)
			queue_ = nullptr;
	}

	queue_type *queue_ = nullptr;
	value_type value_;
};

template <typename Queue>
class queue_output_iterator
{
public:
	using queue_type = Queue;

	using iterator_category = std::output_iterator_tag;

	using value_type = typename queue_type::value_type;
	using difference_type = void;
	using pointer = void;
	using reference = void;

	queue_output_iterator(queue_type &queue) noexcept : queue_(&queue)
	{
	}

	constexpr queue_output_iterator() noexcept = default;
	queue_output_iterator(const queue_output_iterator &other) noexcept = default;
	queue_output_iterator &operator=(const queue_output_iterator &other) noexcept = default;

	queue_output_iterator &operator=(const value_type &value)
	{
		auto status = queue_->wait_push(value);
		if (status != queue_op_status::success) {
			queue_ = nullptr;
			throw status;
		}
		return *this;
	}
	queue_output_iterator &operator=(value_type &&value)
	{
		auto status = queue_->wait_push(std::move(value));
		if (status != queue_op_status::success) {
			queue_ = nullptr;
			throw status;
		}
		return *this;
	}

	queue_output_iterator &operator*() const noexcept
	{
		return *this;
	}
	queue_output_iterator &operator++() const noexcept
	{
		return *this;
	}
	queue_output_iterator &operator++(int) const noexcept
	{
		return *this;
	}

	bool operator==(const queue_output_iterator &rhs) const noexcept
	{
		return queue_ == rhs.queue_;
	}
	bool operator!=(const queue_output_iterator &rhs) const noexcept
	{
		return queue_ != rhs.queue_;
	}

private:
	queue_type *queue_ = nullptr;
};

} // namespace detail

template <typename Queue>
class generic_queue_back
{
public:
	using queue_type = Queue;

	using value_type = typename queue_type::value_type;
	using reference = value_type &;
	using const_reference = const value_type &;

	using iterator = detail::queue_output_iterator<queue_type>;
	using const_iterator = const iterator;

	generic_queue_back(queue_type *queue) noexcept : queue_(queue)
	{
	}
	generic_queue_back(queue_type &queue) noexcept : queue_(&queue)
	{
	}

	generic_queue_back(const generic_queue_back &other) noexcept = default;
	generic_queue_back &operator=(const generic_queue_back &other) noexcept = default;

	// State operations
	void close() noexcept
	{
		queue_->close();
	}
	bool is_closed() const noexcept
	{
		return queue_->is_closed();
	}
	bool is_empty() const noexcept
	{
		return queue_->is_empty();
	}
	bool is_full() const noexcept
	{
		return queue_->is_full();
	}
	bool is_lock_free() const noexcept
	{
		return queue_->is_lock_free();
	}
	bool has_queue() const noexcept
	{
		return queue_ != nullptr;
	}

	// Iterators
	iterator begin() noexcept
	{
		return iterator(*this);
	}
	iterator end() noexcept
	{
		return iterator();
	}
	const_iterator cbegin() noexcept
	{
		return const_iterator(*this);
	}
	const_iterator cend() noexcept
	{
		return const_iterator();
	}

	// Basic operations
	void push(const value_type &value)
	{
		queue_->push(value);
	}
	void push(value_type &&value)
	{
		queue_->push(std::move(value));
	}

	// Waiting operations
	queue_op_status wait_push(const value_type &value)
	{
		return queue_->wait_push(value);
	}
	queue_op_status wait_push(value_type &&value)
	{
		return queue_->wait_push(std::move(value));
	}

	// Non-waiting operations
	queue_op_status try_push(const value_type &value)
	{
		return queue_->try_push(value);
	}
	queue_op_status try_push(value_type &&value)
	{
		return queue_->try_push(std::move(value));
	}

#if ENABLE_QUEUE_NONBLOCKING_OPS
	// Non-blocking operations
	queue_op_status nonblocking_push(const value_type &value)
	{
		return queue_->nonblocking_push(value);
	}
	queue_op_status nonblocking_push(value_type &&value)
	{
		return queue_->nonblocking_push(std::move(value));
	}
#endif

private:
	queue_type *queue_;
};

template <typename Queue>
class generic_queue_front
{
public:
	using queue_type = Queue;

	using value_type = typename queue_type::value_type;
	using reference = value_type &;
	using const_reference = const value_type &;

	using iterator = detail::queue_input_iterator<queue_type>;
	using const_iterator = const iterator;

	generic_queue_front(queue_type *queue) noexcept : queue_(queue)
	{
	}
	generic_queue_front(queue_type &queue) noexcept : queue_(&queue)
	{
	}

	generic_queue_front(const generic_queue_front &other) noexcept = default;
	generic_queue_front &operator=(const generic_queue_front &other) noexcept = default;

	// State operations
	void close() noexcept
	{
		queue_->close();
	}
	bool is_closed() const noexcept
	{
		return queue_->is_closed();
	}
	bool is_empty() const noexcept
	{
		return queue_->is_empty();
	}
	bool is_full() const noexcept
	{
		return queue_->is_full();
	}
	bool is_lock_free() const noexcept
	{
		return queue_->is_lock_free();
	}
	bool has_queue() const noexcept
	{
		return queue_ != nullptr;
	}

	// Iterators
	iterator begin() noexcept
	{
		return iterator(*this);
	}
	iterator end() noexcept
	{
		return iterator();
	}
	const_iterator cbegin() noexcept
	{
		return const_iterator(*this);
	}
	const_iterator cend() noexcept
	{
		return const_iterator();
	}

	// Basic operations
	value_type value_pop()
	{
		return queue_->value_pop();
	}

	// Waiting operations
	queue_op_status wait_pop(value_type &value)
	{
		return queue_->wait_pop(value);
	}

	// Non-waiting operations
	queue_op_status try_pop(value_type &value)
	{
		return queue_->try_pop(value);
	}

#if ENABLE_QUEUE_NONBLOCKING_OPS
	// Non-blocking operations
	queue_op_status nonblocking_pop(value_type &value)
	{
		return queue_->nonblocking_pop(value);
	}
#endif

private:
	queue_type *queue_;
};

template <typename Value>
using queue_back = generic_queue_back<queue_base<Value>>;

template <typename Value>
using queue_front = generic_queue_front<queue_base<Value>>;

} // namespace evenk

#endif // !EVENK_CONQUEUE_H_
