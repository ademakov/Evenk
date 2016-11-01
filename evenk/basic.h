//
// Basic Definitions
//
// Copyright (c) 2015-2016  Aleksey Demakov
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

#ifndef EVENK_BASIC_H_
#define EVENK_BASIC_H_

#include <cstddef>
#include <system_error>

namespace evenk {

constexpr std::size_t cache_line_size = 64;

[[noreturn]] void
throw_system_error(int err_num)
{
	throw std::system_error(err_num, std::system_category());
}

[[noreturn]] void
throw_system_error(int err_num, const char *what)
{
	throw std::system_error(err_num, std::system_category(), what);
}

[[noreturn]] void
throw_system_error(int err_num, const std::string &what)
{
	throw std::system_error(err_num, std::system_category(), what);
}

class non_copyable
{
protected:
	constexpr non_copyable() noexcept = default;
	~non_copyable() noexcept = default;

	non_copyable(const non_copyable &) = delete;
	non_copyable &operator=(const non_copyable &) = delete;
};

} // namespace evenk

#endif // !EVENK_BASIC_H_
