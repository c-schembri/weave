#pragma once
#include "asio_config.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace support {

// A bounded, identical peer for both clients. Each shard owns its listener,
// sockets, and one thread; connection count does not increase the thread count.
class AsyncEchoPeer {
  using tcp = asio::ip::tcp;
  static constexpr auto use_result = asio::as_tuple(asio::use_awaitable);

  struct Session {
    tcp::socket socket;
    std::array<char, 4096> buffer{};

    explicit Session(asio::io_context &context) : socket(context)
    {
    }
  };

  struct Shard {
    asio::io_context context{1};
    tcp::acceptor listener{context};
    asio::executor_work_guard<asio::io_context::executor_type> guard{context.get_executor()};
    std::vector<std::unique_ptr<Session>> sessions;
    std::thread thread;
    std::uint16_t port = 0;
    bool stopping = false;
  };

  std::vector<std::unique_ptr<Shard>> shards_;
  mutable std::mutex mutex_;
  std::condition_variable progress_;
  asio::error_code error_;
  std::size_t accepted_ = 0, active_ = 0;
  bool stopped_ = false;

  static void completed(std::exception_ptr error)
  {
    if (error)
      std::abort();
  }

  void fail(asio::error_code error)
  {
    std::lock_guard lock(mutex_);
    if (!error_)
      error_ = error;
    progress_.notify_all();
  }

  asio::awaitable<void> echo(Shard &shard, Session &session)
  {
    for (;;) {
      auto [read_error, size] = co_await session.socket.async_read_some(asio::buffer(session.buffer), use_result);
      if (read_error) {
        if (read_error != asio::error::eof && !shard.stopping)
          fail(read_error);
        break;
      }
      auto [write_error, sent] = co_await asio::async_write(
        session.socket,
        asio::buffer(session.buffer.data(), size),
        use_result);
      if (write_error || sent != size) {
        if (!shard.stopping)
          fail(write_error ? write_error : asio::error::fault);
        break;
      }
    }
    asio::error_code ignored;
    session.socket.close(ignored);
    std::lock_guard lock(mutex_);
    --active_;
    progress_.notify_all();
  }

  asio::awaitable<void> accept(Shard &shard)
  {
    while (!shard.stopping) {
      auto session = std::make_unique<Session>(shard.context);
      auto [error] = co_await shard.listener.async_accept(session->socket, use_result);
      if (shard.stopping)
        co_return;
      if (error) {
        fail(error);
        co_return;
      }
      session->socket.set_option(tcp::no_delay(true), error);
      if (error) {
        fail(error);
        co_return;
      }
      auto &stored = *session;
      shard.sessions.push_back(std::move(session));
      {
        std::lock_guard lock(mutex_);
        ++accepted_;
        ++active_;
        progress_.notify_all();
      }
      asio::co_spawn(shard.context, echo(shard, stored), completed);
    }
  }

public:
  explicit AsyncEchoPeer(std::size_t workers)
  {
    if (workers == 0) {
      error_ = asio::error::invalid_argument;
      return;
    }
    for (std::size_t i = 0; i < workers; ++i) {
      auto shard = std::make_unique<Shard>();
      shard->listener.open(tcp::v4(), error_);
      if (error_)
        return;
      shard->listener.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0), error_);
      if (error_)
        return;
      shard->listener.listen(asio::socket_base::max_listen_connections, error_);
      if (error_)
        return;
      shard->port = shard->listener.local_endpoint(error_).port();
      if (error_)
        return;
      shards_.push_back(std::move(shard));
    }
    for (auto &shard : shards_) {
      asio::co_spawn(shard->context, accept(*shard), completed);
      shard->thread = std::thread([ptr = shard.get()] { ptr->context.run(); });
    }
  }

  ~AsyncEchoPeer()
  {
    stop();
  }

  std::uint16_t port(std::size_t connection) const
  {
    return shards_[connection % shards_.size()]->port;
  }

  asio::error_code error() const
  {
    std::lock_guard lock(mutex_);
    return error_;
  }

  bool wait_connected(std::size_t count)
  {
    std::unique_lock lock(mutex_);
    return progress_.wait_for(lock, std::chrono::seconds(10), [&] { return error_ || accepted_ >= count; }) &&
      !error_ && accepted_ == count && active_ == count;
  }

  bool wait_idle()
  {
    std::unique_lock lock(mutex_);
    return progress_.wait_for(lock, std::chrono::seconds(10), [&] { return error_ || active_ == 0; }) && !error_ &&
      active_ == 0;
  }

  void stop()
  {
    if (stopped_)
      return;
    stopped_ = true;
    for (auto &shard : shards_) {
      if (!shard->thread.joinable())
        continue;
      asio::post(shard->context, [ptr = shard.get()] {
        ptr->stopping = true;
        asio::error_code ignored;
        ptr->listener.close(ignored);
        for (auto &session : ptr->sessions)
          session->socket.close(ignored);
        ptr->guard.reset();
      });
    }
    for (auto &shard : shards_) {
      if (shard->thread.joinable())
        shard->thread.join();
    }
  }
};

} // namespace support
