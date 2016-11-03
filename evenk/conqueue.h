//
// Concurrent Queue Basics and Utilities
//
// Copyright (c) 2016  Aleksey Demakov
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
// The code in this file is based on the following proposal:
//    http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2016/p0260r0.html
//

#include <iterator>
#include <utility>

namespace evenk {

enum class queue_op_status { success = 0, empty, full, closed, busy };

template <typename Value>
class queue_base
{
public:
	using value_type = Value;
	using reference = value_type &;
	using const_reference = const value_type &;

	virtual ~queue_base(){};

	virtual void close() = 0;

	virtual bool is_closed() = 0;
	virtual bool is_empty() = 0;
	virtual bool is_full() = 0;
	virtual bool is_lock_free() = 0;

	virtual void push(const value_type &) = 0;
	virtual queue_op_status wait_push(const value_type &) = 0;
	virtual queue_op_status try_push(const value_type &e) = 0;
	virtual queue_op_status nonblocking_push(const value_type &) = 0;

	virtual void push(value_type &&) = 0;
	virtual queue_op_status wait_push(value_type &&) = 0;
	virtual queue_op_status try_push(value_type &&) = 0;
	virtual queue_op_status nonblocking_push(value_type &&) = 0;

	virtual value_type value_pop() = 0;
	virtual queue_op_status wait_pop(value_type &) = 0;
	virtual queue_op_status try_pop(value_type &) = 0;
	virtual queue_op_status nonblocking_pop(value_type &) = 0;
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

	virtual void close() override
	{
		queue_->close();
	}

	virtual bool is_closed() override
	{
		return queue_->is_closed();
	}
	virtual bool is_empty() override
	{
		return queue_->is_empty();
	}
	virtual bool is_full() override
	{
		return queue_->is_full();
	}
	virtual bool is_lock_free() override
	{
		return queue_->is_lock_free();
	}

	virtual void push(const value_type &value) override
	{
		queue_->push(value);
	}
	virtual queue_op_status wait_push(const value_type &value) override
	{
		return queue_->wait_push(value);
	}
	virtual queue_op_status try_push(const value_type &value) override
	{
		return queue_->try_push(value);
	}
	virtual queue_op_status nonblocking_push(const value_type &value) override
	{
		return queue_->nonblocking_push(value);
	}

	virtual void push(value_type &&value) override
	{
		queue_->push(std::move(value));
	}
	virtual queue_op_status wait_push(value_type &&value) override
	{
		return queue_->wait_push(std::move(value));
	}
	virtual queue_op_status try_push(value_type &&value) override
	{
		return queue_->try_push(std::move(value));
	}
	virtual queue_op_status nonblocking_push(value_type &&value) override
	{
		return queue_->nonblocking_push(std::move(value));
	}

	virtual value_type value_pop() override
	{
		return queue_->value_pop();
	}
	virtual queue_op_status wait_pop(value_type &value) override
	{
		return queue_->wait_pop(value);
	}
	virtual queue_op_status try_pop(value_type &value) override
	{
		return queue_->try_pop(value);
	}
	virtual queue_op_status nonblocking_pop(value_type &value) override
	{
		return queue_->nonblocking_pop(value);
	}

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

	constexpr queue_input_iterator() noexcept = default;
	~queue_input_iterator() noexcept = default;

	queue_input_iterator(queue_type &queue) noexcept : queue_(&queue)
	{
		pop_value();
	}

	queue_input_iterator(const queue_input_iterator &other) noexcept(
		std::is_nothrow_copy_constructible<value_type>::value)
		: queue_(other.queue_), status_(other.status_)
	{
		if (status_ == queue_op_status::success)
			cached_value_ = other.cached_value_;
	}

	queue_input_iterator &operator=(const queue_input_iterator &other) noexcept(
		std::is_nothrow_copy_constructible<value_type>::value)
	{
		queue_ = other.queue_;
		status_ = other.status_;
		if (status_ == queue_op_status::success)
			cached_value_ = other.cached_value_;
		return *this;
	}

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

	pointer operator->() const
	{
		if (status_ != queue_op_status::success)
			throw status_;
		return &cached_value_;
	}
	reference operator*() const
	{
		if (status_ != queue_op_status::success)
			throw status_;
		return cached_value_;
	}

	bool operator==(const queue_input_iterator &rhs)
	{
		return status_ == rhs.status_
		       && (status_ == queue_op_status::closed || queue_ == rhs.queue_);
	}
	bool operator!=(const queue_input_iterator &rhs)
	{
		return status_ != rhs.status_
		       || (status_ != queue_op_status::closed && queue_ != rhs.queue_);
	}

private:
	void pop_value()
	{
		status_ = queue_->wait_pop(cached_value_);
	}

	queue_type *queue_ = nullptr;
	queue_op_status status_ = queue_op_status::closed;
	value_type cached_value_;
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

	constexpr queue_output_iterator() noexcept = default;
	~queue_output_iterator() noexcept = default;

	queue_output_iterator(queue_type &queue) : queue_(&queue)
	{
	}

	queue_output_iterator &operator=(const value_type &value)
	{
		status_ = queue_->wait_push(value);
		return *this;
	}
	queue_output_iterator &operator=(value_type &&value)
	{
		status_ = queue_->wait_push(std::move(value));
		return *this;
	}

	queue_output_iterator &operator*()
	{
		return *this;
	}
	queue_output_iterator &operator++()
	{
		return *this;
	}
	queue_output_iterator &operator++(int)
	{
		return *this;
	}

	bool operator==(const queue_output_iterator &rhs)
	{
		return status_ == rhs.status_
		       && (status_ == queue_op_status::closed || queue_ == rhs.queue_);
	}
	bool operator!=(const queue_output_iterator &rhs)
	{
		return status_ != rhs.status_
		       || (status_ != queue_op_status::closed && queue_ != rhs.queue_);
	}

private:
	queue_type *queue_ = nullptr;
	queue_op_status status_ = queue_op_status::closed;
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

	generic_queue_back(const generic_queue_back &other) = default;
	generic_queue_back &operator=(const generic_queue_back &other) = default;

	void close()
	{
		queue_->close();
	}

	bool is_closed()
	{
		return queue_->is_closed();
	}
	bool is_empty()
	{
		return queue_->is_empty();
	}
	bool is_full()
	{
		return queue_->is_full();
	}
	bool is_lock_free()
	{
		return queue_->is_lock_free();
	}

	bool has_queue()
	{
		return queue_ != nullptr;
	}

	iterator begin()
	{
		return iterator(*this);
	}
	iterator end()
	{
		return iterator();
	}
	const_iterator cbegin()
	{
		return const_iterator(*this);
	}
	const_iterator cend()
	{
		return const_iterator();
	}

	void push(const value_type &value) override
	{
		queue_->push(value);
	}
	queue_op_status wait_push(const value_type &value) override
	{
		return queue_->wait_push(value);
	}
	queue_op_status try_push(const value_type &value) override
	{
		return queue_->try_push(value);
	}
	queue_op_status nonblocking_push(const value_type &value) override
	{
		return queue_->nonblocking_push(value);
	}

	void push(value_type &&value)
	{
		queue_->push(std::move(value));
	}
	queue_op_status wait_push(value_type &&value) override
	{
		return queue_->wait_push(std::move(value));
	}
	queue_op_status try_push(value_type &&value)
	{
		return queue_->try_push(std::move(value));
	}
	queue_op_status nonblocking_push(value_type &&value)
	{
		return queue_->nonblocking_push(std::move(value));
	}

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

	generic_queue_front(const generic_queue_front &other) = default;
	generic_queue_front &operator=(const generic_queue_front &other) = default;

	void close()
	{
		queue_->close();
	}

	bool is_closed()
	{
		return queue_->is_closed();
	}
	bool is_empty()
	{
		return queue_->is_empty();
	}
	bool is_full()
	{
		return queue_->is_full();
	}
	bool is_lock_free()
	{
		return queue_->is_lock_free();
	}

	bool has_queue()
	{
		return queue_ != nullptr;
	}

	iterator begin()
	{
		return iterator(*this);
	}
	iterator end()
	{
		return iterator();
	}
	const_iterator cbegin()
	{
		return const_iterator(*this);
	}
	const_iterator cend()
	{
		return const_iterator();
	}

	value_type value_pop()
	{
		return queue_->value_pop();
	}
	queue_op_status wait_pop(value_type &value)
	{
		return queue_->wait_pop(value);
	}
	queue_op_status try_pop(value_type &value)
	{
		return queue_->try_pop(value);
	}
	queue_op_status nonblocking_pop(value_type &value)
	{
		return queue_->nonblocking_pop(value);
	}

private:
	queue_type *queue_;
};

template <typename Value>
using queue_back = generic_queue_back<queue_base<Value>>;

template <typename Value>
using queue_front = generic_queue_front<queue_base<Value>>;

} // namespace evenk

#endif // !EVENK_CONQUEUE_H_
