#pragma once
#include <mutex>

namespace aurora::vita::gfx {
// Reads completed frame data without queuing a callback or draining GX work.
// A short mutex protects the copy; a seqlock over ordinary fields would have
// a C++ data race even if a reader subsequently detected a changed sequence.
template <class T> class PublishedSnapshot {
public:
  void publish(const T& value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);value_=value;
  }
  T read() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);return value_;
  }
private:
  mutable std::mutex mutex_;
  T value_{};
};
} // namespace aurora::vita::gfx
