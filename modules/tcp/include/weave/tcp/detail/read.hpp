#pragma once

#include <weave/task.hpp>
#include <array>
#include <span>

namespace weave {

class TcpStream;

namespace detail {

// Shared by the public TCP Task and the inline TCP receive in TLS. Never move a published operation.
class TcpReadOperation {
public:
  enum class Cancellation {
    borrow,
    snapshot
  };

  TcpReadOperation(TcpStream &stream, std::span<std::byte> buffer, Cancellation policy) noexcept;
  TcpReadOperation(const TcpReadOperation &) = delete;
  TcpReadOperation(TcpReadOperation &&) = delete;
  TcpReadOperation &operator=(const TcpReadOperation &) = delete;
  TcpReadOperation &operator=(TcpReadOperation &&) = delete;
  ~TcpReadOperation();

  bool await_ready() const noexcept
  {
    return false;
  }

  template <class P>
  bool await_suspend(std::coroutine_handle<P> continuation) noexcept
  {
    require(!started_);
    started_ = true;

    if (policy_ == Cancellation::snapshot) {
      snapshot_ = continuation.promise().cancellation;
      return start(continuation, snapshot_);
    }
    return start(continuation, continuation.promise().cancellation);
  }

  Result<std::size_t> await_resume() noexcept;

private:
  bool start(std::coroutine_handle<> continuation, const CancelToken &cancellation) noexcept;

  // Both native backends enforce the measured capacity and alignment at compile time.
#ifdef _WIN32
  alignas(void *) std::array<std::byte, 320> storage_;
#else
  alignas(void *) std::array<std::byte, 448> storage_;
#endif
  TcpStream &stream_;
  std::span<std::byte> buffer_;
  Cancellation policy_;
  CancelToken snapshot_;
  Result<std::size_t> result_{std::size_t{0}};
  bool started_ = false;
  bool initialized_ = false;
  bool resumed_ = false;
};

} // namespace detail

} // namespace weave
