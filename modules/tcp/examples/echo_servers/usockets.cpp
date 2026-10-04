#include <libusockets.h>
#include "common.hpp"
#include <algorithm>
#include <memory>
#include <vector>

constexpr int ssl = 0;

struct Session {
  std::vector<char> pending;
  std::size_t offset = 0;
  bool eof = false;
};

Session &session(us_socket_t *socket)
{
  return *static_cast<Session *>(us_socket_ext(ssl, socket));
}

us_socket_t *flush(us_socket_t *socket)
{
  auto &state = session(socket);
  while (state.offset < state.pending.size()) {
    const int size = static_cast<int>((std::min)(state.pending.size() - state.offset, std::size_t{65536}));
    const int sent = us_socket_write(ssl, socket, state.pending.data() + state.offset, size, 0);
    state.offset += static_cast<std::size_t>(sent);
    if (sent < size)
      return socket; // on_writable resumes the unsent tail.
  }
  state.pending.clear();
  state.offset = 0;
  if (state.eof) {
    us_socket_shutdown(ssl, socket);
    return us_socket_close(ssl, socket, 0, nullptr);
  }
  return socket;
}

int main(int argc, char **argv)
{
  auto port = example::port(argc, argv);
  if (!port)
    return 2;
  auto noop = [](us_loop_t *) {};
  auto *loop = us_create_loop(nullptr, noop, noop, noop, 0);
  if (!loop)
    return example::fail("Loop", "Creation failed");
  auto *context = us_create_socket_context(ssl, loop, 0, {});
  if (!context) {
    us_loop_free(loop);
    return example::fail("Context", "Creation failed");
  }

  us_socket_context_on_open(ssl, context, [](us_socket_t *socket, int, char *, int) {
    std::construct_at(static_cast<Session *>(us_socket_ext(ssl, socket)));
    // uSockets enables TCP_NODELAY on accepted sockets itself.
    return socket;
  });
  us_socket_context_on_data(ssl, context, [](us_socket_t *socket, char *data, int size) {
    auto &state = session(socket);
    if (state.pending.empty()) {
      const int sent = us_socket_write(ssl, socket, data, size, 0);
      data += sent;
      size -= sent;
    }
    // Never send newer data ahead of an older unsent tail. The input buffer is borrowed.
    state.pending.insert(state.pending.end(), data, data + size);
    return socket;
  });
  us_socket_context_on_writable(ssl, context, flush);
  us_socket_context_on_end(ssl, context, [](us_socket_t *socket) {
    session(socket).eof = true;
    return flush(socket); // Finish buffered echoes before replying to the client's FIN.
  });
  us_socket_context_on_close(ssl, context, [](us_socket_t *socket, int, void *) {
    std::destroy_at(&session(socket));
    return socket;
  });

  auto *
    listener = us_socket_context_listen(ssl, context, "127.0.0.1", *port, LIBUS_LISTEN_EXCLUSIVE_PORT, sizeof(Session));
  if (!listener) {
    us_socket_context_free(ssl, context);
    us_loop_free(loop);
    return example::fail("Listen", "Bind/listen failed");
  }
  const int bound_port = us_socket_local_port(ssl, reinterpret_cast<us_socket_t *>(listener));
  if (bound_port < 0) {
    us_listen_socket_close(ssl, listener);
    us_loop_run(loop);
    us_socket_context_free(ssl, context);
    us_loop_free(loop);
    return example::fail("Local port", "Lookup failed");
  }
  example::listening(static_cast<unsigned>(bound_port));
  us_loop_run(loop);
  us_socket_context_free(ssl, context);
  us_loop_free(loop);
}
