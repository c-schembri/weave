#pragma once

#include <stop_token>
#include <utility>

namespace weave {

/// Copyable observation of cooperative cancellation; does not stop or own an operation.
class CancelToken {
  std::stop_token token_;

public:
  /// Construct an empty token that cannot request or observe cancellation.
  CancelToken() noexcept = default;

  /// Retain a standard token's shared stop state; does not retain an operation or source object.
  explicit CancelToken(std::stop_token token) noexcept : token_(std::move(token))
  {
  }

  /// Whether cancellation was requested; not proof that an operation finished.
  bool stop_requested() const noexcept
  {
    return token_.stop_requested();
  }

  /// Whether cancellation was requested or a live source can still request it; empty tokens return false.
  bool stop_possible() const noexcept
  {
    return token_.stop_possible();
  }

  /// Borrow the standard token for callbacks; the reference lasts only as long as this token object.
  const std::stop_token &native_token() const noexcept
  {
    return token_;
  }
};

/// Shared, thread-safe cancellation source. Copies control the same state.
class CancelSource {
  std::stop_source source_;

public:
  /// Return an owning observation handle to this source's cancellation state.
  CancelToken token() const noexcept
  {
    return CancelToken{source_.get_token()};
  }

  /// Request cancellation; true only for the first request. Registered stop callbacks run synchronously.
  /// Does not join operations or force frame destruction.
  bool cancel() noexcept
  {
    return source_.request_stop();
  }

  /// Whether any copy of this source has requested cancellation.
  bool stop_requested() const noexcept
  {
    return source_.stop_requested();
  }
};

/// Cancellation for independent spawn/detach work; parent cancellation is not implicitly inherited.
struct SpawnOptions {
  CancelToken cancel;
};

/// Cooperative check marker produced by cancellation_point(); not an operation result or timer.
struct CancellationPoint {};

/// co_await to check the current Task token and fail with operation_canceled if requested.
/// This is a cooperative check, not a scheduler yield or a native-I/O completion barrier.
inline CancellationPoint cancellation_point() noexcept
{
  return {};
}

namespace detail {

struct GetCancellation {};

struct ForwardCancel {
  CancelSource source;

  void operator()() noexcept
  {
    source.cancel();
  }
};

using CancelLink = std::stop_callback<ForwardCancel>;

} // namespace detail

} // namespace weave
