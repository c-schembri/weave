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
};

static void *allocate(void *state, std::size_t size, std::size_t alignment) noexcept
{
  ++static_cast<HeapCounters *>(state)->allocations;
  return weave::db::detail::ResultHeap::reserve(nullptr, size, alignment);
}

static void deallocate(void *state, void *data, std::size_t size, std::size_t alignment) noexcept
{
  ++static_cast<HeapCounters *>(state)->frees;
  weave::db::detail::ResultHeap::release(nullptr, data, size, alignment);
}

} // namespace fixture

template <>
struct weave::db::RowTraits<fixture::Alias> {
  static constexpr auto fields = std::tuple{weave::db::field("id", &fixture::Alias::key)};
};

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
