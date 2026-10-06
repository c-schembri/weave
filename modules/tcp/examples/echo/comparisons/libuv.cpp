#include <uv.h>
#include <weave/port.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>

struct Session {
  uv_tcp_t socket{};
  uv_write_t write{};
  std::array<char, 4096> buffer;

  uv_stream_t *stream()
  {
    return reinterpret_cast<uv_stream_t *>(&socket);
  }

  void close(int error = 0)
  {
    if (error < 0 && error != UV_EOF)
      std::fprintf(stderr, "Client: %s\n", uv_strerror(error));

    auto *handle = reinterpret_cast<uv_handle_t *>(&socket);
    if (!uv_is_closing(handle))
      uv_close(handle, [](uv_handle_t *handle) { delete static_cast<Session *>(handle->data); });
  }

  void read()
  {
    const int error = uv_read_start(stream(), allocate, on_read);
    if (error < 0)
      close(error);
  }

  static void allocate(uv_handle_t *handle, std::size_t, uv_buf_t *buffer)
  {
    auto &client = *static_cast<Session *>(handle->data);
    *buffer = uv_buf_init(client.buffer.data(), static_cast<unsigned>(client.buffer.size()));
  }

  static void on_read(uv_stream_t *stream, ssize_t received, const uv_buf_t *)
  {
    auto &client = *static_cast<Session *>(stream->data);
    if (received < 0) {
      client.close(static_cast<int>(received));
      return;
    }
    if (received == 0)
      return; // libuv's zero-byte callback is not EOF.

    // Keep the buffer unchanged until the write completes, then resume reading.
    uv_read_stop(stream);
    auto buffer = uv_buf_init(client.buffer.data(), static_cast<unsigned>(received));
    const int error = uv_write(&client.write, stream, &buffer, 1, on_write);
    if (error < 0)
      client.close(error);
  }

  static void on_write(uv_write_t *request, int error)
  {
    auto &client = *static_cast<Session *>(request->data);
    if (error < 0)
      client.close(error);
    else
      client.read();
  }
};

static void on_connection(uv_stream_t *listener, int status)
{
  if (status < 0) {
    std::fprintf(stderr, "Accept: %s\n", uv_strerror(status));
    std::exit(1);
  }
  auto *session = new (std::nothrow) Session;
  if (!session)
    std::abort();

  int error = uv_tcp_init(listener->loop, &session->socket);
  if (error < 0) {
    std::fprintf(stderr, "Socket: %s\n", uv_strerror(error));
    delete session;
    return;
  }
  session->socket.data = session->write.data = session;
  error = uv_accept(listener, session->stream());
  if (!error)
    error = uv_tcp_nodelay(&session->socket, 1);
  if (error < 0)
    session->close(error);
  else
    session->read();
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "8080");
  if (argc < 1 || argc > 2 || !port) {
    std::fputs("Usage: echo_server [port: 0-65535]\n", stderr);
    return 2;
  }

  uv_loop_t loop;
  int error = uv_loop_init(&loop);
  if (error < 0) {
    std::fprintf(stderr, "Loop: %s\n", uv_strerror(error));
    return 1;
  }

  uv_tcp_t listener;
  error = uv_tcp_init(&loop, &listener);
  if (error < 0) {
    uv_loop_close(&loop);
    std::fprintf(stderr, "Socket: %s\n", uv_strerror(error));
    return 1;
  }

  sockaddr_in address{};
  error = uv_ip4_addr("127.0.0.1", *port, &address);
  if (!error)
    error = uv_tcp_bind(&listener, reinterpret_cast<const sockaddr *>(&address), 0);
  if (!error)
    error = uv_listen(reinterpret_cast<uv_stream_t *>(&listener), 512, on_connection);
  int length = sizeof(address);
  if (!error)
    error = uv_tcp_getsockname(&listener, reinterpret_cast<sockaddr *>(&address), &length);
  if (error < 0) {
    uv_close(reinterpret_cast<uv_handle_t *>(&listener), nullptr);
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
    std::fprintf(stderr, "Listen: %s\n", uv_strerror(error));
    return 1;
  }

  std::printf("Listening on 127.0.0.1:%u\n", static_cast<unsigned>(ntohs(address.sin_port)));
  std::fflush(stdout);

  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);
}
