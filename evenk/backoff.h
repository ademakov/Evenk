//
// Busy-Waiting Backoff Utilities
//

#ifndef EVENK_BACKOFF_H_
#define EVENK_BACKOFF_H_

#include <atomic>
#include <cstdint>
#include <thread>

#include <time.h>
#include <emmintrin.h>

namespace ev {
namespace concurrency {

//
// Delays for busy waiting.
//

struct CPUCycle {
  void operator()(std::uint32_t n) {
    while (n--) std::atomic_signal_fence(std::memory_order_relaxed);
  }
};

struct CPURelax {
  void operator()(std::uint32_t n) {
    while (n--) ::_mm_pause();
  }
};

struct NanoSleep {
  void operator()(std::uint32_t n) {
    ::timespec ts = {.tv_sec = 0, .tv_nsec = n};
    ::nanosleep(&ts, NULL);
  }
};

//
// Back-off policies for busy waiting.
//

class NoBackoff {
 public:
  bool operator()() { return true; }
};

class YieldBackoff {
 public:
  bool operator()() {
    std::this_thread::yield();
    return false;
  }
};

template <typename Pause>
class LinearBackoff {
 public:
  LinearBackoff(std::uint32_t ceiling) noexcept : ceiling_{ceiling},
                                                  backoff_{0} {}

  bool operator()() {
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

  bool operator()() {
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

}  // namespace concurrency
}  // namespace ev

#endif  // !EVENK_BACKOFF_H_
