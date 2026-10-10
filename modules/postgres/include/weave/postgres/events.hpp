#pragma once

#include <weave/task.hpp>
#include <weave/types.hpp>
#include <functional>
#include <memory>
#include <string_view>

namespace weave::pg {

class Connection;
struct ResultSet;

struct EventId {
  u64 value = 0;
  bool operator==(const EventId &) const = default;
};

using EventData = std::shared_ptr<void>;

enum class EventKind {
  registered,
  connection_reset,
  connection_destroy,
  result_create,
  result_copy,
  result_destroy
};

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

using EventHandler = std::move_only_function<Result<void>(Event &) noexcept>;

} // namespace weave::pg
