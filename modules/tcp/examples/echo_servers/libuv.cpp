#include <uv.h>
#include "common.hpp"
#include <array>
#include <cstdlib>
#include <new>

struct Session {
  uv_tcp_t socket{};
  uv_write_t write{};
  uv_shutdown_t shutdown{};
  std::array<char, 4096> buffer;

  void close()
  {
    if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&socket))) {
      uv_close(reinterpret_cast<uv_handle_t *>(&socket), [](uv_handle_t *handle) {
        delete static_cast<Session *>(handle->data);
      });
    }
  }

  void read()
  {
    const int error = uv_read_start(
      reinterpret_cast<uv_stream_t *>(&socket),
      [](uv_handle_t *handle, std::size_t, uv_buf_t *buffer) {
        auto &session = *static_cast<Session *>(handle->data);
        *buffer = uv_buf_init(session.buffer.data(), static_cast<unsigned>(session.buffer.size()));
      },
      [](uv_stream_t *stream, ssize_t size, const uv_buf_t *) {
        auto &session = *static_cast<Session *>(stream->data);
        if (size == UV_EOF) {
          const int error = uv_shutdown(&session.shutdown, stream, [](uv_shutdown_t *request, int status) {
            if (status < 0)
              example::fail("Shutdown", uv_strerror(status));
            static_cast<Session *>(request->data)->close();
          });
          if (error < 0) {
            example::fail("Shutdown", uv_strerror(error));
            session.close();
          }
        } else if (size < 0) {
          example::fail("Read", uv_strerror(static_cast<int>(size)));
          session.close();
        } else if (size > 0) {
          // Keep this buffer unchanged until the write callback, then resume reading.
          uv_read_stop(stream);
          auto buffer = uv_buf_init(session.buffer.data(), static_cast<unsigned>(size));
          const int error = uv_write(&session.write, stream, &buffer, 1, [](uv_write_t *request, int status) {
            auto &session = *static_cast<Session *>(request->data);
            if (status < 0) {
              example::fail("Write", uv_strerror(status));
              session.close();
            } else
              session.read();
          });
          if (error < 0) {
            example::fail("Write", uv_strerror(error));
            session.close();
          }
        }
      });
    if (error < 0) {
      example::fail("Read", uv_strerror(error));
      close();
    }
  }
};

void accept(uv_stream_t *listener, int status)
{
  if (status < 0) {
    example::fail("Accept", uv_strerror(status));
    std::exit(1);
  }
  auto *session = new (std::nothrow) Session;
  if (!session)
    std::abort();
  int error = uv_tcp_init(listener->loop, &session->socket);
  if (error < 0) {
    example::fail("Socket", uv_strerror(error));
    delete session;
    return;
  }
  session->socket.data = session->write.data = session->shutdown.data = session;
  error = uv_accept(listener, reinterpret_cast<uv_stream_t *>(&session->socket));
  if (!error)
    error = uv_tcp_nodelay(&session->socket, 1);
  if (error < 0) {
    example::fail("Accept/TCP_NODELAY", uv_strerror(error));
    session->close();
  } else
    session->read();
}

int main(int argc, char **argv)
{
  auto port = example::port(argc, argv);
  if (!port)
    return 2;
  uv_loop_t loop;
  int error = uv_loop_init(&loop);
  if (error < 0)
    return example::fail("Loop", uv_strerror(error));
  uv_tcp_t listener;
  error = uv_tcp_init(&loop, &listener);
  if (error < 0) {
    uv_loop_close(&loop);
    return example::fail("Socket", uv_strerror(error));
  }
  sockaddr_in address{};
  error = uv_ip4_addr("127.0.0.1", *port, &address);
  if (!error)
    error = uv_tcp_bind(&listener, reinterpret_cast<const sockaddr *>(&address), 0);
  if (!error)
    error = uv_listen(reinterpret_cast<uv_stream_t *>(&listener), 512, accept);
  int length = sizeof(address);
  if (!error)
    error = uv_tcp_getsockname(&listener, reinterpret_cast<sockaddr *>(&address), &length);
  if (error < 0) {
    uv_close(reinterpret_cast<uv_handle_t *>(&listener), nullptr);
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
    return example::fail("Listen", uv_strerror(error));
  }
  example::listening(ntohs(address.sin_port));
  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);
}
