#include <weave/postgres.hpp>
#include "result_access.hpp"
#include "wire.hpp"
#include <openssl/crypto.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <source_location>
#include <thread>
#include <unordered_map>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace pg = weave::pg;
namespace storage = weave::pg::detail;
static std::atomic<unsigned> checks{0};

static void check(bool condition, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "Result memory check failed: %u\n", location.line());
    std::exit(1);
  }
}

struct Heap {
  struct Request {
    std::size_t size;
    std::size_t alignment;
  };

  std::mutex mutex;
  std::unordered_map<void *, Request> requests;
  std::size_t bytes = 0;
  std::size_t allocated = 0;
  std::size_t freed = 0;

  static void *allocate(void *state, std::size_t size, std::size_t alignment) noexcept
  {
    auto &heap = *static_cast<Heap *>(state);
    auto *data = ::operator new(size, std::align_val_t{alignment}, std::nothrow);
    check(data != nullptr);
    std::lock_guard lock{heap.mutex};
    check(heap.requests.emplace(data, Request{size, alignment}).second);
    heap.bytes += size;
    ++heap.allocated;
    return data;
  }

  static void deallocate(void *state, void *data, std::size_t size, std::size_t alignment) noexcept
  {
    auto &heap = *static_cast<Heap *>(state);
    {
      std::lock_guard lock{heap.mutex};
      auto found = heap.requests.find(data);
      check(found != heap.requests.end());
      check(found->second.size == size && found->second.alignment == alignment);
      check(heap.bytes >= size);
      heap.bytes -= size;
      heap.requests.erase(found);
      ++heap.freed;
    }
    ::operator delete(data, std::align_val_t{alignment});
  }

  storage::ResultHeap provider()
  {
    return {this, allocate, deallocate};
  }

  std::size_t live()
  {
    std::lock_guard lock{mutex};
    return bytes;
  }

  ~Heap()
  {
    check(bytes == 0 && requests.empty() && allocated == freed);
  }
};

static void populate(pg::ResultSet &result)
{
  storage::Writer schema;
  schema.integer(2, 2);
  const std::array<std::string_view, 2> names{"identifier_long_enough_to_allocate", "payload_long_enough_to_allocate"};
  for (auto name : names) {
    schema.string(name);
    schema.integer(41);
    schema.integer(3, 2);
    schema.integer(25);
    schema.integer(65535, 2);
    schema.integer(0xffffffff);
    schema.integer(0, 2);
  }
  auto columns = storage::columns(schema.bytes, storage::ResultAccess::schema(result));
  check(bool(columns));
  result.columns = std::move(*columns);

  for (unsigned index = 0; index != 12; ++index) {
    storage::Writer data;
    data.integer(2, 2);
    data.integer(4);
    data.bytes.insert(data.bytes.end(), {std::byte{'a'}, std::byte{0}, std::byte{'b'}, std::byte{0xff}});
    if (index % 2 == 0) {
      data.integer(0xffffffff);
    } else {
      data.integer(1024);
      data.bytes.insert(data.bytes.end(), 1024, std::byte{'x'});
    }
    auto row = storage::row(data.bytes, result.columns, storage::ResultAccess::rows(result));
    check(bool(row));
    result.rows.push_back(std::move(*row));
  }
  result.kind = pg::ResultKind::tuples;
  result.command.assign(80, 'c');
  result.parameter_types.assign(32, 25);
}

static void same_columns(const pg::ResultSet &left, const pg::ResultSet &right)
{
  check(left.columns.size() == right.columns.size());
  for (std::size_t index = 0; index != left.columns.size(); ++index) {
    const auto &a = left.columns[index];
    const auto &b = right.columns[index];
    check(a.name == b.name && a.table == b.table && a.attribute == b.attribute && a.type == b.type);
    check(a.type_size == b.type_size && a.modifier == b.modifier && a.format == b.format);
  }
}

static void same_rows(const pg::ResultSet &left, const pg::ResultSet &right)
{
  check(left.rows.size() == right.rows.size());
  for (std::size_t index = 0; index != left.rows.size(); ++index) {
    const auto &a = left.rows[index];
    const auto &b = right.rows[index];
    check(a.size() == b.size());
    for (std::size_t field = 0; field != a.size(); ++field)
      check(a[field].data == b[field].data && a[field].format == b[field].format);
  }
}

static void accounting()
{
  Heap heap;
  {
    auto result = storage::ResultAccess::create(heap.provider());
    check(result.memory_size() == heap.live());
    populate(result);
    const auto footprint = result.memory_size();
    check(footprint == heap.live());
    check(result.rows[0][0].bytes() == std::string_view{"a\0b\xff", 4});
    check(!result.rows[0][1].data && result.rows[1][1].data->size() == 1024);

    {
      auto copied = result.copy();
      check(copied.memory_size() + footprint == heap.live());
      same_columns(copied, result);
      same_rows(copied, result);
      check(copied.command == result.command);
      check(copied.rows[1][1].data->data() != result.rows[1][1].data->data());
      copied.rows[1][1].data->front() = 'y';
      check(result.rows[1][1].data->front() == 'x');
      auto moved = std::move(copied);
      check(moved.rows.size() == 12);
    }
    check(heap.live() == footprint);
    {
      pg::ResultSet assigned;
      assigned = result;
      check(assigned.memory_size() + footprint == heap.live());
      same_rows(assigned, result);
    }
    check(heap.live() == footprint);
    {
      auto metadata = result.copy({.columns = true, .rows = false, .observers = false});
      check(metadata.rows.empty());
      same_columns(metadata, result);
      check(metadata.memory_size() + footprint == heap.live());
      auto rows = result.copy({.columns = false, .rows = true, .observers = false});
      same_columns(rows, result);
      same_rows(rows, result);
      check(metadata.memory_size() + rows.memory_size() + footprint == heap.live());
    }
    check(heap.live() == footprint);
    result.rows.clear();
    check(result.memory_size() == footprint && heap.live() == footprint);
  }
  check(heap.live() == 0);
}

static void escaped()
{
  Heap heap;
  std::optional<pg::Row> retained;
  {
    auto result = storage::ResultAccess::create(heap.provider());
    populate(result);
    retained.emplace(std::move(result.rows[1]));
    check(result.memory_size() == heap.live());
  }
  check(retained->at(1).data->size() == 1024);
  check(retained->get_allocator().arena().bytes() == heap.live());
  retained.reset();
  check(heap.live() == 0);

  std::optional<storage::ResultText> text;
  {
    auto result = storage::ResultAccess::create(heap.provider());
    populate(result);
    text.emplace(std::move(*result.rows[1][1].data));
  }
  check(text->size() == 1024 && text->front() == 'x');
  check(text->get_allocator().arena().bytes() == heap.live());
  text.reset();
  check(heap.live() == 0);
}

static void foreign()
{
  Heap heap;
  Heap other;
  {
    auto result = storage::ResultAccess::create(heap.provider());
    populate(result);
    auto arena = storage::ResultArena::create(other.provider());
    storage::ResultText text{2048, 'z', storage::ResultAllocator<char>{arena}};
    result.columns[0].name = std::move(text);
    check(result.memory_size() == heap.live() + other.live());
    auto copy = result.copy();
    check(copy.columns[0].name.size() == 2048);
    check(copy.columns[0].name.get_allocator().arena().identity() != arena.identity());
    check(copy.memory_size() + result.memory_size() == heap.live() + other.live());
  }
  check(heap.live() == 0 && other.live() == 0);
}

static void concurrent()
{
  Heap heap;
  {
    auto result = storage::ResultAccess::create(heap.provider());
    populate(result);
    const auto footprint = result.memory_size();
    std::array<std::thread, 4> workers;
    for (auto &worker : workers) {
      worker = std::thread([&] {
        for (unsigned iteration = 0; iteration != 64; ++iteration) {
          auto copied = result.copy();
          same_rows(copied, result);
          check(copied.memory_size() > 0);
          check(result.memory_size() == footprint);
        }
      });
    }
    for (auto &worker : workers)
      worker.join();
    check(result.memory_size() == footprint && heap.live() == footprint);
  }
  check(heap.live() == 0);
}

static void default_storage()
{
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
  // MSVC allocates iterator proxies even for empty containers. Their pools
  // must remain visible through the source allocator, not a rebound temporary.
  pg::ResultSet result;
  check(result.columns.get_allocator().arena().identity() != nullptr);
  check(result.rows.get_allocator().arena().identity() != nullptr);
  check(result.command.get_allocator().arena().identity() != nullptr);
  check(result.parameter_types.get_allocator().arena().identity() != nullptr);
#endif
}

int main()
{
#if defined(_MSC_VER)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  default_storage();
  accounting();
  escaped();
  foreign();
  concurrent();
  std::printf("Result memory integration: %u checks\n", checks.load());
}
