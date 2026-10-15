#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <weave/database.hpp>
#include <weave/database/detail/result_storage.hpp>
#include <weave/database/detail/pool_inventory.hpp>
#include <doctest/doctest.h>
#include <optional>
#include <variant>

namespace fixture {

struct Decimal {
  weave::i64 coefficient;
  weave::u8 scale;
  bool operator==(const Decimal &) const = default;
};

struct Shipment {
  weave::i64 id;
  Decimal charge;
  std::string_view recipient;
  std::optional<weave::i64> cancelled;
};

struct Alias {
  weave::i64 key = 0;
  int untouched = 17;

  Alias() noexcept
  {
  }
};

struct Column {
  std::string_view name;
};

struct Field {
  std::variant<std::monostate, weave::i64, Decimal, std::string_view> value;
  std::error_code error;

  template <class T>
  weave::Result<T> as() const
  {
    if (error)
      return std::unexpected(error);
    if constexpr (std::same_as<T, std::optional<weave::i64>>) {
      if (std::holds_alternative<std::monostate>(value))
        return T{};
      auto number = as<weave::i64>();
      if (!number)
        return std::unexpected(number.error());
      return T{*number};
    } else {
      auto selected = std::get_if<T>(&value);
      if (!selected)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
      return *selected;
    }
  }
};

struct NativeRow {
  // Native integer/decimal values, not text or a universal Weave Value.
  std::array<Column, 4> columns{{{"recipient"}, {"charge"}, {"id"}, {"cancelled"}}};
  std::array<Field, 4> fields{
    {{std::string_view{"Ada"}, {}}, {Decimal{9007199254740993, 2}, {}}, {weave::i64{42}, {}}, {}}};

  Field operator[](std::string_view name) const
  {
    auto selected = weave::db::find_column(columns, name);
    if (!selected)
      return {{}, std::make_error_code(std::errc::no_such_file_or_directory)};
    return fields[*selected];
  }
};

template <class Row>
concept DecodableShipment = requires(Row &&row) { weave::db::decode<Shipment>(std::forward<Row>(row)); };

static_assert(DecodableShipment<NativeRow &>);
static_assert(DecodableShipment<const NativeRow &>);
static_assert(!DecodableShipment<NativeRow>);
static_assert(!DecodableShipment<const NativeRow>);

struct HeapCounters {
  std::size_t allocations = 0;
  std::size_t frees = 0;
  std::size_t retained = 0;
};

static void *allocate(void *state, std::size_t size, std::size_t alignment) noexcept
{
  auto &counters = *static_cast<HeapCounters *>(state);
  ++counters.allocations;
  counters.retained += size;
  return weave::db::detail::ResultHeap::reserve(nullptr, size, alignment);
}

static void deallocate(void *state, void *data, std::size_t size, std::size_t alignment) noexcept
{
  auto &counters = *static_cast<HeapCounters *>(state);
  ++counters.frees;
  weave::detail::require(size <= counters.retained);
  counters.retained -= size;
  weave::db::detail::ResultHeap::release(nullptr, data, size, alignment);
}

static HeapCounters null_state_counters;

static void *allocate_null_state(void *, std::size_t size, std::size_t alignment) noexcept
{
  return allocate(&null_state_counters, size, alignment);
}

static void deallocate_null_state(void *, void *data, std::size_t size, std::size_t alignment) noexcept
{
  deallocate(&null_state_counters, data, size, alignment);
}

} // namespace fixture

template <>
struct weave::db::RowTraits<fixture::Alias> {
  static constexpr auto fields = std::tuple{weave::db::field("id", &fixture::Alias::key)};
};

TEST_CASE("lazy arena bookkeeping shares its first allocation without freeing live blocks")
{
  namespace storage = weave::db::detail;
  const std::array alignments{std::size_t{1}, std::size_t{8}, std::size_t{64}, std::size_t{256}};
  const std::array sizes{std::size_t{0}, std::size_t{1}, std::size_t{64}, std::size_t{4096}};
  for (auto alignment : alignments) {
    for (auto size : sizes) {
      CAPTURE(alignment);
      CAPTURE(size);
      fixture::HeapCounters counters;
      auto first = storage::ResultStorage::allocate_first(
        {&counters, fixture::allocate, fixture::deallocate},
        size,
        alignment);
      CHECK(counters.allocations == 1);
      CHECK(first.storage->bytes() == counters.retained);
      CHECK(reinterpret_cast<std::uintptr_t>(first.data) % alignment == 0);
      if (size)
        std::memset(first.data, 17, size);

      auto *second = first.storage->allocate(33, 64);
      CHECK(counters.allocations == 2);
      CHECK(first.storage->bytes() == counters.retained);
      CHECK(reinterpret_cast<std::uintptr_t>(second) % 64 == 0);
      std::memset(second, 43, 33);

      first.storage->release();
      CHECK(counters.frees == 0);
      if (size)
        CHECK(static_cast<const unsigned char *>(first.data)[size - 1] == 17);
      storage::ResultStorage::deallocate(first.data);
      CHECK(counters.frees == 0);
      CHECK(static_cast<const unsigned char *>(second)[32] == 43);
      storage::ResultStorage::deallocate(second);
      CHECK(counters.frees == 2);
      CHECK(counters.retained == 0);
    }
  }
}

TEST_CASE("standard arena small copies stay lazy and grow independently of their source")
{
  namespace storage = weave::db::detail;
  std::optional<storage::ResultText> copied_text;
  std::optional<storage::ResultList<weave::u64>> copied_empty;
  {
    auto arena = storage::ResultArena::create();
    auto fresh = arena.fresh();
    CHECK(fresh.identity() == nullptr);
    CHECK(fresh.bytes() == 0);

    storage::ResultText text{"tiny", storage::ResultAllocator<char>{arena}};
    storage::ResultList<weave::u64> empty{storage::ResultAllocator<weave::u64>{arena}};
    const auto source_bytes = arena.bytes();
    copied_text.emplace(text);
    copied_empty.emplace(empty);
    REQUIRE(copied_text);
    REQUIRE(copied_empty);
    if (!copied_text || !copied_empty)
      return;
    CHECK(*copied_text == "tiny");
    CHECK(copied_empty->empty());

    const bool debug_proxies =
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
      true;
#else
      false;
#endif
    CHECK((copied_text->get_allocator().arena().identity() != nullptr) == debug_proxies);
    CHECK((copied_empty->get_allocator().arena().identity() != nullptr) == debug_proxies);
    if (!debug_proxies) {
      CHECK(copied_text->get_allocator().arena().bytes() == 0);
      CHECK(copied_empty->get_allocator().arena().bytes() == 0);
    }
    CHECK(copied_text->get_allocator().arena().identity() != arena.identity());
    CHECK(copied_empty->get_allocator().arena().identity() != arena.identity());

    copied_text->append(256, 'x');
    copied_empty->assign(8, 84);
    CHECK(arena.bytes() == source_bytes);
    CHECK(text == "tiny");
    CHECK(empty.empty());
  }

  REQUIRE(copied_text);
  REQUIRE(copied_empty);
  if (!copied_text || !copied_empty)
    return;
  std::string expected{"tiny"};
  expected.append(256, 'x');
  CHECK(std::string_view{*copied_text} == expected);
  REQUIRE(copied_empty->size() == 8);
  if (copied_empty->size() != 8)
    return;
  CHECK(copied_empty->back() == 84);
}

TEST_CASE("small custom arena copies retain eager provider policy after their source dies")
{
  namespace storage = weave::db::detail;
  fixture::HeapCounters counters;
  const storage::ResultHeap heap{&counters, fixture::allocate, fixture::deallocate};
  std::optional<storage::ResultText> copied;
  {
    auto arena = storage::ResultArena::create(heap);
    const auto before_fresh = counters.allocations;
    auto fresh = arena.fresh();
    CHECK(counters.allocations == before_fresh + 1);
    CHECK(fresh.identity() != nullptr);
    CHECK(fresh.identity() != arena.identity());
    CHECK(fresh.heap().state == heap.state);
    CHECK(fresh.heap().allocate == heap.allocate);
    CHECK(fresh.heap().deallocate == heap.deallocate);

    storage::ResultText text{"tiny", storage::ResultAllocator<char>{arena}};
    copied.emplace(text);
    REQUIRE(copied);
    if (!copied)
      return;
    CHECK(copied->get_allocator().arena().identity() != nullptr);
    CHECK(copied->get_allocator().arena().identity() != arena.identity());
    const auto copied_heap = copied->get_allocator().arena().heap();
    CHECK(copied_heap.state == heap.state);
    CHECK(copied_heap.allocate == heap.allocate);
    CHECK(copied_heap.deallocate == heap.deallocate);
  }

  REQUIRE(copied);
  if (!copied)
    return;
  CHECK(*copied == "tiny");
  copied->append(256, 'x');
  std::string expected{"tiny"};
  expected.append(256, 'x');
  CHECK(std::string_view{*copied} == expected);
  copied.reset();
  CHECK(counters.allocations == counters.frees);
  CHECK(counters.retained == 0);
}

TEST_CASE("null-state custom arena callbacks keep fresh copies eager")
{
  namespace storage = weave::db::detail;
  fixture::null_state_counters = {};
  const storage::ResultHeap heap{nullptr, fixture::allocate_null_state, fixture::deallocate_null_state};
  {
    auto arena = storage::ResultArena::create(heap);
    const auto before_fresh = fixture::null_state_counters.allocations;
    auto fresh = arena.fresh();
    CHECK(fixture::null_state_counters.allocations == before_fresh + 1);
    CHECK(fresh.identity() != nullptr);
    CHECK(fresh.identity() != arena.identity());
    CHECK(fresh.heap().state == nullptr);
    CHECK(fresh.heap().allocate == heap.allocate);
    CHECK(fresh.heap().deallocate == heap.deallocate);
  }
  CHECK(fixture::null_state_counters.allocations == fixture::null_state_counters.frees);
  CHECK(fixture::null_state_counters.retained == 0);
}

TEST_CASE("portable record decoding leaves native codec and lifetimes to the row adapter")
{
  fixture::NativeRow row;
  auto result = weave::db::decode<fixture::Shipment>(row);
  REQUIRE(result);
  if (!result)
    return;

  CHECK(result->id == 42);
  CHECK((result->charge == fixture::Decimal{9007199254740993, 2}));
  CHECK(result->recipient.data() == std::get<std::string_view>(row.fields[0].value).data());
  CHECK(!result->cancelled);

  auto alias = weave::db::decode<fixture::Alias>(row);
  REQUIRE(alias);
  if (!alias)
    return;
  CHECK(alias->key == 42);
  CHECK(alias->untouched == 17);

  row.fields[1].error = std::make_error_code(std::errc::result_out_of_range);
  auto failed = weave::db::decode<fixture::Shipment>(row);
  REQUIRE(!failed);
  if (failed)
    return;
  CHECK(failed.error() == std::errc::result_out_of_range);
  CHECK(std::get<weave::i64>(row.fields[2].value) == 42);
}

TEST_CASE("column lookup and fixed selector binding preserve errors")
{
  const std::array<fixture::Column, 3> columns{{{"id"}, {"name"}, {"id"}}};
  auto missing = weave::db::find_column(columns, "absent");
  auto duplicate = weave::db::find_column(columns, "id");
  auto found = weave::db::find_column(columns, "name");
  REQUIRE(!missing);
  REQUIRE(!duplicate);
  if (missing || duplicate)
    return;
  CHECK(missing.error() == weave::db::ColumnError::missing);
  CHECK(duplicate.error() == weave::db::ColumnError::ambiguous);
  CHECK(found == 1);

  const std::array<std::string_view, 2> names{"name", "id"};
  std::size_t lookups = 0;
  auto indices = weave::db::bind_columns(names, [&](std::string_view name) -> weave::Result<std::size_t> {
    ++lookups;
    if (name == "id")
      return std::unexpected(std::error_code{});
    return 1;
  });
  CHECK(lookups == 2);
  REQUIRE(!indices);
  if (indices)
    return;
  CHECK(indices.error() == std::error_code{});

  const std::array<std::string_view, 0> empty{};
  auto bound_empty = weave::db::bind_columns(empty, [](std::string_view) -> weave::Result<std::size_t> {
    std::abort();
  });
  CHECK(bound_empty);
}

TEST_CASE("portable arena containers preserve allocator ownership and standard adapters")
{
  namespace storage = weave::db::detail;
  fixture::HeapCounters counters;
  {
    auto arena = storage::ResultArena::create({&counters, fixture::allocate, fixture::deallocate});
    storage::ResultList<storage::ResultText> values{storage::ResultAllocator<storage::ResultText>{arena}};
    values.emplace_back("a deliberately non-SSO result string", storage::ResultAllocator<char>{arena});
    auto copied = values;
    CHECK(copied.get_allocator().arena().identity() != values.get_allocator().arena().identity());

    auto moved = std::move(values);
    REQUIRE(moved.size() == 1);
    if (moved.empty())
      return;
    CHECK(std::format("{}", moved[0]) == "a deliberately non-SSO result string");
    CHECK(std::hash<storage::ResultText>{}(moved[0]) == std::hash<std::string_view>{}(moved[0]));
    CHECK(arena.bytes() > sizeof(storage::ResultStorage));
  }
  CHECK(counters.allocations > 0);
  CHECK(counters.allocations == counters.frees);
}

TEST_CASE("portable arena heaps preserve ordinary and over-aligned allocation across copies")
{
  namespace storage = weave::db::detail;

  struct alignas(64) Value {
    std::array<weave::u64, 8> numbers{};
  };

  fixture::HeapCounters counters;
  {
    auto arena = storage::ResultArena::create({&counters, fixture::allocate, fixture::deallocate});
    storage::ResultList<Value> values{storage::ResultAllocator<Value>{arena}};
    values.resize(3);
    CHECK(reinterpret_cast<std::uintptr_t>(values.data()) % alignof(Value) == 0);
    values[2].numbers[0] = 84;

    auto copied = values;
    CHECK(reinterpret_cast<std::uintptr_t>(copied.data()) % alignof(Value) == 0);
    CHECK(copied.get_allocator().arena().identity() != arena.identity());
    values.clear();
    CHECK(copied[2].numbers[0] == 84);
  }
  CHECK(counters.allocations > 0);
  CHECK(counters.allocations == counters.frees);
}

TEST_CASE("synchronous pool bookkeeping needs no execution context or async admission")
{
  weave::db::detail::PoolInventory<int> inventory{1};
  inventory.add_idle(std::make_unique<int>(42));
  CHECK(inventory.stats().idle == 1);
  CHECK(weave::db::valid_pool_options({}));

  const bool admitted = inventory.begin_acquire();
  REQUIRE(admitted);
  if (!admitted)
    return;
  std::unique_ptr<int> entry;
  const bool open = inventory.take_idle(entry);
  REQUIRE(open);
  REQUIRE(entry);
  if (!open || !entry)
    return;
  const bool borrowed = inventory.publish_borrow();
  REQUIRE(borrowed);
  if (!borrowed)
    return;
  inventory.finish_acquire(false);

  inventory.return_entry(entry, true, [](int &value) noexcept {
    value = 17;
  });
  CHECK(!entry);
  auto retired = inventory.retire_idle();
  REQUIRE(retired.size() == 1);
  if (retired.size() != 1)
    return;
  CHECK(*retired[0] == 17);
  CHECK(inventory.stats().borrowed == 0);
  CHECK(inventory.stats().discarded == 1);
  CHECK(!inventory.begin_acquire());
}
