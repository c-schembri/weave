#pragma once

#include <weave/postgres/events.hpp>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace weave::pg::detail {

struct EventReceiver {
  EventId id;
  std::string name;
  EventHandler handler;
  std::mutex mutex;
  std::atomic<bool> invoking{false};

  EventReceiver(EventId key, std::string label, EventHandler callback);
  Result<void> invoke(Event &event);
};

struct EventEntry {
  std::shared_ptr<EventReceiver> receiver;
  EventData data;
};

struct ResultEvents {
  ResultList<EventEntry> entries;
  bool invoking = false;

  explicit ResultEvents(ResultArena arena) : entries(ResultAllocator<EventEntry>{std::move(arena)})
  {
  }
};

struct EventAccess {
  static void create(ResultSet &result, Connection &connection, const std::vector<EventEntry> &entries);
  static Result<void> attach(ResultSet &result, Connection &connection, const std::vector<EventEntry> &entries);
  static void copy(ResultSet &result, const ResultSet &source);
  static void destroy(ResultSet &result) noexcept;
};

struct ConnectionEvents {
  Connection *owner;
  std::vector<EventEntry> entries;
  bool registering = false;

  explicit ConnectionEvents(Connection &connection);
  Result<EventId> add(std::string name, EventHandler handler);
  bool invoking() const noexcept;
  Result<EventData> data(EventId id) const;
  Result<void> set_data(EventId id, EventData data);
  void create(ResultSet &result) const;
  void reset();
  void destroy() noexcept;
};

} // namespace weave::pg::detail
