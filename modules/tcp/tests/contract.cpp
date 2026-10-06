#include <weave/tcp.hpp>
#include <cstdlib>
#include <string_view>

int main(int argc, char **argv)
{
#if defined(_MSC_VER)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  if (argc != 2)
    return 2;
  std::string_view mode = argv[1];
  if (mode == "detach") {
    weave::detach(weave::when_all());
    return 0;
  }
  if (mode.starts_with("wrong-")) {
    auto first = weave::Context::create();
    auto second = weave::Context::create();
    if (!first || !second)
      return 2;
    auto listener = weave::tcp::listen(*first, "127.0.0.1", 0);
    if (!listener)
      return 2;
    if (mode == "wrong-accept") {
      (void)second->run(listener->accept());
    } else if (mode == "wrong-connect") {
      (void)second->run(weave::tcp::connect(*first, "127.0.0.1", listener->local_port()));
    } else {
      auto client = first->run(weave::tcp::connect("127.0.0.1", listener->local_port()));
      if (!client)
        return 2;
      if (mode == "wrong-read")
        (void)second->run(client->read({}));
      else if (mode == "wrong-read-exactly")
        (void)second->run(client->read_exactly({}));
      else if (mode == "wrong-write")
        (void)second->run(client->write_all({}));
      else
        return 2;
    }
    return 0;
  }
  if (mode == "discard") {
    auto listener = weave::tcp::listen("127.0.0.1", 0);
    auto client = weave::tcp::connect("127.0.0.1", 80);
    return 0;
  }
  if (mode == "listen") {
    auto listener = weave::tcp::listen("127.0.0.1", 0);
    weave::detail::TaskAccess::start(listener);
  } else if (mode == "connect") {
    auto client = weave::tcp::connect("127.0.0.1", 80);
    weave::detail::TaskAccess::start(client);
  } else {
    return 2;
  }
  return 0;
}
