#pragma once
#include "callback_clients.hpp"
#include <future>
#include <memory>
#include <optional>
#include <thread>

namespace support {

template <class Client>
class CallbackGroup {
public:
  CallbackLoop loop{Client::backend};
  std::vector<std::unique_ptr<Client>> clients;

  CallbackGroup(std::size_t count, std::size_t size)
  {
    for (std::size_t i = 0; i < count; ++i)
      clients.push_back(std::make_unique<Client>(loop, size));
  }

  ~CallbackGroup()
  {
    close();
  }

  bool connect(std::size_t i, std::uint16_t port)
  {
    struct Setup {
      CallbackLoop &loop;
      Client &client;
      std::uint16_t port;
      bool done = false, ok = false;
    } setup{loop, *clients[i], port};

    CallbackLoop::Command command{&setup, [](void *data) {
                                    auto &setup = *static_cast<Setup *>(data);
                                    setup.client.connect(setup.port, {&setup, [](void *data, bool ok) {
                                                                        auto &s = *static_cast<Setup *>(data);
                                                                        s.done = true;
                                                                        s.ok = ok;
                                                                        s.loop.stop();
                                                                      }});
                                  }};
    loop.post(command);
    loop.run();
    weave::detail::require(setup.done);
    return setup.ok;
  }

  void close()
  {
    for (auto &client : clients)
      client->close();
    while (std::any_of(clients.begin(), clients.end(), [](const auto &client) { return !client->is_closed(); }))
      loop.once();
  }
};

// Affine native loops, with one external submission and one future per job.
// Futures model the same controller-to-worker join boundary as Asio's use_future.
template <class Client>
class CallbackPool {
  struct Remote {
    CallbackLoop &loop;
    Client client;
    CallbackLoop::Command command;
    std::optional<std::promise<bool>> result;
    std::size_t rounds = 1;
    std::uint16_t port = 0;
    bool connect = false, validate = false;

    Remote(CallbackLoop &owner, std::size_t size) : loop(owner), client(owner, size)
    {
      command = {this, [](void *data) {
                   auto &remote = *static_cast<Remote *>(data);
                   Callback done{&remote, [](void *data, bool ok) {
                                   auto &r = *static_cast<Remote *>(data);
                                   // Publish only after the reusable connection state is no longer touched.
                                   auto result = std::move(*r.result);
                                   r.result.reset();
                                   result.set_value(ok);
                                 }};
                   if (remote.connect)
                     remote.client.connect(remote.port, done);
                   else
                     remote.client.exchange(remote.rounds, remote.validate, done);
                 }};
    }

    std::future<bool> submit()
    {
      result.emplace();
      auto future = result->get_future();
      loop.post(command);
      return future;
    }
  };

  struct Worker {
    CallbackLoop loop{Client::backend};
    CallbackLoop::Command stop;
    std::thread thread;
    std::vector<Remote *> clients;

    Worker()
    {
      stop = {this, [](void *data) {
                auto &worker = *static_cast<Worker *>(data);
                for (auto *remote : worker.clients)
                  remote->client.close();
                worker.loop.close_wakeup();
              }};
    }
  };

  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<std::unique_ptr<Remote>> clients_;
  bool stopped_ = false;

public:
  CallbackPool(std::size_t workers, std::size_t connections, std::size_t size)
  {
    weave::detail::require(workers != 0);
    for (std::size_t i = 0; i < workers; ++i)
      workers_.push_back(std::make_unique<Worker>());
    for (std::size_t i = 0; i < connections; ++i) {
      auto &worker = *workers_[i % workers];
      clients_.push_back(std::make_unique<Remote>(worker.loop, size));
      worker.clients.push_back(clients_.back().get());
    }
    for (auto &worker : workers_)
      worker->thread = std::thread([ptr = worker.get()] { ptr->loop.run(); });
  }

  ~CallbackPool()
  {
    stop();
  }

  Client &client(std::size_t i)
  {
    return clients_[i]->client;
  }

  CallbackLoop &loop(std::size_t i)
  {
    return workers_[i % workers_.size()]->loop;
  }

  std::future<bool> connect(std::size_t i, std::uint16_t port)
  {
    auto &remote = *clients_[i];
    remote.connect = true;
    remote.port = port;
    return remote.submit();
  }

  std::future<bool> exchange(std::size_t i, std::size_t rounds, bool validate)
  {
    auto &remote = *clients_[i];
    remote.connect = false;
    remote.rounds = rounds;
    remote.validate = validate;
    return remote.submit();
  }

  void stop()
  {
    if (std::exchange(stopped_, true))
      return;
    for (auto &worker : workers_)
      worker->loop.post(worker->stop);
    for (auto &worker : workers_)
      worker->thread.join();
  }
};

} // namespace support
