#pragma once

#include <weave/types.hpp>
#include <functional>
#include <span>
#include <cstddef>

namespace weave::pg {

/// Frontend means client-to-server; backend means server-to-client.
enum class TraceDirection {
  frontend,
  backend
};

/// Metadata-only default or explicit sensitive application-payload tracing (SQL/data may be exposed).
enum class TraceContent {
  metadata,
  application
};

/// Inline borrowed protocol event; retain copies explicitly if needed after the callback returns.
/// Startup/authentication/backend-key bodies are withheld even in application tracing mode.
struct TraceMessage {
  TraceDirection direction;
  // Zero denotes an untagged startup/SSL request; length includes the four-byte length field, not the tag.
  char kind = 0;
  u32 length = 0;
  // Borrowed only through handler invocation. Startup/authentication/backend-key bodies are always withheld.
  std::span<const std::byte> payload;
  bool redacted = false;
  bool truncated = false;
};

/// Owned inline noexcept observer; its message and payload span are borrowed for one invocation.
using TraceHandler = std::move_only_function<void(const TraceMessage &) noexcept>;

/// Owned trace callback and bounded payload prefix policy; capture references must remain valid.
struct Trace {
  TraceHandler handler;
  TraceContent content = TraceContent::metadata;
  std::size_t payload_bytes = 4096;
};

} // namespace weave::pg
