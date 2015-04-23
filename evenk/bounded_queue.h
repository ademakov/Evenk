//
// Fast Bounded Concurrent Queue
//

#ifndef EVENK_BOUNDED_QUEUE_H_
#define EVENK_BOUNDED_QUEUE_H_

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#include "evenk/backoff.h"
#include "evenk/basic.h"
#include "evenk/futex.h"
#include "evenk/synch.h"

namespace ev {
namespace concurrency {

struct BoundedQueueSlotBase {
 public:
  void Initialize(std::uint32_t value) {
    ticket_.store(value, std::memory_order_relaxed);
  }

  std::uint32_t Load() const { return ticket_.load(std::memory_order_acquire); }

  void Store(std::uint32_t value) {
    ticket_.store(value, std::memory_order_release);
  }

 protected:
  std::atomic<std::uint32_t> ticket_;
};

class BoundedQueueNoWait : public BoundedQueueSlotBase {
 public:
  std::uint32_t WaitAndLoad(std::uint32_t) { return Load(); }

  void StoreAndWake(std::uint32_t value) { Store(value); }

  void Wake() {}
};

class BoundedQueueYieldWait : public BoundedQueueSlotBase {
 public:
  std::uint32_t WaitAndLoad(std::uint32_t) {
    std::this_thread::yield();
    return Load();
  }

  void StoreAndWake(std::uint32_t value) { Store(value); }

  void Wake() {}
};

class BoundedQueueFutexWait : public BoundedQueueSlotBase {
 public:
  std::uint32_t WaitAndLoad(std::uint32_t value) {
    wait_count_.fetch_add(1, std::memory_order_relaxed);
    ev::futex_wait(ticket_, value);  // Presuming this is a full memory fence.
    wait_count_.fetch_sub(1, std::memory_order_relaxed);
    return Load();
  }

  void StoreAndWake(std::uint32_t value) {
    Store(value);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (wait_count_.load(std::memory_order_relaxed)) Wake();
  }

  void Wake() { futex_wake(ticket_, INT32_MAX); }

 private:
  std::atomic<std::uint32_t> wait_count_ = ATOMIC_VAR_INIT(0);
};

template <typename Synch = DefaultSynch>
class BoundedQueueSynchWait : public BoundedQueueSlotBase {
 public:
  std::uint32_t WaitAndLoad(std::uint32_t value) {
    LockGuard<LockType> guard(lock_);
    std::uint32_t current_value = ticket_.load(std::memory_order_relaxed);
    if (current_value == value) {
      cond_.Wait(guard);
      current_value = ticket_.load(std::memory_order_relaxed);
    }
    return current_value;
  }

  void StoreAndWake(std::uint32_t value) {
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
  BoundedQueue(std::uint32_t size)
      : ring_{nullptr}, mask_{size - 1}, finish_{false}, head_{0}, tail_{0} {
    if (size == 0 || (size & mask_) != 0)
      throw std::invalid_argument("BoundedQueue size must be a power of two");

    void* ring;
    if (::posix_memalign(&ring, ev::kCacheLineSize, size * sizeof(Slot)))
      throw std::bad_alloc();

    ring_ = new (ring) Slot[size];
    for (std::uint32_t i = 0; i < size; i++) ring_[i].Initialize(i);
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
    for (std::uint32_t i = 0; i < mask_ + 1; i++) ring_[i].Wake();
  }

  template <typename... Backoff>
  void Enqueue(ValueType&& value, Backoff... backoff) {
    const std::uint64_t tail = tail_.fetch_add(1, std::memory_order_seq_cst);
    Slot& slot = ring_[tail & mask_];
    WaitTail(slot, tail, std::forward<Backoff>(backoff)...);
    slot.value = std::move(value);
    WakeHead(slot, tail + 1);
  }

  template <typename... Backoff>
  void Enqueue(const ValueType& value, Backoff... backoff) {
    const std::uint64_t tail = tail_.fetch_add(1, std::memory_order_seq_cst);
    Slot& slot = ring_[tail & mask_];
    WaitTail(slot, tail, std::forward<Backoff>(backoff)...);
    slot.value = value;
    WakeHead(slot, tail + 1);
  }

  template <typename... Backoff>
  bool Dequeue(ValueType& value, Backoff... backoff) {
    const std::uint64_t head = head_.fetch_add(1, std::memory_order_relaxed);
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
      std::uint32_t size = mask_ + 1;
      for (std::uint32_t i = 0; i < size; i++) ring_[i].~Slot();
      std::free(ring_);
    }
  }

  void WaitTail(Slot& slot, std::uint64_t required_ticket) {
    std::uint32_t current_ticket = slot.Load();
    while (current_ticket != std::uint32_t(required_ticket)) {
      current_ticket = slot.WaitAndLoad(current_ticket);
    }
  }

  template <typename Backoff>
  void WaitTail(Slot& slot, std::uint64_t required_ticket, Backoff backoff) {
    bool waiting = false;
    std::uint32_t current_ticket = slot.Load();
    while (current_ticket != std::uint32_t(required_ticket)) {
      if (waiting) {
        current_ticket = slot.WaitAndLoad(current_ticket);
      } else {
        waiting = backoff();
        current_ticket = slot.Load();
      }
    }
  }

  bool WaitHead(Slot& slot, std::uint64_t required_ticket) {
    std::uint32_t current_ticket = slot.Load();
    while (current_ticket != std::uint32_t(required_ticket)) {
      if (Finished()) {
        std::uint64_t tail = tail_.load(std::memory_order_seq_cst);
        if (required_ticket >= tail) return false;
      }
      current_ticket = slot.WaitAndLoad(current_ticket);
    }
    return true;
  }

  template <typename Backoff>
  bool WaitHead(Slot& slot, std::uint64_t required_ticket, Backoff backoff) {
    bool waiting = false;
    std::uint32_t current_ticket = slot.Load();
    while (current_ticket != std::uint32_t(required_ticket)) {
      if (Finished()) {
        std::uint64_t tail = tail_.load(std::memory_order_seq_cst);
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

  void WakeHead(Slot& slot, std::uint32_t new_ticket) {
    slot.StoreAndWake(new_ticket);
  }

  void WakeTail(Slot& slot, std::uint32_t new_ticket) {
    slot.StoreAndWake(new_ticket);
  }

  Slot* ring_;
  const std::uint32_t mask_;

  std::atomic<bool> finish_;

  alignas(ev::kCacheLineSize) std::atomic<std::uint64_t> head_;
  alignas(ev::kCacheLineSize) std::atomic<std::uint64_t> tail_;
};

template <typename ValueType>
using DefaultBoundedQueue = BoundedQueue<ValueType, BoundedQueueNoWait>;

}  // namespace concurrency
}  // namespace ev

#endif  // !EVENK_BOUNDED_QUEUE_H_
