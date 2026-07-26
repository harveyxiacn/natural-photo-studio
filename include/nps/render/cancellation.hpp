#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace nps::render {

// A small C++20 cancellation primitive for synchronous render/export APIs.
// Tokens share one monotonic atomic flag with their source. A default token is
// valid but cannot be cancelled and therefore never reports a stop request.
class CancellationToken final {
 public:
  CancellationToken() noexcept = default;

  [[nodiscard]] bool stop_requested() const noexcept {
    return state_ != nullptr &&
           state_->load(std::memory_order_acquire);
  }

 private:
  explicit CancellationToken(
      std::shared_ptr<const std::atomic_bool> state) noexcept
      : state_(std::move(state)) {}

  std::shared_ptr<const std::atomic_bool> state_;

  friend class CancellationSource;
};

class CancellationSource final {
 public:
  CancellationSource()
      : state_(std::make_shared<std::atomic_bool>(false)) {}

  [[nodiscard]] CancellationToken get_token() const noexcept {
    return CancellationToken{state_};
  }

  // Returns true only for the first request delivered to the shared state.
  bool request_stop() noexcept {
    return state_ != nullptr &&
           !state_->exchange(true, std::memory_order_acq_rel);
  }

 private:
  std::shared_ptr<std::atomic_bool> state_;
};

}  // namespace nps::render
