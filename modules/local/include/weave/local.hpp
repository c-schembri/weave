#pragma once

#include <weave/io.hpp>
#include <span>
#include <string>

namespace weave {

class LocalStream;
class LocalListener;

/// Linux kernel-reported peer process/user/group identifiers; not a cross-platform identity API.
struct LocalPeer {
  u64 process;
  u64 user;
  u64 group;
};

namespace local {

/// Connect an owned filesystem path (or Linux @abstract name) on context, which must outlive the stream.
/// Copies lvalue strings and moves rvalues before suspension; never unlinks filesystem paths.
Task<LocalStream> connect(Context &context, std::string address);
/// Connect an owned local address using the active Context when the lazy task executes.
Task<LocalStream> connect(std::string address);
/// Synchronously bind an owned local address on context. Caller manages filesystem path cleanup.
Result<LocalListener> listen(Context &context, std::string address, int backlog = 512);
/// Bind an owned address lazily on the active Context; abstract names are Linux-only.
Task<LocalListener> listen(std::string address, int backlog = 512);

} // namespace local

/// Move-only AF_UNIX byte stream retaining its original Context. One read and one write may be pending.
/// Keep the stream, Context and borrowed buffers alive until native operations drain.
/// Synchronous socket methods also require owner/compatible runtime execution.
class LocalStream {
public:
  /// Transfer an idle stream; moving during I/O is a contract violation.
  LocalStream(LocalStream &&other) noexcept;
  LocalStream(const LocalStream &) = delete;
  /// Close the idle socket; active operations must have drained first.
  ~LocalStream();

  /// Read up to buffer.size() bytes on a compatible Context; zero is EOF or an empty buffer.
  /// Borrow writable storage through completion/cancellation.
  Task<std::size_t> read(std::span<std::byte> buffer);
  /// Fill the borrowed buffer or fail with connection_reset on premature EOF.
  Task<void> read_exactly(std::span<std::byte> buffer);
  /// Send all borrowed bytes; storage must survive completion/cancellation.
  Task<void> write_all(std::span<const std::byte> buffer);
  /// Synchronously half-close sending while preserving reads; return native errors.
  Result<void> shutdown_send();
  /// Request native I/O cancellation without waiting; pending tasks must still drain.
  Result<void> cancel();
  /// Close synchronously when idle, or return busy if I/O is pending. Does not unlink paths.
  Result<void> close();
  /// Synchronously query and return an owning local address string.
  Result<std::string> local_address() const;
  /// Synchronously query and return an owning peer address string.
  Result<std::string> peer_address() const;
  /// Query Linux SO_PEERCRED synchronously. Windows reports operation_not_supported, not fabricated IDs.
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

/// Move-only local listener. Context and listener must outlive its single pending accept.
class LocalListener {
public:
  /// Transfer an idle listener; a pending accept prevents safe movement.
  LocalListener(LocalListener &&other) noexcept;
  LocalListener(const LocalListener &) = delete;
  /// Close an idle listener without unlinking its filesystem path.
  ~LocalListener();

  /// Accept on a compatible Context; concurrent accepts fail as busy.
  Task<LocalStream> accept();
  /// Request accept cancellation without waiting for native completion.
  Result<void> cancel();
  /// Close synchronously if no accept is pending; otherwise return busy. Never unlinks paths.
  Result<void> close();
  /// Borrow the cached bound address until listener destruction or movement; no syscall.
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
