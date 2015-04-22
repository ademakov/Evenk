//
// Simple Concurrent Queue
//

#ifndef EVENK_QUEUE_H_
#define EVENK_QUEUE_H_

#include <deque>

#include "evenk/concurrency.h"

namespace ev {
namespace concurrency {

template <typename ValueType, typename SynchPolicy = DefaultSynch,
          typename Sequence = std::deque<ValueType>>
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
  Sequence queue_;
};

}  // namespace concurrency
}  // namespace ev

#endif  // !EVENK_QUEUE_H_
