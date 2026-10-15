#include <weave/mysql.hpp>
#include <weave/tls.hpp>
#include <weave/channel.hpp>
#include <weave/scope.hpp>
#if defined(WEAVE_MYSQL_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include "tls_certificates.hpp"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;

enum class Mode {
  fast,
  full,
  switch_native,
  switch_caching,
  wrong_sequence,
  malformed_column,
  malformed_row,
  malformed_end,
  eof_counts,
  sql_error,
  mid_error,
  local_file,
  oversized,
  repeated_auth,
  truncated,
  cancelled,
  row_limit,
  byte_limit,
  no_tls,
  wrapped_sequence,
  binary,
  schema_flags,
  binary_truncated,
  binary_bitmap,
  prepared_error,
  prepare_metadata_error,
  prepared_cancel,
  stream_abandon,
  stream_sql_error,
  deferred_cancel,
  pending_write_cancel
};

struct Shipment {
  weave::i64 id;
  std::string name;
};

static void put(Bytes &bytes, weave::u64 value, std::size_t size)
{
  for (std::size_t index = 0; index < size; ++index)
    bytes.push_back(std::byte{static_cast<weave::u8>(value >> (8 * index))});
}

static void text(Bytes &bytes, std::string_view value, bool nul = false)
{
  if (!nul)
    put(bytes, value.size(), 1);
  auto data = std::as_bytes(std::span{value});
  bytes.insert(bytes.end(), data.begin(), data.end());
  if (nul)
    bytes.push_back(std::byte{0});
}

static Bytes greeting(bool tls = true, bool modern = false)
{
  const weave::u32 capabilities = 1 | 4 | 8 | 512 | (tls ? 2048 : 0) | 8192 | 32768 | 524288 | (modern ? 1 << 24 : 0);
  Bytes bytes{std::byte{10}};
  text(bytes, "8.4-fixture", true);
  put(bytes, 123, 4);
  const std::string_view nonce = "0123456789abcdefghij";
  auto data = std::as_bytes(std::span{nonce});
  bytes.insert(bytes.end(), data.begin(), data.begin() + 8);
  put(bytes, 0, 1);
  put(bytes, capabilities, 2);
  put(bytes, 45, 1);
  put(bytes, 2, 2);
  put(bytes, capabilities >> 16, 2);
  put(bytes, 21, 1);
  bytes.resize(bytes.size() + 10, std::byte{0});
  bytes.insert(bytes.end(), data.begin() + 8, data.end());
  put(bytes, 0, 1);
  text(bytes, "caching_sha2_password", true);
  return bytes;
}

static Bytes column(std::string_view name, weave::u8 type = 253, weave::u16 flags = 0)
{
  Bytes bytes;
  text(bytes, "def");
  text(bytes, "");
  text(bytes, "");
  text(bytes, "");
  text(bytes, name);
  text(bytes, "");
  put(bytes, 12, 1);
  put(bytes, 45, 2);
  put(bytes, 128, 4);
  put(bytes, type, 1);
  put(bytes, flags, 2);
  put(bytes, 0, 1);
  put(bytes, 0, 2);
  return bytes;
}

static Bytes sql_error()
{
  Bytes bytes{std::byte{0xff}};
  put(bytes, 1054, 2);
  const std::string_view message = "#42S22Missing fixture column";
  auto data = std::as_bytes(std::span{message});
  bytes.insert(bytes.end(), data.begin(), data.end());
  return bytes;
}

static Bytes hex(std::string_view text)
{
  Bytes bytes;
  for (std::size_t index = 0; index < text.size(); index += 2) {
    unsigned value = 0;
    std::from_chars(text.data() + index, text.data() + index + 2, value, 16);
    bytes.push_back(std::byte{static_cast<weave::u8>(value)});
  }
  return bytes;
}

template <class S>
struct Peer {
  S &stream;
  weave::u8 sequence = 0;
  bool coalesced = false;
  Bytes queued;

  weave::Task<void> flush()
  {
    if (!queued.empty()) {
      co_await stream.write_all(queued);
      queued.clear();
    }
  }

  weave::Task<void> send(Bytes bytes)
  {
    Bytes header;
    put(header, bytes.size(), 3);
    put(header, sequence++, 1);
    if (coalesced) {
      queued.insert(queued.end(), header.begin(), header.end());
      queued.insert(queued.end(), bytes.begin(), bytes.end());
      co_return;
    }
    // Fragment headers deliberately; TCP/TLS record boundaries must not affect framing.
    for (const auto &byte : header)
      co_await stream.write_all(std::span{&byte, 1});
    co_await stream.write_all(bytes);
  }

  weave::Task<Bytes> receive()
  {
    co_await flush();
    std::array<std::byte, 4> header{};
    co_await stream.read_exactly(header);
    const auto size = std::to_integer<unsigned>(header[0]) | (std::to_integer<unsigned>(header[1]) << 8) |
      (std::to_integer<unsigned>(header[2]) << 16);
    CHECK(header[3] == std::byte{sequence++});
    if (size > 65536)
      co_await weave::fail(std::errc::message_size);
    Bytes bytes(size);
    co_await stream.read_exactly(bytes);
    co_return bytes;
  }
};

struct PendingWriteControl {
  weave::Channel<int> ready{1};
  weave::Channel<int> release{1};
};

template <class S>
static weave::Task<void> protocol(
  Peer<S> &peer,
  Mode mode,
  weave::Channel<int> &pending,
  bool secure,
  bool modern,
  PendingWriteControl *write_control = nullptr)
{
  auto response = co_await peer.receive();
  REQUIRE(response.size() > 33);
  if (response.size() <= 33)
    co_return;
  weave::u32 capabilities = 0;
  for (std::size_t index = 0; index < sizeof(capabilities); ++index)
    capabilities |= static_cast<weave::u32>(std::to_integer<weave::u8>(response[index])) << (8 * index);
  CHECK(((capabilities & (1 << 24)) != 0) == modern);
  auto name_end = std::find(response.begin() + 32, response.end(), std::byte{0});
  REQUIRE(name_end != response.end());
  if (name_end == response.end())
    co_return;
  const auto proof_offset = static_cast<std::size_t>(name_end - response.begin()) + 1;
  REQUIRE(proof_offset + 33 <= response.size());
  if (proof_offset + 33 > response.size())
    co_return;
  CHECK(response[proof_offset] == std::byte{32});
  auto expected = hex("c37f6723974ee52bf4291f82f794af9cd1cd28aa56c12ac045ba44585db42dac");
  CHECK(std::ranges::equal(std::span{response}.subspan(proof_offset + 1, 32), expected));

  if (mode == Mode::switch_native || mode == Mode::switch_caching) {
    const bool native = mode == Mode::switch_native;
    Bytes switched{std::byte{0xfe}};
    text(switched, native ? "mysql_native_password" : "caching_sha2_password", true);
    const std::string_view nonce = "0123456789abcdefghij";
    auto data = std::as_bytes(std::span{nonce});
    switched.insert(switched.end(), data.begin(), data.end());
    put(switched, 0, 1);
    co_await peer.send(std::move(switched));
    response = co_await peer.receive();
    if (native)
      expected = hex("255aaa63ad48cc7cc1831a5cb59328936b8bd068");
    CHECK(response == expected);
  }

  if (mode == Mode::full) {
    co_await peer.send({std::byte{1}, std::byte{4}});
    if (!secure)
      co_return;
    response = co_await peer.receive();
    const std::string_view password = "fixture-password";
    auto data = std::as_bytes(std::span{password});
    CHECK(response.size() == password.size() + 1);
    CHECK(std::ranges::equal(std::span{response}.first(password.size()), data));
    CHECK(response.back() == std::byte{0});
  } else if (mode != Mode::switch_native) {
    co_await peer.send({std::byte{1}, std::byte{3}});
    if (mode == Mode::repeated_auth) {
      co_await peer.send({std::byte{1}, std::byte{4}});
      co_return;
    }
  }
  const Bytes ok{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}};
  auto eof = modern ? ok : Bytes{std::byte{0xfe}, std::byte{0}, std::byte{0}, std::byte{2}, std::byte{0}};
  if (modern) {
    eof.front() = std::byte{0xfe};
    const std::string_view info = "fixture row completion";
    auto data = std::as_bytes(std::span{info});
    eof.insert(eof.end(), data.begin(), data.end());
  }
  co_await peer.send(ok);

  if (mode == Mode::deferred_cancel) {
    co_await pending.send(1);
    peer.sequence = 0;
    response = co_await peer.receive();
    Bytes recovered{std::byte{3}};
    const std::string_view sql = "SELECT recovered";
    auto data = std::as_bytes(std::span{sql});
    recovered.insert(recovered.end(), data.begin(), data.end());
    CHECK(response == recovered);
    co_await peer.send(ok);
    peer.sequence = 0;
    response = co_await peer.receive();
    CHECK(response == Bytes{std::byte{1}});
    co_return;
  }

  const bool prepared = mode == Mode::binary || mode == Mode::schema_flags || mode == Mode::binary_truncated ||
    mode == Mode::binary_bitmap || mode == Mode::prepared_error || mode == Mode::prepared_cancel ||
    mode == Mode::stream_abandon || mode == Mode::prepare_metadata_error || mode == Mode::stream_sql_error ||
    mode == Mode::pending_write_cancel;
  if (prepared) {
    peer.sequence = 0;
    response = co_await peer.receive();
    REQUIRE(!response.empty());
    if (response.empty())
      co_return;
    CHECK(response[0] == std::byte{22});
    Bytes prepared_ok{std::byte{0}};
    put(prepared_ok, 7, 4);
    put(prepared_ok, 2, 2);
    put(prepared_ok, 1, 2);
    put(prepared_ok, 0, 1);
    put(prepared_ok, 0, 2);
    co_await peer.send(std::move(prepared_ok));
    if (mode == Mode::prepare_metadata_error) {
      co_await peer.send(sql_error());
      co_return;
    }
    co_await peer.send(column("parameter", 8));
    if (!modern)
      co_await peer.send(eof);
    co_await peer.send(column("id", 8));
    co_await peer.send(column("absent"));
    if (!modern)
      co_await peer.send(eof);
    if (mode == Mode::pending_write_cancel) {
      REQUIRE(write_control != nullptr);
      if (!write_control)
        co_return;
      co_await write_control->ready.send(1);
      co_await write_control->release.receive();
      co_return;
    }
    peer.sequence = 0;
    response = co_await peer.receive();
    Bytes encoded;
    put(encoded, 23, 1);
    put(encoded, 7, 4);
    put(encoded, 0, 1);
    put(encoded, 1, 4);
    put(encoded, 0, 1);
    put(encoded, 1, 1);
    put(encoded, 8, 1);
    put(encoded, 0, 1);
    put(encoded, static_cast<weave::u64>(-42), 8);
    CHECK(response == encoded);
    if (mode == Mode::prepared_cancel) {
      co_await pending.send(1);
      std::array<std::byte, 1> buffer;
      co_await peer.stream.read(buffer);
      co_return;
    }
    if (mode == Mode::prepared_error) {
      co_await peer.send(sql_error());
    } else {
      co_await peer.send({std::byte{2}});
      co_await peer.send(column("id", 8));
      co_await peer.send(column("absent"));
      if (!modern)
        co_await peer.send(eof);
      Bytes row{std::byte{0}, std::byte{8}};
      put(row, static_cast<weave::u64>(-42), 8);
      if (mode == Mode::binary_truncated)
        row.pop_back();
      if (mode == Mode::binary_bitmap)
        row[1] |= std::byte{1};
      co_await peer.send(std::move(row));
      if (mode == Mode::binary_truncated || mode == Mode::binary_bitmap || mode == Mode::stream_abandon)
        co_return;
      co_await peer.send(mode == Mode::stream_sql_error ? sql_error() : eof);
    }
    if (mode == Mode::schema_flags) {
      peer.sequence = 0;
      response = co_await peer.receive();
      CHECK(response == encoded);
      co_await peer.send({std::byte{2}});
      co_await peer.send(column("id", 8, 32));
      co_await peer.send(column("absent"));
      if (!modern)
        co_await peer.send(eof);
      Bytes row{std::byte{0}, std::byte{8}};
      put(row, (std::numeric_limits<weave::u64>::max)(), 8);
      co_await peer.send(std::move(row));
      co_await peer.send(eof);
    }
    peer.sequence = 0;
    response = co_await peer.receive();
    CHECK((response.size() == 5 && response[0] == std::byte{25}));
    peer.sequence = 0;
    response = co_await peer.receive();
    CHECK((!response.empty() && response[0] == std::byte{3}));
    co_await peer.send(ok);
    peer.sequence = 0;
    response = co_await peer.receive();
    CHECK(response == Bytes{std::byte{1}});
    co_return;
  }

  peer.sequence = 0;
  response = co_await peer.receive();
  REQUIRE(!response.empty());
  if (response.empty())
    co_return;
  CHECK(response[0] == std::byte{3});
  if (mode == Mode::cancelled) {
    co_await pending.send(1);
    std::array<std::byte, 1> buffer;
    co_await peer.stream.read(buffer);
    co_return;
  }
  if (mode == Mode::truncated) {
    const std::array header{std::byte{5}, std::byte{0}};
    co_await peer.stream.write_all(header);
    co_return;
  }
  if (mode == Mode::oversized) {
    const std::array header{std::byte{1}, std::byte{4}, std::byte{0}, std::byte{1}};
    co_await peer.stream.write_all(header);
    co_return;
  }
  if (mode == Mode::wrong_sequence) {
    ++peer.sequence;
    co_await peer.send(ok);
    co_return;
  }
  if (mode == Mode::local_file) {
    co_await peer.send({std::byte{0xfb}, std::byte{'x'}});
    co_return;
  }
  if (mode == Mode::sql_error) {
    co_await peer.send(sql_error());
  } else {
    const std::array
      names{"id", "name", "absent", "blank", "flag", "max", "negative", "number", "bad", "dup", "dup", "bits"};
    co_await peer.send({std::byte{static_cast<weave::u8>(names.size())}});
    for (const auto *name : names) {
      auto definition = column(name, std::string_view{name} == "bits" ? 16 : 253);
      if (mode == Mode::malformed_column)
        definition.pop_back();
      co_await peer.send(std::move(definition));
      if (mode == Mode::malformed_column)
        co_return;
    }
    if (!modern)
      co_await peer.send(eof);

    Bytes row;
    text(row, "42");
    text(row, "shipment");
    put(row, 0xfb, 1);
    text(row, "");
    text(row, "1");
    text(row, "18446744073709551615");
    text(row, "-1");
    text(row, "12.75");
    text(row, "3x");
    text(row, "a");
    text(row, "b");
    text(row, "1");
    if (mode == Mode::malformed_row)
      row.pop_back();
    co_await peer.send(row);
    if (mode == Mode::wrapped_sequence) {
      for (int index = 1; index < 300; ++index)
        co_await peer.send(row);
    }
    if (mode == Mode::malformed_row || mode == Mode::byte_limit)
      co_return;
    if (mode == Mode::row_limit) {
      co_await peer.send(row);
      co_return;
    }
    if (mode == Mode::malformed_end || mode == Mode::eof_counts) {
      auto invalid = eof;
      if (mode == Mode::malformed_end)
        invalid.resize(3);
      else
        invalid[1] = std::byte{1};
      co_await peer.send(std::move(invalid));
      co_return;
    }
    co_await peer.send(mode == Mode::mid_error ? sql_error() : eof);
  }

  peer.sequence = 0;
  response = co_await peer.receive();
  CHECK(!response.empty());
  if (response.empty())
    co_return;
  CHECK(response[0] == std::byte{3});
  co_await peer.send(ok);
  peer.sequence = 0;
  response = co_await peer.receive();
  CHECK(response == Bytes{std::byte{1}});
}

static weave::Task<void> server(
  weave::TcpListener &listener,
  Mode mode,
  weave::Channel<int> &pending,
  const weave::TlsContext *tls,
  bool coalesced = false,
  bool modern = false,
  PendingWriteControl *write_control = nullptr)
{
  auto stream = co_await listener.accept({.no_delay = true});
  Peer peer{stream, 0, coalesced};
  co_await peer.send(greeting(mode != Mode::no_tls, modern));
  if (tls) {
    auto request = co_await peer.receive();
    CHECK(request.size() == 32);
    auto secure = co_await weave::tls::server(std::move(stream), *tls);
    Peer encrypted{secure, peer.sequence, coalesced};
    co_await protocol(encrypted, mode, pending, true, modern, write_control);
    co_await encrypted.flush();
    if (mode == Mode::fast || mode == Mode::full)
      co_await secure.shutdown_send();
  } else {
    co_await protocol(peer, mode, pending, false, modern, write_control);
    co_await peer.flush();
  }
}

static weave::Task<void> busy(weave::mysql::Connection &db, weave::Channel<int> &pending)
{
  co_await pending.receive();
  auto result = co_await weave::as_result(db.execute("SELECT busy"));
  CHECK(!result);
  if (!result)
    CHECK(result.error() == weave::mysql::ClientError::busy);
  CHECK(!db.close());
  CHECK(!db.last_failure());
  CHECK(!db.tls_info());
}

static weave::Task<std::optional<weave::mysql::Row>> retain_streamed_row(
  weave::mysql::Statement owner,
  weave::mysql::Connection &connection,
  std::span<const weave::mysql::ParameterView> values)
{
  std::optional<weave::mysql::Row> retained;
  {
    auto stream = co_await owner.stream_borrowed(values);
    CHECK(!connection.close());
    retained = co_await stream.next();
    REQUIRE(retained);
    if (!retained)
      co_return std::nullopt;
  }

  co_await owner.close();
  co_return std::move(retained);
}

static weave::Task<void> client(
  weave::u16 port,
  Mode mode,
  weave::Channel<int> &pending,
  const weave::TlsContext *tls,
  bool modern,
  bool borrowed_query = false)
{
  weave::mysql::Options options{.port = port, .user = "weave", .password = "fixture-password"};
  options.plaintext = !tls;
  if (tls)
    options.tls = *tls;
  if (mode == Mode::oversized)
    options.max_packet_bytes = 1024;
  if (mode == Mode::row_limit)
    options.max_rows = 1;
  if (mode == Mode::byte_limit)
    options.max_result_bytes = 128;

  auto connected = co_await weave::as_result(weave::mysql::connect(std::move(options)));
  if (mode == Mode::repeated_auth || mode == Mode::no_tls || (mode == Mode::full && !tls)) {
    CHECK(!connected);
    if (!connected) {
      const bool requires_tls = mode == Mode::full || mode == Mode::no_tls;
      auto expected = requires_tls ? weave::mysql::ClientError::tls_required : weave::mysql::ClientError::protocol;
      CHECK(connected.error() == expected);
    }
    co_return;
  }
  REQUIRE(connected);
  if (!connected)
    co_return;
  auto &db = *connected;
  auto info = db.tls_info();
  CHECK((info && info->has_value() == bool(tls)));

  const bool prepared = mode == Mode::binary || mode == Mode::schema_flags || mode == Mode::binary_truncated ||
    mode == Mode::binary_bitmap || mode == Mode::prepared_error || mode == Mode::prepared_cancel ||
    mode == Mode::stream_abandon || mode == Mode::prepare_metadata_error || mode == Mode::stream_sql_error;
  if (prepared) {
    if (mode == Mode::prepare_metadata_error) {
      auto result = co_await weave::as_result(db.prepare("SELECT ?"));
      CHECK(!result);
      CHECK(!db.open());
      co_return;
    }
    auto statement = co_await db.prepare("SELECT ? AS id, NULL AS absent");
    CHECK(statement.parameters().size() == 1);
    CHECK(statement.columns().size() == 2);
    const std::array<weave::mysql::ParameterView, 1> parameters{-42};
    if (mode == Mode::stream_abandon) {
      auto retained = co_await retain_streamed_row(std::move(statement), db, parameters);
      REQUIRE(retained);
      if (!retained)
        co_return;
      CHECK(!db.open());
      connected = std::unexpected(weave::mysql::make_error_code(weave::mysql::ClientError::closed));
      CHECK(!connected);
      CHECK((*retained)["id"].as<int>() == -42);
      CHECK((*retained)["absent"].is_null() == true);
      co_return;
    }
    if (mode == Mode::stream_sql_error) {
      {
        auto stream = co_await statement.stream_borrowed(parameters);
        CHECK(!db.close());
        auto row = co_await stream.next();
        REQUIRE(row);
        if (row)
          CHECK((*row)["id"].as<int>() == -42);
        auto failed = co_await weave::as_result(stream.next());
        CHECK(!failed);
        CHECK(db.open());
        CHECK(!stream.summary());
      }
    } else {
      auto wrong = co_await weave::as_result(statement.execute_borrowed({}));
      CHECK(!wrong);
      CHECK(db.open());
      auto task = statement.execute_borrowed(parameters);
      if (mode == Mode::prepared_cancel) {
        auto failed = co_await weave::as_result(weave::timeout(50ms, std::move(task)));
        CHECK(!failed);
        if (!failed)
          CHECK(failed.error() == std::errc::timed_out);
        CHECK(!db.open());
        co_return;
      }
      auto result = co_await weave::as_result(std::move(task));
      if (mode == Mode::binary || mode == Mode::schema_flags) {
        REQUIRE((result && result->rows.size() == 1));
        if (result && result->rows.size() == 1) {
          if (modern)
            CHECK(result->info == "fixture row completion");
          CHECK(result->rows[0]["id"].as<int>() == -42);
          CHECK(!result->rows[0]["id"].as<unsigned>());
          CHECK(!result->rows[0]["id"].as<std::string_view>());
          CHECK(result->rows[0]["absent"].is_null() == true);
        }
        if (mode == Mode::schema_flags && result && result->rows.size() == 1) {
          result->columns[0].flags = 32;
          auto changed = co_await statement.execute_borrowed(parameters);
          REQUIRE(changed.rows.size() == 1);
          if (changed.rows.size() == 1) {
            CHECK(changed.rows[0]["id"].as<weave::u64>() == (std::numeric_limits<weave::u64>::max)());
            CHECK(!changed.rows[0]["id"].as<weave::i64>());
          }
          CHECK(result->rows[0]["id"].as<int>() == -42);
          CHECK(!result->rows[0]["id"].as<weave::u64>());
        }
      } else {
        CHECK(!result);
        CHECK(db.open() == (mode == Mode::prepared_error));
        if (mode != Mode::prepared_error)
          co_return;
      }
    }
    co_await statement.close();
    co_await db.execute("SELECT recovered");
    co_await db.disconnect();
    co_return;
  }

  if (mode == Mode::cancelled) {
    auto query = [&]() -> weave::Task<void> {
      auto result = co_await weave::as_result(weave::timeout(50ms, db.execute("SELECT pending")));
      CHECK(!result);
      if (!result)
        CHECK(result.error() == std::errc::timed_out);
    };
    co_await weave::when_all(query(), busy(db, pending));
    CHECK(!db.open());
    auto retry = co_await weave::as_result(db.execute("SELECT retry"));
    CHECK(!retry);
    co_return;
  }

  if (borrowed_query) {
    auto failed = co_await weave::as_result(db.query_borrowed("SELECT fixture"));
    REQUIRE(!failed);
    if (failed)
      co_return;
    const auto expected = weave::mysql::server_error(1054);
    CHECK(failed.error() == expected);
    CHECK(&failed.error().category() == &expected.category());
    auto diagnostic = db.last_failure();
    REQUIRE((diagnostic && *diagnostic));
    if (!diagnostic || !*diagnostic)
      co_return;
    CHECK((**diagnostic).sqlstate == "42S22");
    CHECK(db.open());

    auto recovered = co_await weave::as_result(db.query_borrowed("SELECT recovered"));
    REQUIRE((recovered && recovered->size() == 1));
    if (!recovered || recovered->size() != 1)
      co_return;
    CHECK((*recovered)[0].rows.empty());
    auto cleared = db.last_failure();
    REQUIRE(cleared);
    if (!cleared)
      co_return;
    CHECK(!*cleared);
    co_await db.disconnect();
    CHECK(!db.open());
    co_return;
  }

  auto result = co_await weave::as_result(db.execute("SELECT fixture"));
  const bool sql_failure = mode == Mode::sql_error || mode == Mode::mid_error;
  const bool normal = mode == Mode::fast || mode == Mode::full || mode == Mode::switch_native ||
    mode == Mode::switch_caching || mode == Mode::wrapped_sequence;
  if (!normal && !sql_failure) {
    CHECK(!result);
    CHECK(!db.open());
    if (!result) {
      const bool limited = mode == Mode::oversized || mode == Mode::row_limit || mode == Mode::byte_limit;
      if (limited)
        CHECK(result.error() == weave::mysql::ClientError::resource_limit);
      else if (mode != Mode::truncated)
        CHECK(result.error() == weave::mysql::ClientError::protocol);
    }
    co_return;
  }
  if (normal && modern && result)
    CHECK(result->info == "fixture row completion");
  if (sql_failure) {
    CHECK(!result);
    if (!result)
      CHECK(result.error() == weave::mysql::server_error(1054));
    auto failure = db.last_failure();
    REQUIRE((failure && *failure));
    if (failure && *failure)
      CHECK((**failure).sqlstate == "42S22");
  } else {
    const std::size_t expected_rows = mode == Mode::wrapped_sequence ? 300 : 1;
    REQUIRE((result && result->rows.size() == expected_rows));
    if (!result || result->rows.size() != expected_rows)
      co_return;
    const auto &row = result->rows[0];
    auto shipment = row.as<Shipment>();
    REQUIRE(shipment);
    if (shipment) {
      CHECK(shipment->id == 42);
      CHECK(shipment->name == "shipment");
    }
    CHECK(row[0].as<int>() == 42);

    {
      const auto missing_error = weave::mysql::make_error_code(weave::mysql::ClientError::missing_column);
      const auto ambiguous_error = weave::mysql::make_error_code(weave::mysql::ClientError::ambiguous_column);
      auto check_selection_error = [](const weave::mysql::Field &field, std::error_code expected) {
        auto value = field.as<int>();
        CHECK(!value);
        if (!value) {
          CHECK(value.error() == expected);
          CHECK(&value.error().category() == &expected.category());
        }
      };

      auto valid = row[0];
      auto valid_copy = valid;
      CHECK(valid_copy.as<int>() == 42);
      auto valid_move = std::move(valid_copy);
      CHECK(valid_move.as<int>() == 42);

      auto missing = row["missing"];
      auto missing_copy = missing;
      check_selection_error(missing_copy, missing_error);
      auto missing_move = std::move(missing_copy);
      check_selection_error(missing_move, missing_error);

      valid = missing;
      check_selection_error(valid, missing_error);
      missing = valid_move;
      CHECK(missing.as<int>() == 42);
      valid_move = std::move(missing_move);
      check_selection_error(valid_move, missing_error);
      missing_move = std::move(missing);
      CHECK(missing_move.as<int>() == 42);

      auto ambiguous = row["dup"];
      valid = ambiguous;
      check_selection_error(valid, ambiguous_error);
      ambiguous = std::move(valid_move);
      check_selection_error(ambiguous, missing_error);
    }

    auto absent = row["absent"].as<std::optional<int>>();
    CHECK((absent && !*absent));
    CHECK(!row["absent"].as<int>());
    CHECK(row["blank"].as<std::string_view>() == "");
    CHECK(row["flag"].as<bool>() == true);
    CHECK(row["max"].as<weave::u64>() == (std::numeric_limits<weave::u64>::max)());
    CHECK(!row["max"].as<weave::i64>());
    CHECK(!row["negative"].as<weave::u32>());
    CHECK(row["number"].as<double>() == 12.75);
    CHECK(!row["bad"].as<int>());
    CHECK(!row["bits"].as<int>());
    CHECK(row["bits"].as<std::string_view>() == "1");
    CHECK(!row["missing"].as<std::string>());
    CHECK(!row[99].as<int>());
    auto duplicate = row["dup"].as<std::string>();
    CHECK(!duplicate);
    if (!duplicate)
      CHECK(duplicate.error() == weave::mysql::ClientError::ambiguous_column);
    auto retained = row;
    result->rows.clear();
    result->columns.clear();
    CHECK(retained["name"].as<std::string_view>() == "shipment");
    CHECK(retained["max"].as<weave::u64>() == (std::numeric_limits<weave::u64>::max)());
    CHECK(retained["absent"].is_null() == true);
    auto moved = std::move(retained);
    CHECK(moved["max"].bytes() == "18446744073709551615");
  }

  CHECK(db.open());
  auto recovered = co_await db.execute_borrowed("SELECT recovered");
  CHECK(recovered.rows.empty());
  auto failure = db.last_failure();
  CHECK((failure && !*failure));
  co_await db.disconnect();
  CHECK(!db.open());
  auto deferred = db.execute("SELECT closed");
  auto closed = co_await weave::as_result(std::move(deferred));
  CHECK(!closed);
  if (!closed)
    CHECK(closed.error() == weave::mysql::ClientError::closed);
}

static weave::Task<void> exercise(
  Mode mode,
  const weave::TlsContext *server_tls = nullptr,
  const weave::TlsContext *client_tls = nullptr,
  bool coalesced = false,
  bool modern = false,
  bool borrowed_query = false)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  weave::Channel<int> pending{1};
  auto expected_disconnect = [&]() -> weave::Task<void> {
    static_cast<void>(co_await weave::as_result(server(listener, mode, pending, server_tls, coalesced, modern)));
  };
  co_await weave::when_all(
    expected_disconnect(),
    client(listener.local_port(), mode, pending, client_tls, modern, borrowed_query));
}

static weave::Task<void> deferred_cancellation(weave::Context &ctx)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  weave::Channel<int> pending{1};
  auto peer = [&]() -> weave::Task<void> {
    co_await server(listener, Mode::deferred_cancel, pending, nullptr);
  };
  auto query = [&]() -> weave::Task<void> {
    auto connected = co_await weave::as_result(
      weave::mysql::connect(
        {.port = listener.local_port(), .user = "weave", .password = "fixture-password", .plaintext = true}));
    REQUIRE(connected);
    if (!connected)
      co_return;
    auto &db = *connected;
    co_await pending.receive();

    const std::array borrowing{false, true};
    for (bool borrowed : borrowing) {
      auto deferred = borrowed ? db.query_borrowed("SELECT cancelled borrowed") : db.query("SELECT cancelled owned");
      CHECK(db.open());
      const auto before = ctx.metrics();
      auto attempt = [&](weave::TaskScope &children) -> weave::Task<void> {
        children.cancel();
        auto job = children.spawn(std::move(deferred));
        REQUIRE(job);
        if (!job)
          co_return;
        static_cast<void>(co_await weave::as_result(children.join()));
      };
      auto cancelled = co_await weave::as_result(weave::scope(attempt));
      CHECK(!cancelled);
      if (!cancelled)
        CHECK(cancelled.error() == std::errc::operation_canceled);
      const auto after = ctx.metrics();
      CHECK(after.submitted == before.submitted);
      CHECK(after.write_calls == before.write_calls);
      CHECK(db.open());
    }

    auto recovered = co_await db.query_borrowed("SELECT recovered");
    REQUIRE(recovered.size() == 1);
    if (recovered.size() != 1)
      co_return;
    CHECK(recovered[0].rows.empty());
    co_await db.disconnect();
  };
  co_await weave::when_all(peer(), query());
}

TEST_CASE("MySQL pre-cancelled deferred queries send no command and leave the session reusable")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  if (!ctx)
    return;
  CHECK(ctx->run(weave::timeout(5s, deferred_cancellation(*ctx))));
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
}

static weave::Task<void> pending_write_cancellation(weave::Context &ctx)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  weave::Channel<int> pending{1};
  PendingWriteControl control;
  auto peer = [&]() -> weave::Task<void> {
    co_await server(listener, Mode::pending_write_cancel, pending, nullptr, false, false, &control);
  };
  auto client = [&]() -> weave::Task<void> {
    auto connected = co_await weave::as_result(
      weave::mysql::connect(
        {.port = listener.local_port(), .user = "weave", .password = "fixture-password", .plaintext = true}));
    REQUIRE(connected);
    if (!connected)
      co_return;
    auto &db = *connected;
    auto prepared = co_await weave::as_result(db.prepare("SELECT ? AS id, NULL AS absent"));
    REQUIRE(prepared);
    if (!prepared)
      co_return;
    auto &statement = *prepared;
    co_await control.ready.receive();

    bool selected = false;
    auto operation = [&](weave::TaskScope &children) -> weave::Task<void> {
      struct WriteState {
        weave::Context::Metrics before;
        bool writing = false;
        bool inline_seen = false;
        bool destroyed = false;
      } state;
      constexpr std::size_t attempt_limit = 4096;
      auto write = [&]() -> weave::Task<void> {
        struct Guard {
          WriteState &state;
          ~Guard()
          {
            state.writing = false;
            state.destroyed = true;
          }
        } guard{state};
        const std::array<std::byte, 4096> bytes{};
        for (std::size_t attempt = 0; attempt < attempt_limit; ++attempt) {
          state.before = ctx.metrics();
          state.writing = true;
          co_await statement.send_long_data(0, bytes);
          state.writing = false;
          const auto after = ctx.metrics();
          state.inline_seen = state.inline_seen || after.inline_completions > state.before.inline_completions;
          co_await ctx.yield();
        }
      };
      auto job = children.spawn(write());
      REQUIRE(job);
      if (!job)
        co_return;

      for (std::size_t turn = 0; turn < attempt_limit * 4 && !job->ready(); ++turn) {
        const auto current = ctx.metrics();
        // The channel-paused peer has no native operation; exclude inline fairness completions.
        if (state.writing && state.inline_seen && state.before.submitted == state.before.completed &&
          current.submitted > state.before.submitted && current.submitted == current.completed + 1 &&
          current.fairness_posts == state.before.fairness_posts) {
          selected = true;
          break;
        }
        co_await ctx.yield();
      }
      REQUIRE(selected);
      if (!selected) {
        children.cancel();
        static_cast<void>(co_await weave::as_result(children.join()));
        co_return;
      }

      CHECK(!state.destroyed);
      children.cancel();
      auto drained = co_await weave::as_result(children.join());
      CHECK(!drained);
      if (!drained)
        CHECK(drained.error() == std::errc::operation_canceled);
      CHECK(state.destroyed);
      CHECK(ctx.metrics().submitted == ctx.metrics().completed);
      CHECK(!db.open());
    };
    auto cancelled = co_await weave::as_result(weave::scope(operation));
    CHECK(!cancelled);
    if (!cancelled)
      CHECK(cancelled.error() == std::errc::operation_canceled);
    if (!selected) {
      co_await control.release.send(1);
      co_return;
    }
    auto retry = co_await weave::as_result(statement.send_long_data(0, {}));
    CHECK(!retry);
    if (!retry)
      CHECK(retry.error() == weave::mysql::ClientError::closed);
    co_await control.release.send(1);
  };
  co_await weave::when_all(peer(), client());
}

TEST_CASE("MySQL cancelling a staged prepared packet drains its pending native write and closes the session")
{
  auto ctx = weave::Context::create({.skip_successful_completions = true});
  REQUIRE(ctx);
  if (!ctx)
    return;
  CHECK(ctx->run(weave::timeout(5s, pending_write_cancellation(*ctx))));
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
}

TEST_CASE("MySQL text protocol validates framing, rows, SQL recovery and cancellation")
{
  const std::array modes{
    Mode::fast,
    Mode::full,
    Mode::switch_native,
    Mode::switch_caching,
    Mode::wrong_sequence,
    Mode::malformed_column,
    Mode::malformed_row,
    Mode::sql_error,
    Mode::mid_error,
    Mode::local_file,
    Mode::oversized,
    Mode::repeated_auth,
    Mode::truncated,
    Mode::cancelled,
    Mode::row_limit,
    Mode::byte_limit,
    Mode::wrapped_sequence};

  for (auto mode : modes) {
    CAPTURE(static_cast<int>(mode));
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    if (!ctx)
      continue;
    CHECK(ctx->run(weave::timeout(5s, exercise(mode))));
  }
}

TEST_CASE("MySQL borrowed queries recover from a coalesced mid-row SQL error")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  if (!ctx)
    return;
  CHECK(ctx->run(weave::timeout(5s, exercise(Mode::mid_error, nullptr, nullptr, true, false, true))));
}

TEST_CASE("MySQL prepared execution validates binary rows, encoded parameters and stream cleanup")
{
  const std::array modes{
    Mode::binary,
    Mode::schema_flags,
    Mode::binary_truncated,
    Mode::binary_bitmap,
    Mode::prepared_error,
    Mode::prepared_cancel,
    Mode::prepare_metadata_error,
    Mode::stream_abandon,
    Mode::stream_sql_error};
  for (auto mode : modes) {
    CAPTURE(static_cast<int>(mode));
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    if (ctx)
      CHECK(ctx->run(weave::timeout(5s, exercise(mode))));
  }
}

TEST_CASE("MySQL read-ahead preserves owning rows, sequence wrap, bounds and SQL recovery")
{
  const std::array modes{
    Mode::fast,
    Mode::wrapped_sequence,
    Mode::wrong_sequence,
    Mode::malformed_column,
    Mode::malformed_row,
    Mode::row_limit,
    Mode::byte_limit,
    Mode::mid_error,
    Mode::binary,
    Mode::schema_flags,
    Mode::binary_truncated,
    Mode::binary_bitmap,
    Mode::stream_abandon,
    Mode::stream_sql_error};
  for (auto mode : modes) {
    CAPTURE(static_cast<int>(mode));
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    if (ctx)
      CHECK(ctx->run(weave::timeout(5s, exercise(mode, nullptr, nullptr, true))));
  }
}

TEST_CASE("MySQL upgrades inside protocol to verified TLS; full and fast authentication work")
{
  fixture::Certificates certificates;
  auto server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  auto client = weave::TlsContext::client({.ca_file = certificates.ca});
  REQUIRE((server && client));
  if (!server || !client)
    return;

  const std::array modes{Mode::fast, Mode::full, Mode::cancelled, Mode::no_tls};
  for (auto mode : modes) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    if (ctx)
      CHECK(ctx->run(weave::timeout(5s, exercise(mode, &*server, &*client))));
  }
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  if (ctx)
    CHECK(ctx->run(weave::timeout(5s, exercise(Mode::wrapped_sequence, &*server, &*client, true))));
}

TEST_CASE("MySQL negotiated EOF replacement validates long terminal OK packets and omitted metadata EOFs")
{
  fixture::Certificates certificates;
  auto server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  auto client = weave::TlsContext::client({.ca_file = certificates.ca});
  REQUIRE((server && client));
  if (!server || !client)
    return;

  const std::array modes{
    Mode::fast,
    Mode::wrapped_sequence,
    Mode::sql_error,
    Mode::mid_error,
    Mode::malformed_end,
    Mode::eof_counts,
    Mode::binary,
    Mode::schema_flags,
    Mode::binary_truncated,
    Mode::binary_bitmap,
    Mode::prepared_error,
    Mode::prepare_metadata_error,
    Mode::stream_abandon,
    Mode::stream_sql_error};
  const std::array batching{false, true};
  const std::array encryption{false, true};
  for (bool tls : encryption) {
    for (bool coalesced : batching) {
      for (auto mode : modes) {
        CAPTURE(tls);
        CAPTURE(coalesced);
        CAPTURE(static_cast<int>(mode));
        auto ctx = weave::Context::create();
        REQUIRE(ctx);
        if (ctx) {
          CHECK(ctx->run(
            weave::timeout(5s, exercise(mode, tls ? &*server : nullptr, tls ? &*client : nullptr, coalesced, true))));
        }
      }
    }
  }
}

static weave::Task<void> reject_certificate(const weave::TlsContext &server_tls, const weave::TlsContext &client_tls)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  weave::Channel<int> pending{1};
  auto peer = [&]() -> weave::Task<void> {
    static_cast<void>(co_await weave::as_result(server(listener, Mode::full, pending, &server_tls)));
  };
  auto rejected = [&]() -> weave::Task<void> {
    auto connected = co_await weave::as_result(
      weave::mysql::connect(
        {.port = listener.local_port(), .user = "weave", .password = "fixture-password", .tls = client_tls}));
    CHECK(!connected);
    if (!connected)
      CHECK(connected.error() == weave::TlsError::certificate_verification);
  };
  co_await weave::when_all(peer(), rejected());
}

TEST_CASE("MySQL never falls back to plaintext after certificate verification fails")
{
  fixture::Certificates certificates;
  auto server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  auto client = weave::TlsContext::client({.ca_file = certificates.untrusted});
  auto ctx = weave::Context::create();
  REQUIRE((server && client && ctx));
  if (server && client && ctx)
    CHECK(ctx->run(weave::timeout(5s, reject_certificate(*server, *client))));
}

TEST_CASE("MySQL rejects invalid configuration and unverified credential policies without connecting")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  if (!ctx)
    return;
  auto invalid = ctx->run(weave::mysql::connect({.user = ""}));
  CHECK(!invalid);
  if (!invalid)
    CHECK(invalid.error() == weave::mysql::ClientError::invalid_options);

  auto credentials = weave::TlsContext::client({.verification = weave::TlsVerification::none});
  REQUIRE(credentials);
  if (!credentials)
    return;
  auto unverified = ctx->run(weave::mysql::connect({.user = "weave", .tls = *credentials}));
  CHECK(!unverified);
  if (!unverified)
    CHECK(unverified.error() == weave::mysql::ClientError::tls_required);
}

#if defined(WEAVE_MYSQL_TEST_RUNTIME)
TEST_CASE("MySQL sessions work on four stealing runtime workers without a driver-specific API")
{
  auto runtime = weave::Runtime::create({.workers = 4, .scheduler = weave::Scheduler::work_stealing});
  REQUIRE(runtime);
  if (!runtime)
    return;
  const std::array modes{Mode::fast, Mode::switch_native, Mode::sql_error, Mode::cancelled};
  for (auto mode : modes)
    CHECK(runtime->run(weave::timeout(5s, exercise(mode))));
}
#endif
