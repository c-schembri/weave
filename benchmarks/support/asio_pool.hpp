#pragma once
#include "asio_config.hpp"
#include <memory>
#include <thread>
#include <vector>

namespace bench {

class AsioPool {
  std::vector<std::unique_ptr<asio::io_context>> contexts_;
  std::vector<asio::executor_work_guard<asio::io_context::executor_type>> guards_;
  std::vector<std::thread> threads_;

public:
  AsioPool(std::size_t workers, bool sharded)
  {
    for (std::size_t i = 0; i < (sharded ? workers : 1); ++i) {
      contexts_.push_back(std::make_unique<asio::io_context>());
      guards_.emplace_back(contexts_.back()->get_executor());
    }
    for (std::size_t i = 0; i < workers; ++i)
      threads_.emplace_back([this, i] { context(i).run(); });
  }

  ~AsioPool()
  {
    guards_.clear();
    for (auto &thread : threads_)
      thread.join();
  }

  asio::io_context &context(std::size_t worker)
  {
    return *contexts_[worker % contexts_.size()];
  }
};

} // namespace bench
