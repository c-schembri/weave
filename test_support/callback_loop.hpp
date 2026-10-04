#pragma once
#include <weave/core.hpp>
#include <uv.h>
#include <libusockets.h>
#include <mutex>
#include <utility>

namespace support {

enum class CallbackBackend {
  libuv,
  usockets
};

struct Callback {
  void *data = nullptr;
  void (*invoke)(void *, bool) = nullptr;

  void operator()(bool ok) const
  {
    if (invoke)
      invoke(data, ok);
  }
};

// Benchmark-only cross-thread submission. One node per submitted job, with
// the native async wakeup allowed to coalesce notifications, not jobs.
class CallbackLoop {
public:
  struct Command {
    void *data = nullptr;
    void (*invoke)(void *) = nullptr;
    Command *next = nullptr;
  };

private:
  uv_loop_t loop_{};
  uv_async_t wakeup_{};
  us_loop_t *us_loop_ = nullptr;
  us_socket_context_t *us_context_ = nullptr;
  std::mutex mutex_;
  Command *head_ = nullptr, *tail_ = nullptr;
  bool wakeup_closed_ = false;

  void dispatch()
  {
    Command *command;
    {
      std::lock_guard lock(mutex_);
      command = std::exchange(head_, nullptr);
      tail_ = nullptr;
    }
    while (command) {
      // A completion can publish its result and allow reuse of this node.
      auto *next = command->next;
      command->invoke(command->data);
      command = next;
    }
  }

public:
  explicit CallbackLoop(CallbackBackend backend)
  {
    weave::detail::require(uv_loop_init(&loop_) == 0);
    weave::detail::require(uv_async_init(&loop_, &wakeup_, [](uv_async_t *handle) {
      static_cast<CallbackLoop *>(handle->data)->dispatch();
    }) == 0);
    wakeup_.data = this;
    if (backend == CallbackBackend::usockets) {
      auto noop = [](us_loop_t *) {};
      us_loop_ = us_create_loop(&loop_, noop, noop, noop, 0);
      weave::detail::require(us_loop_ != nullptr);
      us_context_ = us_create_socket_context(0, us_loop_, 0, {});
      weave::detail::require(us_context_ != nullptr);
    }
  }

  ~CallbackLoop()
  {
    // Clients have been closed and callbacks drained before their storage dies.
    if (us_loop_) {
      uv_run(&loop_, UV_RUN_NOWAIT);
      us_socket_context_free(0, us_context_);
      us_loop_free(us_loop_);
    }
    close_wakeup();
    uv_run(&loop_, UV_RUN_DEFAULT);
    weave::detail::require(uv_loop_close(&loop_) == 0);
  }

  CallbackLoop(const CallbackLoop &) = delete;
  CallbackLoop &operator=(const CallbackLoop &) = delete;

  uv_loop_t *native()
  {
    return &loop_;
  }

  us_socket_context_t *context()
  {
    return us_context_;
  }

  void post(Command &command)
  {
    command.next = nullptr;
    {
      std::lock_guard lock(mutex_);
      if (tail_)
        tail_->next = &command;
      else
        head_ = &command;
      tail_ = &command;
    }
    weave::detail::require(uv_async_send(&wakeup_) == 0);
  }

  void run()
  {
    uv_run(&loop_, UV_RUN_DEFAULT);
  }

  void once()
  {
    uv_run(&loop_, UV_RUN_ONCE);
  }

  void stop()
  {
    uv_stop(&loop_);
  }

  void close_wakeup()
  {
    if (!std::exchange(wakeup_closed_, true))
      uv_close(reinterpret_cast<uv_handle_t *>(&wakeup_), nullptr);
  }
};

} // namespace support
