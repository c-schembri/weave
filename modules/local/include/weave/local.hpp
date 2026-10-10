#pragma once

#include <weave/io.hpp>
#include <span>
#include <string>

namespace weave {

class LocalStream;
class LocalListener;

struct LocalPeer {
  u64 process;
  u64 user;
  u64 group;
};

namespace local {

// Filesystem paths, or @name on Linux. Arguments are owned; paths are never unlinked.
Task<LocalStream> connect(Context &context, std::string address);
Task<LocalStream> connect(std::string address);
Result<LocalListener> listen(Context &context, std::string address, int backlog = 512);
Task<LocalListener> listen(std::string address, int backlog = 512);

} // namespace local

class LocalStream {
public:
  LocalStream(LocalStream &&other) noexcept;
  LocalStream(const LocalStream &) = delete;
  ~LocalStream();

  Task<std::size_t> read(std::span<std::byte> buffer);
  Task<void> read_exactly(std::span<std::byte> buffer);
  Task<void> write_all(std::span<const std::byte> buffer);
  Result<void> shutdown_send();
  Result<void> cancel();
  Result<void> close();
  Result<std::string> local_address() const;
  Result<std::string> peer_address() const;
  // Linux SO_PEERCRED. Windows reports operation_not_supported, never fabricated IDs.
  Result<LocalPeer> peer_credentials() const;

private:
  friend Task<LocalStream> local::connect(Context &, std::string);
  friend class LocalListener;

  LocalStream(Context &context, std::uintptr_t socket, bool skip_success) noexcept
      : ctx_(&context), socket_(socket), skip_success_(skip_success)
  {
  }

  Context *ctx_;
  std::uintptr_t socket_;
  bool reading_ = false;
  bool writing_ = false;
  bool skip_success_ = false;
};

class LocalListener {
public:
  LocalListener(LocalListener &&other) noexcept;
  LocalListener(const LocalListener &) = delete;
  ~LocalListener();

  Task<LocalStream> accept();
  Result<void> cancel();
  Result<void> close();
  const std::string &local_address() const noexcept;

private:
  friend Result<LocalListener> local::listen(Context &, std::string, int);

  LocalListener(Context &context, std::uintptr_t socket, bool skip_success, std::string address) noexcept
      : ctx_(&context), socket_(socket), skip_success_(skip_success), address_(std::move(address))
  {
  }

  Context *ctx_;
  std::uintptr_t socket_;
  bool accepting_ = false;
  bool skip_success_ = false;
  std::string address_;
};

} // namespace weave
