#pragma once

#include <stop_token>
#include <utility>

namespace weave {

class CancelToken {
  std::stop_token token_;

public:
  CancelToken() noexcept = default;

  explicit CancelToken(std::stop_token token) noexcept : token_(std::move(token))
  {
  }

  bool stop_requested() const noexcept
  {
    return token_.stop_requested();
  }

  bool stop_possible() const noexcept
  {
    return token_.stop_possible();
  }

  const std::stop_token &native_token() const noexcept
  {
    return token_;
  }
};

class CancelSource {
  std::stop_source source_;

public:
  CancelToken token() const noexcept
  {
    return CancelToken{source_.get_token()};
  }

  bool cancel() noexcept
  {
    return source_.request_stop();
  }

  bool stop_requested() const noexcept
  {
    return source_.stop_requested();
  }
};

struct SpawnOptions {
  CancelToken cancel;
};

struct CancellationPoint {};

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
