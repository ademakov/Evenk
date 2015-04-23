//
// Spin Locks
//

#ifndef EVENK_SPINLOCK_H_
#define EVENK_SPINLOCK_H_

#include <atomic>

#include "evenk/backoff.h"

namespace ev {
namespace concurrency {

class SpinLock {
 public:
  SpinLock() = default;
  SpinLock(const SpinLock&) = delete;
  SpinLock& operator=(const SpinLock&) = delete;

  void Lock() { Lock(NoBackoff{}); }

  template <typename Backoff>
  void Lock(Backoff backoff) noexcept {
    while (lock_.test_and_set(std::memory_order_acquire)) backoff();
  }

  void Unlock() { lock_.clear(std::memory_order_release); }

 private:
  std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};

class TicketLock {
 public:
  TicketLock() = default;
  TicketLock(const TicketLock&) = delete;
  TicketLock& operator=(const TicketLock&) = delete;

  void Lock() { Lock(NoBackoff{}); }

  template <typename Backoff>
  void Lock(Backoff backoff) {
    base_type tail = tail_.fetch_add(1, std::memory_order_relaxed);
    for (;;) {
      base_type head = head_.load(std::memory_order_acquire);
      if (tail == head) break;
      backoff();
    }
  }

  template <typename Pause, uint32_t Backoff = 50>
  void Lock(ProportionalBackoff<Pause> backoff) {
    base_type tail = tail_.fetch_add(1, std::memory_order_relaxed);
    for (;;) {
      base_type head = head_.load(std::memory_order_acquire);
      if (tail == head) break;
      backoff(static_cast<base_type>(tail - head));
    }
  }

  void Unlock() { head_.fetch_add(1, std::memory_order_release); }

 private:
  using base_type = uint16_t;

  std::atomic<base_type> head_ = ATOMIC_VAR_INIT(0);
  std::atomic<base_type> tail_ = ATOMIC_VAR_INIT(0);
};

}  // namespace concurrency
}  // namespace ev

#endif  // !EVENK_SPINLOCK_H_
