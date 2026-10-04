#pragma once
#include "callback_loop.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace support {

// Only protocol bookkeeping is shared. The transports use their native C APIs,
// not Weave/Asio coroutines or a replacement socket engine.
template <class Derived>
class EchoTransfer {
protected:
  CallbackLoop &loop_;
  Callback done_;
  std::size_t sent_ = 0, received_ = 0, rounds_ = 0;
  bool active_ = false, connected_ = false, closed_ = true, validate_each_ = false;
  int error_ = 0;

  Derived &self()
  {
    return static_cast<Derived &>(*this);
  }

  void finish(bool ok)
  {
    active_ = false;
    std::exchange(done_, {})(ok);
  }

  void begin_connect(Callback done)
  {
    weave::detail::require(closed_ && !active_);
    done_ = done;
    error_ = 0;
    closed_ = false;
  }

  void opened()
  {
    connected_ = true;
    finish(true);
  }

  void closed()
  {
    connected_ = false;
    closed_ = true;
    finish(false);
  }

  void fail(int error)
  {
    if (!error_)
      error_ = error;
    self().close();
  }

  void progress()
  {
    if (!active_ || sent_ != tx.size() || received_ != rx.size())
      return;
    if (validate_each_ && tx != rx) {
      fail(UV_EIO);
      return;
    }
    if (--rounds_ == 0) {
      finish(true);
      return;
    }
    sent_ = received_ = 0;
    self().write();
  }

  void received(std::size_t size)
  {
    if (!active_ || size > rx.size() - received_) {
      fail(UV_EPROTO);
      return;
    }
    received_ += size;
    progress();
  }

public:
  std::vector<std::byte> tx, rx;

  EchoTransfer(CallbackLoop &loop, std::size_t size) : loop_(loop), tx(size), rx(size)
  {
  }

  EchoTransfer(const EchoTransfer &) = delete;
  EchoTransfer &operator=(const EchoTransfer &) = delete;

  bool is_closed() const
  {
    return closed_;
  }

  int error() const
  {
    return error_;
  }

  void exchange(std::size_t rounds, bool validate_each, Callback done)
  {
    weave::detail::require(!active_);
    if (!connected_) {
      done(false);
      return;
    }
    done_ = done;
    active_ = true;
    rounds_ = rounds;
    validate_each_ = validate_each;
    sent_ = received_ = 0;
    if (rounds == 0 || tx.empty())
      finish(true);
    else
      self().write();
  }
};

class LibuvClient : public EchoTransfer<LibuvClient> {
  friend class EchoTransfer<LibuvClient>;
  uv_tcp_t socket_{};
  uv_connect_t connect_{};
  uv_write_t write_{};
  bool initialized_ = false;
  std::size_t queued_bytes_ = 0;
  char idle_byte_{};

  void write()
  {
    while (sent_ < tx.size()) {
      auto size = static_cast<unsigned int>((std::min)(tx.size() - sent_, std::size_t{65536}));
      auto buffer = uv_buf_init(reinterpret_cast<char *>(tx.data() + sent_), size);
      int sent = uv_try_write(reinterpret_cast<uv_stream_t *>(&socket_), &buffer, 1);
      if (sent < 0 && sent != UV_EAGAIN && sent != UV_ENOSYS) {
        fail(sent);
        return;
      }
      if (sent > 0)
        sent_ += static_cast<std::size_t>(sent);
      if (sent == static_cast<int>(size))
        continue;
      queued_bytes_ = size - (sent > 0 ? static_cast<std::size_t>(sent) : 0);
      buffer = uv_buf_init(reinterpret_cast<char *>(tx.data() + sent_), static_cast<unsigned int>(queued_bytes_));
      const int error = uv_write(
        &write_,
        reinterpret_cast<uv_stream_t *>(&socket_),
        &buffer,
        1,
        [](uv_write_t *request, int status) {
          auto &client = *static_cast<LibuvClient *>(request->data);
          if (uv_is_closing(reinterpret_cast<uv_handle_t *>(&client.socket_)))
            return;
          if (status < 0) {
            client.fail(status);
            return;
          }
          client.sent_ += client.queued_bytes_;
          client.write();
        });
      if (error < 0)
        fail(error);
      return;
    }
    progress();
  }

public:
  static constexpr auto backend = CallbackBackend::libuv;
  using EchoTransfer::EchoTransfer;

  void connect(std::uint16_t port, Callback done)
  {
    begin_connect(done);
    int error = uv_tcp_init(loop_.native(), &socket_);
    if (error < 0) {
      fail(error);
      return;
    }
    initialized_ = true;
    socket_.data = connect_.data = write_.data = this;
    sockaddr_in address{};
    error = uv_ip4_addr("127.0.0.1", port, &address);
    if (!error) {
      error = uv_tcp_connect(
        &connect_,
        &socket_,
        reinterpret_cast<sockaddr *>(&address),
        [](uv_connect_t *request, int status) {
          auto &client = *static_cast<LibuvClient *>(request->data);
          if (uv_is_closing(reinterpret_cast<uv_handle_t *>(&client.socket_)))
            return;
          if (!status)
            status = uv_tcp_nodelay(&client.socket_, 1);
          if (!status) {
            status = uv_read_start(
              reinterpret_cast<uv_stream_t *>(&client.socket_),
              [](uv_handle_t *handle, std::size_t, uv_buf_t *buffer) {
                auto &c = *static_cast<LibuvClient *>(handle->data);
                if (c.active_ && c.received_ < c.rx.size()) {
                  *buffer = uv_buf_init(
                    reinterpret_cast<char *>(c.rx.data() + c.received_),
                    static_cast<unsigned int>((std::min)(c.rx.size() - c.received_, std::size_t{65536})));
                } else {
                  *buffer = uv_buf_init(&c.idle_byte_, 1);
                }
              },
              [](uv_stream_t *handle, ssize_t size, const uv_buf_t *) {
                auto &c = *static_cast<LibuvClient *>(handle->data);
                if (size < 0)
                  c.fail(static_cast<int>(size));
                else if (size > 0)
                  c.received(static_cast<std::size_t>(size));
              });
          }
          if (status < 0)
            client.fail(status);
          else
            client.opened();
        });
    }
    if (error < 0)
      fail(error);
  }

  void close()
  {
    if (!initialized_) {
      closed();
      return;
    }
    if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&socket_))) {
      uv_close(reinterpret_cast<uv_handle_t *>(&socket_), [](uv_handle_t *handle) {
        auto &client = *static_cast<LibuvClient *>(handle->data);
        client.initialized_ = false;
        client.closed();
      });
    }
  }

  bool send_buffer(int bytes)
  {
    return uv_send_buffer_size(reinterpret_cast<uv_handle_t *>(&socket_), &bytes) == 0;
  }
};

class UsocketsClient : public EchoTransfer<UsocketsClient> {
  friend class EchoTransfer<UsocketsClient>;
  us_socket_t *socket_ = nullptr;

  static UsocketsClient &get(us_socket_t *socket)
  {
    return **static_cast<UsocketsClient **>(us_socket_ext(0, socket));
  }

  SOCKET native() const
  {
    return static_cast<SOCKET>(reinterpret_cast<std::uintptr_t>(us_socket_get_native_handle(0, socket_)));
  }

  void write()
  {
    while (sent_ < tx.size()) {
      const int size = static_cast<int>((std::min)(tx.size() - sent_, std::size_t{65536}));
      const int sent = us_socket_write(0, socket_, reinterpret_cast<const char *>(tx.data() + sent_), size, 0);
      sent_ += static_cast<std::size_t>(sent);
      if (sent != size)
        return;
    }
    progress();
  }

public:
  static constexpr auto backend = CallbackBackend::usockets;

  UsocketsClient(CallbackLoop &loop, std::size_t size) : EchoTransfer(loop, size)
  {
    auto *context = loop.context();
    us_socket_context_on_open(0, context, [](us_socket_t *socket, int, char *, int) {
      auto &client = get(socket);
      // Windows readiness may report writable on a failed connect. Validate it
      // before publishing success; all connection setup is outside timing.
      int pending_error = 0, length = sizeof(pending_error);
      if (getsockopt(client.native(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&pending_error), &length))
        pending_error = WSAGetLastError();
      sockaddr_storage remote{};
      length = sizeof(remote);
      if (!pending_error && getpeername(client.native(), reinterpret_cast<sockaddr *>(&remote), &length))
        pending_error = WSAGetLastError();
      if (pending_error) {
        client.fail(uv_translate_sys_error(pending_error));
        return socket;
      }
      const int yes = 1;
      if (setsockopt(client.native(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&yes), sizeof(yes)))
        client.fail(uv_translate_sys_error(WSAGetLastError()));
      else
        client.opened();
      return socket;
    });
    us_socket_context_on_data(0, context, [](us_socket_t *socket, char *data, int length) {
      auto &client = get(socket);
      const auto size = static_cast<std::size_t>(length);
      if (!client.active_ || size > client.rx.size() - client.received_)
        client.fail(UV_EPROTO);
      else {
        std::memcpy(client.rx.data() + client.received_, data, size);
        client.received(size);
      }
      return socket;
    });
    us_socket_context_on_writable(0, context, [](us_socket_t *socket) {
      auto &client = get(socket);
      if (client.active_)
        client.write();
      return socket;
    });
    us_socket_context_on_close(0, context, [](us_socket_t *socket, int, void *) {
      auto &client = get(socket);
      client.socket_ = nullptr;
      client.closed();
      return socket;
    });
    us_socket_context_on_end(0, context, [](us_socket_t *socket) {
      get(socket).fail(UV_EOF);
      return socket;
    });
    us_socket_context_on_connect_error(0, context, [](us_socket_t *socket, int) {
      auto &client = get(socket);
      // This upstream backend supplies no detailed connect error (code is zero).
      client.error_ = UV_ECONNREFUSED;
      client.socket_ = nullptr;
      client.closed();
      return socket;
    });
  }

  void connect(std::uint16_t port, Callback done)
  {
    begin_connect(done);
    socket_ = us_socket_context_connect(0, loop_.context(), "127.0.0.1", port, nullptr, 0, sizeof(UsocketsClient *));
    if (!socket_) {
      fail(UV_ECONNREFUSED);
      return;
    }
    *static_cast<UsocketsClient **>(us_socket_ext(0, socket_)) = this;
  }

  void close()
  {
    if (!socket_)
      closed();
    else if (!us_socket_is_established(0, socket_)) {
      us_socket_close_connecting(0, socket_);
      socket_ = nullptr;
      closed();
    } else
      us_socket_close(0, socket_, 0, nullptr);
  }

  bool send_buffer(int bytes)
  {
    return setsockopt(native(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&bytes), sizeof(bytes)) == 0;
  }
};

} // namespace support
