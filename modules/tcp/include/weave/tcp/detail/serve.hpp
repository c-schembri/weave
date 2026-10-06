#pragma once

#include <weave/tcp/listener.hpp>
#include <weave/scope.hpp>

namespace weave::detail {

template <class F>
concept TcpServeHandler = std::invocable<F &, TcpStream> &&
  std::same_as<std::invoke_result_t<F &, TcpStream>, Task<void>>;

template <TcpServeHandler F, ErrorObserver H>
struct TcpServeClient {
  F &handler;
  H &on_error;
  TcpStream client;

  Task<void> operator()()
  {
    auto result = co_await as_result(std::invoke(handler, std::move(client)));
    if (!result)
      std::invoke(on_error, std::as_const(result).error());
  }
};

template <TcpServeHandler F, ErrorObserver H>
struct TcpServeBody {
  TcpListener &listener;
  AcceptOptions options;
  F handler;
  H on_error;

  Task<void> operator()(TaskScope &children)
  {
    for (;;) {
      auto client = co_await listener.accept(options);
      auto job = children.spawn(TcpServeClient<F, H>{handler, on_error, std::move(client)});
      if (!job)
        co_await fail(job.error());
    }
  }
};

} // namespace weave::detail
