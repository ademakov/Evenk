//
// Miscellaneous concurrency utilities.
//

#ifndef EVENK_CONCURRENCY_H_
#define EVENK_CONCURRENCY_H_

#include <assert.h>
#include <limits.h>
#include <pthread.h>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <system_error>

#include <emmintrin.h>

#include "evenk/futex.h"

namespace ev {
namespace concurrency {

//
// Delays for busy waiting.
//

class CPUCycle {
 public:
  void operator()(uint32_t n) noexcept {
    while (n--) std::atomic_signal_fence(std::memory_order_relaxed);
  }
};

class CPURelax {
 public:
  void operator()(uint32_t n) noexcept {
    while (n--) _mm_pause();
  }
};

class NanoSleep {
 public:
  void operator()(uint32_t n) noexcept {
    timespec ts = {.tv_sec = 0, .tv_nsec = n};
    nanosleep(&ts, NULL);
  }
};

//
// Back-off policies for busy waiting.
//

class NoBackoff {
 public:
  bool operator()() noexcept { return true; }
};

class YieldBackoff {
 public:
  bool operator()() noexcept {
    std::this_thread::yield();
    return false;
  }
};

template <typename Pause>
class LinearBackoff {
 public:
  LinearBackoff(uint32_t ceiling) noexcept : ceiling_{ceiling}, backoff_{0} {}

  bool operator()() noexcept {
    if (backoff_ >= ceiling_) {
      pause_(ceiling_);
      return true;
    } else {
      pause_(backoff_++);
      return false;
    }
  }

 private:
  const uint32_t ceiling_;
  uint32_t backoff_;
  Pause pause_;
};

template <typename Pause>
class ExponentialBackoff {
 public:
  ExponentialBackoff(uint32_t ceiling) noexcept : ceiling_{ceiling},
                                                  backoff_{0} {}

  bool operator()() noexcept {
    if (backoff_ >= ceiling_) {
      pause_(ceiling_);
      return true;
    } else {
      pause_(backoff_);
      backoff_ += backoff_ + 1;
      return false;
    }
  }

 private:
  const uint32_t ceiling_;
  uint32_t backoff_;
  Pause pause_;
};

template <typename Pause>
class ProportionalBackoff {
 public:
  ProportionalBackoff(uint32_t backoff) noexcept : backoff_{backoff} {}

  bool operator()(uint32_t factor = 1) noexcept {
    pause_(backoff_ * factor);
    return false;
  }

 private:
  uint32_t backoff_;
  Pause pause_;
};

template <typename FirstBackoff, typename SecondBackoff>
class CompositeBackoff {
 public:
  CompositeBackoff(FirstBackoff a, SecondBackoff b) noexcept
      : first_(a),
        second_(b),
        use_second_{false} {}

  bool operator()() noexcept {
    if (use_second_) return second_();
    use_second_ = first_();
    return false;
  }

 private:
  FirstBackoff first_;
  SecondBackoff second_;
  bool use_second_;
};

//
// Spin Locks
//

class SpinLock {
 public:
  SpinLock() = default;
  SpinLock(const SpinLock&) = delete;
  SpinLock& operator=(const SpinLock&) = delete;

  void Lock() noexcept { Lock(NoBackoff{}); }

  template <typename Backoff>
  void Lock(Backoff backoff) noexcept {
    while (lock_.test_and_set(std::memory_order_acquire)) backoff();
  }

  void Unlock() noexcept { lock_.clear(std::memory_order_release); }

 private:
  std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};

class TicketLock {
 public:
  TicketLock() = default;
  TicketLock(const TicketLock&) = delete;
  TicketLock& operator=(const TicketLock&) = delete;

  void Lock() noexcept { Lock(NoBackoff{}); }

  template <typename Backoff>
  void Lock(Backoff backoff) noexcept {
    base_type tail = tail_.fetch_add(1, std::memory_order_relaxed);
    for (;;) {
      base_type head = head_.load(std::memory_order_acquire);
      if (tail == head) break;
      backoff();
    }
  }

  template <typename Pause, uint32_t Backoff = 50>
  void Lock(ProportionalBackoff<Pause> backoff) noexcept {
    base_type tail = tail_.fetch_add(1, std::memory_order_relaxed);
    for (;;) {
      base_type head = head_.load(std::memory_order_acquire);
      if (tail == head) break;
      backoff(static_cast<base_type>(tail - head));
    }
  }

  void Unlock() noexcept { head_.fetch_add(1, std::memory_order_release); }

 private:
  using base_type = uint16_t;

  std::atomic<base_type> head_ = ATOMIC_VAR_INIT(0);
  std::atomic<base_type> tail_ = ATOMIC_VAR_INIT(0);
};

//
// Waitable Locks
//

class StdMutex : public std::mutex {
 public:
  void Lock() { lock(); }
  void Unlock() { unlock(); }
};

class PosixMutex {
 public:
  PosixMutex() noexcept : mutex_(PTHREAD_MUTEX_INITIALIZER) {}

  ~PosixMutex() noexcept { pthread_mutex_destroy(&mutex_); }

  PosixMutex(const PosixMutex&) = delete;
  PosixMutex& operator=(const PosixMutex&) = delete;

  void Lock() {
    int ret = pthread_mutex_lock(&mutex_);
    if (ret)
      throw std::system_error(ret, std::system_category(),
                              "pthread_mutex_lock()");
  }

  void Unlock() {
    int ret = pthread_mutex_unlock(&mutex_);
    if (ret)
      throw std::system_error(ret, std::system_category(),
                              "pthread_mutex_unlock()");
  }

 private:
  friend class PosixCondVar;

  pthread_mutex_t mutex_;
};

class FutexLock {
 public:
  FutexLock() noexcept : futex_(0) {}

  FutexLock(const FutexLock&) = delete;
  FutexLock& operator=(const FutexLock&) = delete;

  void Lock() { Lock(NoBackoff{}); }

  template <typename Backoff>
  void Lock(Backoff backoff) {
    for (uint32_t value = 0; !futex_.compare_exchange_strong(
             value, 1, std::memory_order_acquire, std::memory_order_relaxed);
         value = 0) {
      if (backoff()) {
        if (value == 2 || futex_.exchange(2, std::memory_order_acquire)) {
          do
            futex_wait(futex_, 2);
          while (futex_.exchange(2, std::memory_order_acquire));
        }
        break;
      }
    }
  }

  void Unlock() {
    if (futex_.fetch_sub(1, std::memory_order_release) != 1) {
      futex_.store(0, std::memory_order_relaxed);
      ev::futex_wake(futex_, 1);
    }
  }

 private:
  friend class FutexCondVar;

  std::atomic<uint32_t> futex_;
};

//
// Lock Guard
//

template <typename LockType>
class LockGuard {
 public:
  LockGuard(LockType& lock) : lock_ptr_(&lock), owns_lock_(false) { Lock(); }

  template <typename Backoff>
  LockGuard(LockType& lock, Backoff backoff)
      : lock_ptr_(&lock), owns_lock_(false) {
    Lock(backoff);
  }

  LockGuard(LockType& lock, std::adopt_lock_t) noexcept : lock_ptr_(&lock),
                                                          owns_lock_(true) {}

  LockGuard(LockType& lock, std::defer_lock_t) noexcept : lock_ptr_(&lock),
                                                          owns_lock_(false) {}

  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

  ~LockGuard() {
    if (owns_lock_) lock_ptr_->Unlock();
  }

  void Lock() {
    lock_ptr_->Lock();
    owns_lock_ = true;
  }

  template <typename Backoff>
  void Lock(Backoff backoff) {
    lock_ptr_->Lock(backoff);
    owns_lock_ = true;
  }

  void Unlock() {
    lock_ptr_->Unlock();
    owns_lock_ = false;
  }

  LockType* GetLockPtr() { return lock_ptr_; }

  bool OwnsLock() { return owns_lock_; }

 private:
  LockType* lock_ptr_;
  bool owns_lock_;
};

//
// Condition Variables
//

class StdCondVar : public std::condition_variable {
 public:
  void Wait(LockGuard<StdMutex>& guard) {
    std::unique_lock<std::mutex> lock(*guard.GetLockPtr(), std::adopt_lock);
    wait(lock);
    lock.release();
  }

  void NotifyOne() { notify_one(); }
  void NotifyAll() { notify_all(); }
};

class PosixCondVar {
 public:
  PosixCondVar() noexcept : condition_(PTHREAD_COND_INITIALIZER) {}

  PosixCondVar(const PosixCondVar&) = delete;
  PosixCondVar& operator=(const PosixCondVar&) = delete;

  ~PosixCondVar() noexcept { pthread_cond_destroy(&condition_); }

  void Wait(LockGuard<PosixMutex>& guard) {
    int ret = pthread_cond_wait(&condition_, &guard.GetLockPtr()->mutex_);
    if (ret)
      throw std::system_error(ret, std::system_category(),
                              "pthread_cond_wait()");
  }

  void NotifyOne() {
    int ret = pthread_cond_signal(&condition_);
    if (ret)
      throw std::system_error(ret, std::system_category(),
                              "pthread_cond_signal()");
  }

  void NotifyAll() {
    int ret = pthread_cond_broadcast(&condition_);
    if (ret)
      throw std::system_error(ret, std::system_category(),
                              "pthread_cond_broadcast()");
  }

 private:
  pthread_cond_t condition_;
};

class FutexCondVar {
 public:
  FutexCondVar() noexcept : futex_(0), count_(0), owner_(nullptr) {}

  FutexCondVar(const FutexCondVar&) = delete;
  FutexCondVar& operator=(const FutexCondVar&) = delete;

  void Wait(LockGuard<FutexLock>& guard) {
    FutexLock* owner = guard.GetLockPtr();
    assert(owner_ == nullptr || owner_ == owner);
    owner_.store(owner, std::memory_order_relaxed);

    count_.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acq_rel);
    uint32_t value = futex_.load(std::memory_order_relaxed);

    owner->Unlock();

    ev::futex_wait(futex_, value);

    count_.fetch_sub(1, std::memory_order_relaxed);
    while (owner->futex_.exchange(2, std::memory_order_acquire))
      futex_wait(owner->futex_, 2);
  }

  void NotifyOne() {
    futex_.fetch_add(1, std::memory_order_acquire);
    if (count_.load(std::memory_order_relaxed)) ev::futex_wake(futex_, 1);
  }

  void NotifyAll() {
    futex_.fetch_add(1, std::memory_order_acquire);
    if (count_.load(std::memory_order_relaxed)) {
      FutexLock* owner = owner_.load(std::memory_order_relaxed);
      if (owner) ev::futex_requeue(futex_, 1, INT_MAX, owner->futex_);
    }
  }

 private:
  std::atomic<uint32_t> futex_;
  std::atomic<uint32_t> count_;
  std::atomic<FutexLock*> owner_;
};

//
// Synchronization Traits
//

class StdSynch {
 public:
  using LockType = StdMutex;
  using CondVarType = StdCondVar;
};

class PosixSynch {
 public:
  using LockType = PosixMutex;
  using CondVarType = PosixCondVar;
};

class FutexSynch {
 public:
  using LockType = FutexLock;
  using CondVarType = FutexCondVar;
};

#if __linux__
using DefaultSynch = FutexSynch;
#else
using DefaultSynch = StdSynch;
#endif

}  // namespace concurrency
}  // namespace ev

#endif  // !EVENK_CONCURRENCY_H_
