#pragma once

#include <weave/types.hpp>
#include <functional>
#include <span>
#include <cstddef>

namespace weave::pg {

enum class TraceDirection {
  frontend,
  backend
};

enum class TraceContent {
  metadata,
  application
};

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

using TraceHandler = std::move_only_function<void(const TraceMessage &) noexcept>;

struct Trace {
  TraceHandler handler;
  TraceContent content = TraceContent::metadata;
  std::size_t payload_bytes = 4096;
};

} // namespace weave::pg
