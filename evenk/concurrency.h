//
// Miscellaneous concurrency utilities.
//
#ifndef EVENK_CONCURRENCY_H_
#define EVENK_CONCURRENCY_H_

#include <assert.h>
#include <limits.h>
#include <pthread.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <system_error>

#include <emmintrin.h>

#include "evenk/basic.h"
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

//
// Simple Concurrent Queue
//

template <typename ValueType, typename SynchPolicy = DefaultSynch,
          typename Container = std::deque<ValueType>>
class Queue {
 public:
  Queue() noexcept : finish_(false) {}

  Queue(Queue&& other) noexcept : finish_(other.finish_) {
    std::swap(queue_, other.queue_);
  }

  bool Empty() const {
    LockGuard<LockType> guard(lock_);
    return queue_.empty();
  }

  bool Finished() const { return finish_; }

  void Finish() {
    LockGuard<LockType> guard(lock_);
    finish_ = true;
    cond_.NotifyAll();
  }

  template <typename... Backoff>
  void Enqueue(ValueType&& data, Backoff... backoff) {
    LockGuard<LockType> guard(lock_, std::forward<Backoff>(backoff)...);
    queue_.push_back(std::move(data));
    cond_.NotifyOne();
  }

  template <typename... Backoff>
  void Enqueue(const ValueType& data, Backoff... backoff) {
    LockGuard<LockType> guard(lock_, std::forward<Backoff>(backoff)...);
    queue_.push_back(data);
    cond_.NotifyOne();
  }

  template <typename... Backoff>
  bool Dequeue(ValueType& data, Backoff... backoff) {
    LockGuard<LockType> guard(lock_, std::forward<Backoff>(backoff)...);
    while (queue_.empty()) {
      if (Finished()) return false;
      cond_.Wait(guard);
    }
    data = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

 private:
  using LockType = typename SynchPolicy::LockType;
  using CondVarType = typename SynchPolicy::CondVarType;

  bool finish_;
  LockType lock_;
  CondVarType cond_;
  Container queue_;
};

//
// Fast Bounded Concurrent Queue
//

struct BoundedQueueSlotBase {
 public:
  void Initialize(uint32_t value) {
    ticket_.store(value, std::memory_order_relaxed);
  }

  uint32_t Load() const { return ticket_.load(std::memory_order_acquire); }

  void Store(uint32_t value) {
    ticket_.store(value, std::memory_order_release);
  }

 protected:
  std::atomic<uint32_t> ticket_;
};

class BoundedQueueNoWait : public BoundedQueueSlotBase {
 public:
  uint32_t WaitAndLoad(uint32_t) { return Load(); }

  void StoreAndWake(uint32_t value) { Store(value); }

  void Wake() {}
};

class BoundedQueueYieldWait : public BoundedQueueSlotBase {
 public:
  uint32_t WaitAndLoad(uint32_t) {
    std::this_thread::yield();
    return Load();
  }

  void StoreAndWake(uint32_t value) { Store(value); }

  void Wake() {}
};

class BoundedQueueFutexWait : public BoundedQueueSlotBase {
 public:
  uint32_t WaitAndLoad(uint32_t value) {
    wait_count_.fetch_add(1, std::memory_order_relaxed);
    ev::futex_wait(ticket_, value);  // Presuming this is a full memory fence.
    wait_count_.fetch_sub(1, std::memory_order_relaxed);
    return Load();
  }

  void StoreAndWake(uint32_t value) {
    Store(value);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (wait_count_.load(std::memory_order_relaxed)) Wake();
  }

  void Wake() { futex_wake(ticket_, INT32_MAX); }

 private:
  std::atomic<uint32_t> wait_count_ = ATOMIC_VAR_INIT(0);
};

template <typename Synch = DefaultSynch>
class BoundedQueueSynchWait : public BoundedQueueSlotBase {
 public:
  uint32_t WaitAndLoad(uint32_t value) {
    LockGuard<LockType> guard(lock_);
    uint32_t current_value = ticket_.load(std::memory_order_relaxed);
    if (current_value == value) {
      cond_.Wait(guard);
      current_value = ticket_.load(std::memory_order_relaxed);
    }
    return current_value;
  }

  void StoreAndWake(uint32_t value) {
    LockGuard<LockType> guard(lock_);
    ticket_.store(value, std::memory_order_relaxed);
    cond_.NotifyAll();
  }

  void Wake() {
    LockGuard<LockType> guard(lock_);
    cond_.NotifyAll();
  }

 private:
  using LockType = typename Synch::LockType;
  using CondVarType = typename Synch::CondVarType;

  LockType lock_;
  CondVarType cond_;
};

template <typename ValueType, typename WaitType = BoundedQueueNoWait>
class BoundedQueue {
 public:
  BoundedQueue(uint32_t size)
      : ring_{nullptr}, mask_{size - 1}, finish_{false}, head_{0}, tail_{0} {
    if (size == 0 || (size & mask_) != 0)
      throw std::invalid_argument("BoundedQueue size must be a power of two");

    void* ring;
    if (posix_memalign(&ring, ev::kCacheLineSize, size * sizeof(Slot)))
      throw std::bad_alloc();

    ring_ = new (ring) Slot[size];
    for (uint32_t i = 0; i < size; i++) ring_[i].Initialize(i);
  }

  BoundedQueue(BoundedQueue&& other) noexcept : ring_{other.ring_},
                                                mask_{other.mask_},
                                                finish_{false},
                                                head_{0},
                                                tail_{0} {
    other.ring_ = nullptr;
  }

  BoundedQueue(BoundedQueue const&) = delete;
  BoundedQueue& operator=(BoundedQueue const&) = delete;

  ~BoundedQueue() { Destroy(); }

  bool Empty() const {
    int64_t head = head_.load(std::memory_order_relaxed);
    int64_t tail = tail_.load(std::memory_order_relaxed);
    return (tail <= head);
  }

  bool Finished() const { return finish_.load(std::memory_order_relaxed); }

  void Finish() {
    finish_.store(true, std::memory_order_relaxed);
    for (uint32_t i = 0; i < mask_ + 1; i++) ring_[i].Wake();
  }

  template <typename... Backoff>
  void Enqueue(ValueType&& value, Backoff... backoff) {
    const uint64_t tail = tail_.fetch_add(1, std::memory_order_seq_cst);
    Slot& slot = ring_[tail & mask_];
    WaitTail(slot, tail, std::forward<Backoff>(backoff)...);
    slot.value = std::move(value);
    WakeHead(slot, tail + 1);
  }

  template <typename... Backoff>
  void Enqueue(const ValueType& value, Backoff... backoff) {
    const uint64_t tail = tail_.fetch_add(1, std::memory_order_seq_cst);
    Slot& slot = ring_[tail & mask_];
    WaitTail(slot, tail, std::forward<Backoff>(backoff)...);
    slot.value = value;
    WakeHead(slot, tail + 1);
  }

  template <typename... Backoff>
  bool Dequeue(ValueType& value, Backoff... backoff) {
    const uint64_t head = head_.fetch_add(1, std::memory_order_relaxed);
    Slot& slot = ring_[head & mask_];
    if (!WaitHead(slot, head + 1, std::forward<Backoff>(backoff)...))
      return false;
    value = std::move(slot.value);
    WakeTail(slot, head + mask_ + 1);
    return true;
  }

 private:
  struct alignas(ev::kCacheLineSize) Slot : public WaitType {
    ValueType value;
  };

  void Destroy() {
    if (ring_ != nullptr) {
      uint32_t size = mask_ + 1;
      for (uint32_t i = 0; i < size; i++) ring_[i].~Slot();
      free(ring_);
    }
  }

  void WaitTail(Slot& slot, uint64_t required_ticket) {
    uint32_t current_ticket = slot.Load();
    while (current_ticket != uint32_t(required_ticket)) {
      current_ticket = slot.WaitAndLoad(current_ticket);
    }
  }

  template <typename Backoff>
  void WaitTail(Slot& slot, uint64_t required_ticket, Backoff backoff) {
    bool waiting = false;
    uint32_t current_ticket = slot.Load();
    while (current_ticket != uint32_t(required_ticket)) {
      if (waiting) {
        current_ticket = slot.WaitAndLoad(current_ticket);
      } else {
        waiting = backoff();
        current_ticket = slot.Load();
      }
    }
  }

  bool WaitHead(Slot& slot, uint64_t required_ticket) {
    uint32_t current_ticket = slot.Load();
    while (current_ticket != uint32_t(required_ticket)) {
      if (Finished()) {
        uint64_t tail = tail_.load(std::memory_order_seq_cst);
        if (required_ticket >= tail) return false;
      }
      current_ticket = slot.WaitAndLoad(current_ticket);
    }
    return true;
  }

  template <typename Backoff>
  bool WaitHead(Slot& slot, uint64_t required_ticket, Backoff backoff) {
    bool waiting = false;
    uint32_t current_ticket = slot.Load();
    while (current_ticket != uint32_t(required_ticket)) {
      if (Finished()) {
        uint64_t tail = tail_.load(std::memory_order_seq_cst);
        if (required_ticket >= tail) return false;
      }
      if (waiting) {
        current_ticket = slot.WaitAndLoad(current_ticket);
      } else {
        waiting = backoff();
        current_ticket = slot.Load();
      }
    }
    return true;
  }

  void WakeHead(Slot& slot, uint32_t new_ticket) {
    slot.StoreAndWake(new_ticket);
  }

  void WakeTail(Slot& slot, uint32_t new_ticket) {
    slot.StoreAndWake(new_ticket);
  }

  Slot* ring_;
  const uint32_t mask_;

  std::atomic<bool> finish_;

  alignas(ev::kCacheLineSize) std::atomic<uint64_t> head_;
  alignas(ev::kCacheLineSize) std::atomic<uint64_t> tail_;
};

template <typename ValueType>
using DefaultBoundedQueue = BoundedQueue<ValueType, BoundedQueueNoWait>;

}  // namespace concurrency
}  // namespace ev

#endif  // EVENK_CONCURRENCY_H_
