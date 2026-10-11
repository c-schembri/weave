#include <weave/postgres/record.hpp>
#include "result_access.hpp"
#include <doctest/doctest.h>
#include <thread>

namespace pg = weave::pg;
namespace storage = weave::pg::detail;

namespace record_test {

struct Shipment {
  weave::i64 id;
  double weight;
  bool delivered;
  std::string recipient;
  std::optional<std::string> notes;
};

struct Borrowed {
  std::string_view recipient;
  std::optional<std::string_view> notes;
};

struct Empty {};

struct Alias {
  weave::i64 key = 0;
  std::string name;
  int unselected = 17;

  Alias() noexcept
  {
  }
};

struct Base {
  weave::i64 id = 0;
};

struct Inherited : Base {
  std::string recipient;
};

} // namespace record_test

template <>
struct weave::pg::RowTraits<record_test::Alias> {
  static constexpr auto fields = std::tuple{
    pg::field("id", &record_test::Alias::key),
    pg::field("recipient", &record_test::Alias::name)};
};

template <>
struct weave::pg::RowTraits<record_test::Inherited> {
  static constexpr auto mapping = std::tuple{
    pg::field("id", &record_test::Base::id),
    pg::field("recipient", &record_test::Inherited::recipient)};
  static constexpr const auto &fields = mapping;
};

static pg::Value text(std::string_view bytes)
{
  return {storage::ResultText{bytes}};
}

static pg::ResultSet sample()
{
  pg::ResultSet result;
  result.columns.push_back({.name = "recipient", .type = 25});
  result.columns.push_back({.name = "notes", .type = 25});
  result.columns.push_back({.name = "weight", .type = 701});
  result.columns.push_back({.name = "extra", .type = 25});
  result.columns.push_back({.name = "id", .type = 20});
  result.columns.push_back({.name = "delivered", .type = 16});
  result.rows.push_back(pg::Row{text("Ada"), pg::Value{}, text("1.25"), text("ignored"), text("42"), text("t")});
  storage::ResultAccess::bind_rows(result);
  return result;
}

template <class T>
static void rejects(const weave::Result<T> &result, pg::DecodeError error)
{
  CHECK_FALSE(result);
  if (!result)
    CHECK(result.error() == pg::make_error_code(error));
}

TEST_CASE("PostgreSQL structs decode by name without registration")
{
  auto result = sample();
  auto shipment = result.rows.front().as<record_test::Shipment>();
  REQUIRE(shipment);
  if (!shipment)
    return;

  CHECK(shipment->id == 42);
  CHECK(shipment->weight == 1.25);
  CHECK(shipment->delivered);
  CHECK(shipment->recipient == "Ada");
  CHECK_FALSE(shipment->notes);

  auto borrowed = result.rows.front().as<record_test::Borrowed>();
  REQUIRE(borrowed);
  if (!borrowed)
    return;
  CHECK(borrowed->recipient.data() == result.rows.front().values()[0].data->data());
  CHECK_FALSE(borrowed->notes);

  result = {};
  CHECK(shipment->recipient == "Ada");
  pg::Row empty;
  CHECK(empty.as<record_test::Empty>());
}

TEST_CASE("PostgreSQL cached struct decoders retain and validate their schema")
{
  auto result = sample();
  auto decoder = result.decoder<record_test::Shipment>();
  REQUIRE(decoder);
  if (!decoder)
    return;

  auto shipment = decoder->decode(result.rows.front());
  CHECK((shipment && shipment->id == 42 && shipment->recipient == "Ada"));
  auto copied_decoder = *decoder;
  auto retained = result.rows.front();
  auto moved = std::move(result);
  CHECK(copied_decoder.decode(moved.rows.front()));

  auto copied = moved.copy();
  rejects(decoder->decode(copied.rows.front()), pg::DecodeError::schema_mismatch);
  auto independent = sample();
  rejects(decoder->decode(independent.rows.front()), pg::DecodeError::schema_mismatch);
  moved = {};
  CHECK(decoder->decode(retained));

  auto moved_decoder = std::move(*decoder);
  rejects(decoder->decode(retained), pg::DecodeError::schema_mismatch);
  CHECK(moved_decoder.decode(retained));

  retained.clear();
  rejects(moved_decoder.decode(retained), pg::DecodeError::out_of_range);
}

TEST_CASE("PostgreSQL struct decoding preserves lookup NULL and conversion errors")
{
  auto result = sample();
  result.rows.front().values()[4] = text("invalid");
  rejects(result.rows.front().as<record_test::Shipment>(), pg::DecodeError::invalid_value);
  result.rows.front().values()[4] = text("9223372036854775808");
  rejects(result.rows.front().as<record_test::Shipment>(), pg::DecodeError::out_of_range);
  result.rows.front().values()[4] = pg::Value{};
  rejects(result.rows.front().as<record_test::Shipment>(), pg::DecodeError::null_value);

  pg::ResultSet missing;
  missing.columns.push_back({.name = "id"});
  missing.rows.push_back(pg::Row{text("42")});
  storage::ResultAccess::bind_rows(missing);
  rejects(missing.rows.front().as<record_test::Shipment>(), pg::DecodeError::missing_column);
  rejects(missing.decoder<record_test::Shipment>(), pg::DecodeError::missing_column);

  pg::ResultSet ambiguous;
  ambiguous.columns.push_back({.name = "recipient"});
  ambiguous.columns.push_back({.name = "recipient"});
  ambiguous.columns.push_back({.name = "notes"});
  ambiguous.rows.push_back(pg::Row{text("Ada"), text("Grace"), pg::Value{}});
  storage::ResultAccess::bind_rows(ambiguous);
  rejects(ambiguous.rows.front().as<record_test::Borrowed>(), pg::DecodeError::ambiguous_column);
  rejects(ambiguous.decoder<record_test::Borrowed>(), pg::DecodeError::ambiguous_column);

  auto empty = sample();
  empty.rows.clear();
  CHECK(empty.decoder<record_test::Shipment>());
  pg::ResultSet uninitialized;
  rejects(uninitialized.decoder<record_test::Shipment>(), pg::DecodeError::missing_column);
}

TEST_CASE("PostgreSQL explicit struct mappings support aliases and non-aggregates")
{
  static_assert(!std::is_aggregate_v<record_test::Alias>);
  auto result = sample();
  auto alias = result.rows.front().as<record_test::Alias>();
  CHECK((alias && alias->key == 42 && alias->name == "Ada"));
  CHECK((alias && alias->unselected == 17));
  auto decoder = result.decoder<record_test::Alias>();
  REQUIRE(decoder);
  if (!decoder)
    return;
  auto decoded = decoder->decode(result.rows.front());
  CHECK((decoded && decoded->key == 42 && decoded->name == "Ada"));

  auto inherited = result.rows.front().as<record_test::Inherited>();
  CHECK((inherited && inherited->id == 42 && inherited->recipient == "Ada"));
  auto base_decoder = result.decoder<record_test::Inherited>();
  REQUIRE(base_decoder);
  if (!base_decoder)
    return;
  CHECK(base_decoder->decode(result.rows.front()));
}

TEST_CASE("PostgreSQL failed struct decoding leaves source values unchanged")
{
  auto result = sample();
  auto &row = result.rows.front();
  row.values()[2] = text("invalid weight");
  row.values()[5] = pg::Value{};
  auto decoder = result.decoder<record_test::Shipment>();
  REQUIRE(decoder);
  if (!decoder)
    return;

  rejects(row.as<record_test::Shipment>(), pg::DecodeError::invalid_value);
  rejects(decoder->decode(row), pg::DecodeError::invalid_value);
  CHECK(row["id"].as<weave::i64>() == 42);
  CHECK(row["weight"].bytes() == "invalid weight");
  CHECK(row["recipient"].bytes() == "Ada");
  CHECK(row["delivered"].is_null());

  row.values()[2] = text("1.25");
  rejects(decoder->decode(row), pg::DecodeError::null_value);
  row.values()[5] = text("t");
  CHECK(decoder->decode(row));
}

TEST_CASE("PostgreSQL cached struct decoding shares immutable metadata across threads")
{
  const auto result = sample();
  const auto decoder = result.decoder<record_test::Shipment>();
  REQUIRE(decoder);
  if (!decoder)
    return;

  std::array<bool, 4> passed{};
  std::array<std::jthread, 4> workers;
  for (std::size_t index = 0; index < workers.size(); ++index) {
    workers[index] = std::jthread([&, index] {
      for (int iteration = 0; iteration < 32; ++iteration) {
        auto shipment = decoder->decode(result.rows.front());
        if (!shipment || shipment->id != 42 || shipment->recipient != "Ada" || shipment->notes)
          return;
      }
      passed[index] = true;
    });
  }

  for (auto &worker : workers)
    worker.join();
  for (bool success : passed)
    CHECK(success);
}
