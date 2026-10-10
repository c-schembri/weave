#include <weave/postgres/connection.hpp>
#include "events.hpp"
#include "result_access.hpp"
#include <algorithm>
#include <limits>
#include <utility>
#include <unordered_set>

namespace weave::pg {

namespace {

std::atomic<u64> event_sequence{0};
thread_local bool dispatching_event = false;

struct Dispatch {
  std::atomic<bool> &invoking;

  explicit Dispatch(std::atomic<bool> &active) : invoking(active)
  {
    weave::detail::require(!dispatching_event);
    dispatching_event = true;
    invoking.store(true, std::memory_order_release);
  }

  ~Dispatch()
  {
    invoking.store(false, std::memory_order_release);
    dispatching_event = false;
  }
};

struct ResultDispatch {
  detail::ResultEvents &events;

  explicit ResultDispatch(detail::ResultEvents &state) : events(state)
  {
    weave::detail::require(!events.invoking);
    events.invoking = true;
  }

  ~ResultDispatch()
  {
    events.invoking = false;
  }
};

auto find_event(auto &entries, EventId id)
{
  return std::ranges::find_if(entries, [id](const auto &entry) {
    return entry.receiver->id == id;
  });
}

void copy_fields(ResultSet &destination, const ResultSet &source, ResultCopyOptions options = {})
{
  destination.kind = source.kind;
  destination.suspended = source.suspended;
  destination.command.assign(source.command.data(), source.command.size());
  destination.parameter_types.assign(source.parameter_types.begin(), source.parameter_types.end());

  const auto &schema = detail::ResultAccess::schema(destination);
  if (options.columns || options.rows) {
    destination.columns.reserve(source.columns.size());
    for (const auto &column : source.columns) {
      destination.columns.push_back(
        {detail::ResultText{column.name.data(), column.name.size(), detail::ResultAllocator<char>{schema}},
          column.table,
          column.attribute,
          column.type,
          column.type_size,
          column.modifier,
          column.format});
    }
  }
  if (options.rows && !source.rows.empty()) {
    auto arena = detail::ResultAccess::rows(destination);
    destination.rows.reserve(source.rows.size());
    for (const auto &row : source.rows) {
      Row copied{detail::ResultAllocator<Value>{arena}};
      copied.reserve(row.size());
      for (const auto &value : row) {
        std::optional<detail::ResultText> data;
        if (value.data)
          data.emplace(value.data->data(), value.data->size(), detail::ResultAllocator<char>{arena});
        copied.push_back({std::move(data), value.format});
      }
      destination.rows.push_back(std::move(copied));
    }
  }
}

} // namespace

namespace detail {

EventReceiver::EventReceiver(EventId key, std::string label, EventHandler callback)
    : id(key), name(std::move(label)), handler(std::move(callback))
{
}

Result<void> EventReceiver::invoke(Event &event)
{
  // Fail reentrancy before locking, including cross-observer lock cycles.
  weave::detail::require(!dispatching_event);
  std::lock_guard lock{mutex};
  Dispatch dispatch{invoking};
  return handler(event);
}

void EventAccess::create(ResultSet &result, Connection &connection, const std::vector<EventEntry> &entries)
{
  weave::detail::require(!result.events_);
  if (entries.empty())
    return;

  result.events_ = std::make_unique<ResultEvents>(ResultAccess::schema(result));
  ResultDispatch dispatch{*result.events_};
  for (const auto &entry : entries) {
    EventEntry created{entry.receiver, {}};
    Event event{
      EventKind::result_create,
      entry.receiver->id,
      entry.receiver->name,
      created.data,
      &connection,
      &result,
      nullptr,
      &entry.data};
    if (auto accepted = entry.receiver->invoke(event); accepted)
      result.events_->entries.push_back(std::move(created));
  }
}

Result<void> EventAccess::attach(ResultSet &result, Connection &connection, const std::vector<EventEntry> &entries)
{
  if (dispatching_event || (result.events_ && result.events_->invoking))
    return std::unexpected(make_error_code(Error::busy));
  if (entries.empty())
    return {};

  if (!result.events_)
    result.events_ = std::make_unique<ResultEvents>(ResultAccess::schema(result));
  ResultDispatch dispatch{*result.events_};
  Result<void> status;
  for (const auto &entry : entries) {
    if (find_event(result.events_->entries, entry.receiver->id) != result.events_->entries.end())
      continue;

    EventEntry created{entry.receiver, {}};
    Event event{
      EventKind::result_create,
      entry.receiver->id,
      entry.receiver->name,
      created.data,
      &connection,
      &result,
      nullptr,
      &entry.data};
    if (auto accepted = entry.receiver->invoke(event); accepted)
      result.events_->entries.push_back(std::move(created));
    else if (status)
      status = std::unexpected(accepted.error());
  }
  return status;
}

void EventAccess::copy(ResultSet &result, const ResultSet &source)
{
  if (!source.events_)
    return;

  weave::detail::require(!source.events_->invoking);
  result.events_ = std::make_unique<ResultEvents>(ResultAccess::schema(result));
  ResultDispatch dispatch{*result.events_};
  for (const auto &entry : source.events_->entries) {
    EventEntry copied{entry.receiver, {}};
    Event event{
      EventKind::result_copy,
      entry.receiver->id,
      entry.receiver->name,
      copied.data,
      nullptr,
      &result,
      &source,
      &entry.data};
    if (auto accepted = entry.receiver->invoke(event); accepted)
      result.events_->entries.push_back(std::move(copied));
  }
}

void EventAccess::destroy(ResultSet &result) noexcept
{
  if (!result.events_)
    return;

  {
    ResultDispatch dispatch{*result.events_};
    for (auto &entry : result.events_->entries) {
      Event event{EventKind::result_destroy, entry.receiver->id, entry.receiver->name, entry.data, nullptr, &result};
      static_cast<void>(entry.receiver->invoke(event));
    }
  }
  result.events_.reset();
}

ConnectionEvents::ConnectionEvents(Connection &connection) : owner(&connection)
{
}

Result<EventId> ConnectionEvents::add(std::string name, EventHandler handler)
{
  if (invoking())
    return std::unexpected(make_error_code(Error::busy));
  if (name.empty() || name.find('\0') != std::string::npos || !handler)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if (std::ranges::any_of(entries, [&name](const auto &entry) {
        return entry.receiver->name == name;
      }))
    return std::unexpected(std::make_error_code(std::errc::file_exists));

  auto sequence = event_sequence.fetch_add(1, std::memory_order_relaxed);
  weave::detail::require(sequence != std::numeric_limits<u64>::max());
  EventId id{sequence + 1};
  EventEntry entry{std::make_shared<EventReceiver>(id, std::move(name), std::move(handler)), {}};

  struct Registration {
    bool &active;

    ~Registration()
    {
      active = false;
    }
  } registration{registering};

  registering = true;
  Event event{EventKind::registered, id, entry.receiver->name, entry.data, owner};
  if (auto accepted = entry.receiver->invoke(event); !accepted)
    return std::unexpected(accepted.error());

  entries.push_back(std::move(entry));
  return id;
}

bool ConnectionEvents::invoking() const noexcept
{
  return registering || std::ranges::any_of(entries, [](const auto &entry) {
    return entry.receiver->invoking.load(std::memory_order_acquire);
  });
}

Result<EventData> ConnectionEvents::data(EventId id) const
{
  auto entry = find_event(entries, id);
  if (entry == entries.end())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  return entry->data;
}

Result<void> ConnectionEvents::set_data(EventId id, EventData data)
{
  if (invoking())
    return std::unexpected(make_error_code(Error::busy));

  auto entry = find_event(entries, id);
  if (entry == entries.end())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  entry->data = std::move(data);
  return {};
}

void ConnectionEvents::create(ResultSet &result) const
{
  EventAccess::create(result, *owner, entries);
}

void ConnectionEvents::reset()
{
  for (auto &entry : entries) {
    Event event{EventKind::connection_reset, entry.receiver->id, entry.receiver->name, entry.data, owner};
    static_cast<void>(entry.receiver->invoke(event));
  }
}

void ConnectionEvents::destroy() noexcept
{
  for (auto &entry : entries) {
    Event event{EventKind::connection_destroy, entry.receiver->id, entry.receiver->name, entry.data, owner};
    static_cast<void>(entry.receiver->invoke(event));
  }
  entries.clear();
}

} // namespace detail

ResultSet::ResultSet() = default;

ResultSet::ResultSet(detail::ResultHeap heap)
    : schema_storage_(detail::ResultArena::create(heap)), columns(detail::ResultAllocator<Column>{schema_storage_}),
      rows(detail::ResultAllocator<Row>{detail::ResultArena::create(heap)}),
      command(detail::ResultAllocator<char>{schema_storage_}),
      parameter_types(detail::ResultAllocator<u32>{schema_storage_})
{
}

ResultSet::ResultSet(const ResultSet &other) : ResultSet(other.schema_storage_.heap())
{
  copy_fields(*this, other);
  detail::EventAccess::copy(*this, other);
}

ResultSet::ResultSet(ResultSet &&other) noexcept
    : schema_storage_(std::move(other.schema_storage_)), events_(std::move(other.events_)), kind(other.kind),
      columns(std::move(other.columns)), rows(std::move(other.rows)), command(std::move(other.command)),
      parameter_types(std::move(other.parameter_types)), suspended(other.suspended)
{
  weave::detail::require(!events_ || !events_->invoking);
}

ResultSet &ResultSet::operator=(const ResultSet &other)
{
  if (this != &other) {
    detail::EventAccess::destroy(*this);
    schema_storage_ = detail::ResultArena::create(other.schema_storage_.heap());
    columns = detail::ResultList<Column>{detail::ResultAllocator<Column>{schema_storage_}};
    rows = detail::ResultList<Row>{detail::ResultAllocator<Row>{detail::ResultArena::create(schema_storage_.heap())}};
    command = detail::ResultText{detail::ResultAllocator<char>{schema_storage_}};
    parameter_types = detail::ResultList<u32>{detail::ResultAllocator<u32>{schema_storage_}};
    copy_fields(*this, other);
    detail::EventAccess::copy(*this, other);
  }
  return *this;
}

ResultSet &ResultSet::operator=(ResultSet &&other) noexcept
{
  if (this != &other) {
    weave::detail::require(!other.events_ || !other.events_->invoking);
    detail::EventAccess::destroy(*this);
    schema_storage_ = std::move(other.schema_storage_);
    kind = other.kind;
    columns = std::move(other.columns);
    rows = std::move(other.rows);
    command = std::move(other.command);
    parameter_types = std::move(other.parameter_types);
    suspended = other.suspended;
    events_ = std::move(other.events_);
  }
  return *this;
}

ResultSet::~ResultSet()
{
  detail::EventAccess::destroy(*this);
}

ResultSet ResultSet::copy(ResultCopyOptions options) const
{
  weave::detail::require(!events_ || !events_->invoking);

  ResultSet result{schema_storage_.heap()};
  copy_fields(result, *this, options);

  if (options.observers)
    detail::EventAccess::copy(result, *this);
  return result;
}

std::size_t ResultSet::memory_size() const
{
  std::unordered_set<const void *> visited;
  std::size_t total = events_ ? sizeof(detail::ResultEvents) : 0;
  auto add = [&](const detail::ResultArena &arena) {
    if (arena.identity() && visited.insert(arena.identity()).second) {
      const auto size = arena.bytes();
      weave::detail::require(size <= std::numeric_limits<std::size_t>::max() - total);
      total += size;
    }
  };

  add(schema_storage_);
  add(columns.get_allocator().arena());
  add(rows.get_allocator().arena());
  add(command.get_allocator().arena());
  add(parameter_types.get_allocator().arena());
  if (events_)
    add(events_->entries.get_allocator().arena());
  for (const auto &column : columns)
    add(column.name.get_allocator().arena());
  for (const auto &row : rows) {
    add(row.get_allocator().arena());
    for (const auto &value : row) {
      if (value.data)
        add(value.data->get_allocator().arena());
    }
  }
  return total;
}

Result<EventData> ResultSet::event_data(EventId id) const
{
  if (!events_)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto entry = find_event(events_->entries, id);
  if (entry == events_->entries.end())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  return entry->data;
}

Result<void> ResultSet::set_event_data(EventId id, EventData data)
{
  if (events_ && events_->invoking)
    return std::unexpected(make_error_code(Error::busy));
  if (!events_)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto entry = find_event(events_->entries, id);
  if (entry == events_->entries.end())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  entry->data = std::move(data);
  return {};
}

} // namespace weave::pg
