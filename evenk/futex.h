//
// Linux Futex Support
//
// Copyright (c) 2015  Aleksey Demakov
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

#ifndef EVENK_FUTEX_H_
#define EVENK_FUTEX_H_

#include <atomic>
#include <cerrno>
#include <cstddef>

#if __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace evenk {

typedef std::atomic<std::uint32_t> futex_t;

inline int
futex_wait(futex_t &futex __attribute__((unused)), std::uint32_t value __attribute__((unused)))
{
#if __linux__
#if __x86_64__
	unsigned result;
	__asm__ __volatile__("xor %%r10, %%r10\n\t"
			     "syscall"
			     : "=a"(result), "+m"(futex)
			     : "0"(SYS_futex), "D"(&futex), "S"(FUTEX_WAIT_PRIVATE), "d"(value)
			     : "cc", "rcx", "r10", "r11", "memory");
	return (result > (unsigned) -4096) ? (int) result : 0;
#else
	if (syscall(SYS_futex, &futex, FUTEX_WAIT_PRIVATE, value, NULL, NULL, 0) == -1)
		return -errno;
	else
		return 0;
#endif
#else
	return -ENOSYS;
#endif
}

inline int
futex_wake(futex_t &futex __attribute__((unused)), int count __attribute__((unused)))
{
#if __linux__
#if __x86_64__
	unsigned result;
	__asm__ __volatile__("syscall"
			     : "=a"(result), "+m"(futex)
			     : "0"(SYS_futex), "D"(&futex), "S"(FUTEX_WAKE_PRIVATE), "d"(count)
			     : "cc", "rcx", "r11");
	return (result > (unsigned) -4096) ? (int) result : 0;
#else
	if (syscall(SYS_futex, &futex, FUTEX_WAKE_PRIVATE, count, NULL, NULL, 0) == -1)
		return -errno;
	else
		return 0;
#endif
#else
	return -ENOSYS;
#endif
}

inline int
futex_requeue(futex_t &futex __attribute__((unused)),
	      int futex_count __attribute__((unused)),
	      int queue_count __attribute__((unused)),
	      futex_t &queue __attribute__((unused)))
{
#if __linux__
#if __x86_64__
	unsigned result;
	register int arg4 __asm__("r10") = queue_count;
	register void *arg5 __asm__("r8") = &queue;
	__asm__ __volatile__("syscall"
			     : "=a"(result), "+m"(futex)
			     : "0"(SYS_futex),
			       "D"(&futex),
			       "S"(FUTEX_REQUEUE_PRIVATE),
			       "d"(futex_count),
			       "r"(arg4),
			       "r"(arg5)
			     : "cc", "rcx", "r11");
	return (result > (unsigned) -4096) ? (int) result : 0;
#else
	if (syscall(SYS_futex,
		    &futex,
		    FUTEX_REQUEUE_PRIVATE,
		    futex_count,
		    queue_count,
		    &queue,
		    0)
	    == -1)
		return -errno;
	else
		return 0;
#endif
#else
	return -ENOSYS;
#endif
}

inline int
futex_requeue(futex_t &futex __attribute__((unused)),
	      int futex_count __attribute__((unused)),
	      int queue_count __attribute__((unused)),
	      futex_t &queue __attribute__((unused)),
	      std::uint32_t futex_value __attribute__((unused)))
{
#if __linux__
#if __x86_64__
	unsigned result;
	register int arg4 __asm__("r10") = queue_count;
	register void *arg5 __asm__("r8") = &queue;
	register int arg6 __asm__("r9") = futex_value;
	__asm__ __volatile__("syscall"
			     : "=a"(result), "+m"(futex)
			     : "0"(SYS_futex),
			       "D"(&futex),
			       "S"(FUTEX_CMP_REQUEUE_PRIVATE),
			       "d"(futex_count),
			       "r"(arg4),
			       "r"(arg5),
			       "r"(arg6)
			     : "cc", "rcx", "r11");
	return (result > (unsigned) -4096) ? (int) result : 0;
#else
	if (syscall(SYS_futex,
		    &futex,
		    FUTEX_CMP_REQUEUE_PRIVATE,
		    futex_count,
		    queue_count,
		    &queue,
		    futex_value)
	    == -1)
		return -errno;
	else
		return 0;
#endif
#else
	return -ENOSYS;
#endif
}

} // namespace evenk

#endif // !EVENK_FUTEX_H_
