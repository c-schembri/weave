#include <trantor/net/EventLoop.h>
#include <trantor/net/TcpServer.h>
#include <trantor/utils/Logger.h>
#include <weave/port.hpp>
#include <cstdio>
#include <exception>

static void echo(const trantor::TcpConnectionPtr &client, trantor::MsgBuffer *buffer)
{
  client->send(buffer->peek(), buffer->readableBytes());
  buffer->retrieveAll();
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "8080");
  if (argc < 1 || argc > 2 || !port) {
    std::fputs("Usage: echo_server [port: 0-65535]\n", stderr);
    return 2;
  }

  try {
    trantor::Logger::setLogLevel(trantor::Logger::kWarn);
    trantor::EventLoop loop;
    trantor::TcpServer server(&loop, trantor::InetAddress("127.0.0.1", *port), "echo", false, false);
    server.setConnectionCallback([](const trantor::TcpConnectionPtr &client) {
      if (client->connected())
        client->setTcpNoDelay(true);
    });
    server.setRecvMessageCallback(echo);
    server.start();

    std::printf("Listening on 127.0.0.1:%u\n", static_cast<unsigned>(server.address().toPort()));
    std::fflush(stdout);

    loop.loop();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Server: %s\n", error.what());
    return 1;
  }
}
