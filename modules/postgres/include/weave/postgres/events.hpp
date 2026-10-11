#pragma once

#include <weave/task.hpp>
#include <weave/types.hpp>
#include <functional>
#include <memory>
#include <string_view>

namespace weave::pg {

class Connection;
struct ResultSet;

/// Lifecycle observer registration identity; zero is an unregistered/default value.
struct EventId {
  u64 value = 0;
  bool operator==(const EventId &) const = default;
};

/// Application-owned shared state retained with an observer/connection/result lifecycle.
using EventData = std::shared_ptr<void>;

/// Supported connection/result lifecycle notifications.
enum class EventKind {
  registered,
  connection_reset,
  connection_destroy,
  result_create,
  result_copy,
  result_destroy
};

/// Invocation-scoped lifecycle context; all pointers/views/references are borrowed for the callback only.
/// data is mutable application state; do not re-enter conflicting protocol operations from the callback.
struct Event {
  EventKind kind;
  EventId id;
  std::string_view name;
  EventData &data;
  Connection *connection = nullptr;
  const ResultSet *result = nullptr;
  const ResultSet *source = nullptr;
  const EventData *source_data = nullptr;
};

/// Owned noexcept lifecycle callback. Captured references are not owned; destruction notifications
/// cannot undo destruction. Return errors for fallible lifecycle setup/copy operations.
using EventHandler = std::move_only_function<Result<void>(Event &) noexcept>;

} // namespace weave::pg
