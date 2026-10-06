#pragma once

#include <weave/tls/context.hpp>
#include <weave/types.hpp>
#include <span>

namespace weave::detail {

enum class TlsAction {
  ready,
  input,
  output,
  eof,
  failed
};

enum class TlsOperation {
  read,
  write,
  exclusive
};

struct TlsStep {
  TlsAction action;
  std::size_t transferred = 0;
  Error error;
  u64 generation = 0;
};

class TlsEngine {
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit TlsEngine(std::unique_ptr<Impl> impl) noexcept;

public:
  static Result<TlsEngine> create(const TlsContext &context, bool server, const std::string &name);

  TlsEngine(TlsEngine &&other) noexcept;
  ~TlsEngine();

  Result<void> begin(TlsOperation operation) noexcept;
  void end(TlsOperation operation) noexcept;
  bool active() const noexcept;
  void fail(Error error) noexcept;

  TlsStep handshake() noexcept;
  TlsStep read(std::span<std::byte> buffer) noexcept;
  TlsStep write(std::span<const std::byte> buffer) noexcept;
  TlsStep shutdown(bool wait_peer) noexcept;

  Result<std::size_t> output(std::span<std::byte> buffer) noexcept;
  Result<bool> needs_input(u64 generation) const noexcept;
  Result<void> input(std::span<const std::byte> buffer) noexcept;

  TlsVersion version() const noexcept;
  std::string alpn() const;
};

struct TlsOperationGuard {
  TlsEngine &engine;
  TlsOperation operation;
  bool acquired = false;

  Result<void> begin() noexcept
  {
    auto result = engine.begin(operation);
    acquired = result.has_value();

    return result;
  }

  ~TlsOperationGuard()
  {
    if (acquired)
      engine.end(operation);
  }
};

} // namespace weave::detail
