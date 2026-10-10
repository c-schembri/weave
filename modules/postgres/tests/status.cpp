#include <weave/postgres/connection.hpp>
#include <doctest/doctest.h>
#include <array>
#include <atomic>
#include <thread>

namespace pg = weave::pg;

static_assert(std::same_as<decltype(pg::status_name(pg::ResultSet{})), std::string_view>);
static_assert(std::same_as<decltype(pg::status_name(pg::Outcome{})), std::string_view>);
static_assert(std::same_as<decltype(pg::status_name(pg::ExchangeEvent{})), std::string_view>);
static_assert(noexcept(pg::status_name(std::declval<const pg::PipelineResult &>())));

TEST_CASE("PostgreSQL status names inspect explicit kinds, never row counts or command tags")
{
  struct Kind {
    pg::ResultKind value;
    std::string_view name;
  };

  const std::array kinds{
    Kind{pg::ResultKind::uninitialized, "uninitialized"},
    Kind{pg::ResultKind::empty_query, "empty_query"},
    Kind{pg::ResultKind::command, "command"},
    Kind{pg::ResultKind::tuples, "tuples"},
    Kind{pg::ResultKind::description, "description"},
    Kind{pg::ResultKind::row_chunk, "row_chunk"},
    Kind{pg::ResultKind::acknowledgment, "acknowledgment"},
    Kind{static_cast<pg::ResultKind>(-1), "unknown"}};
  for (auto kind : kinds) {
    pg::ResultSet result;
    result.kind = kind.value;
    CHECK(pg::status_name(result) == kind.name);
    result.rows.emplace_back();
    result.columns.push_back({.name = "not a kind"});
    result.command = "SELECT 9000";
    result.suspended = true;
    CHECK(pg::status_name(result) == kind.name);
    CHECK(pg::status_name(pg::ExchangeEvent{result}) == kind.name);
  }

  std::string_view retained;
  {
    pg::ResultSet result;
    result.kind = pg::ResultKind::tuples;
    retained = pg::status_name(result);
  }
  CHECK(retained == "tuples");
}

TEST_CASE("PostgreSQL outcome status rejects conflicting envelopes without inspecting diagnostic text")
{
  const std::array<std::string_view, 8>
    names{"uninitialized", "tuples", "sql_error", "invalid", "aborted", "invalid", "invalid", "invalid"};
  for (unsigned flags = 0; flags < names.size(); ++flags) {
    pg::Outcome outcome;
    if (flags & 1) {
      outcome.result.emplace();
      outcome.result->kind = pg::ResultKind::tuples;
    }
    if (flags & 2)
      outcome.error.fields.emplace_back('M', "");
    outcome.aborted = (flags & 4) != 0;
    CHECK(pg::status_name(outcome) == names[flags]);
  }

  pg::Diagnostic diagnostic;
  CHECK(pg::status_name(diagnostic) == "uninitialized");
  diagnostic.fields.emplace_back('S', "NOTICE");
  CHECK(pg::status_name(diagnostic) == "diagnostic");
  diagnostic.fields.front().second = "FATAL";
  CHECK(pg::status_name(diagnostic) == "diagnostic");
}

TEST_CASE("PostgreSQL COPY status distinguishes direction, data, receive end and terminal command")
{
  const std::array directions{
    pg::CopyDirection::input,
    pg::CopyDirection::output,
    pg::CopyDirection::both,
    static_cast<pg::CopyDirection>(-1)};
  const std::array<std::string_view, 4> names{"copy_input", "copy_output", "copy_both", "unknown"};
  for (std::size_t index = 0; index < directions.size(); ++index) {
    pg::CopyFormat format;
    format.direction = directions[index];
    CHECK(pg::status_name(format) == names[index]);
    CHECK(pg::status_name(pg::ExchangeEvent{format}) == names[index]);
  }
  CHECK(pg::status_name(pg::ExchangeEvent{std::vector<std::byte>{}}) == "copy_data");
  CHECK(pg::status_name(pg::ExchangeEvent{pg::CopyDone{}}) == "copy_done");
  pg::ResultSet result;
  result.kind = pg::ResultKind::command;
  result.command = "COPY 0";
  CHECK(pg::status_name(pg::ExchangeEvent{result}) == "command");
}

TEST_CASE("PostgreSQL pipeline status checks barrier and chunk envelopes without claiming transaction success")
{
  pg::PipelineResult result;
  CHECK(pg::status_name(result) == "uninitialized");
  result.kind = pg::PipelineKind::sync;
  CHECK(pg::status_name(result) == "uninitialized");
  const std::array transactions{pg::Transaction::idle, pg::Transaction::active, pg::Transaction::failed};
  for (auto transaction : transactions) {
    result.transaction = transaction;
    CHECK(pg::status_name(result) == "pipeline_sync");
  }
  result.transaction = static_cast<pg::Transaction>(-1);
  CHECK(pg::status_name(result) == "invalid");
  result.transaction = pg::Transaction::idle;
  result.complete = false;
  CHECK(pg::status_name(result) == "invalid");
  result.complete = true;
  result.outcome.aborted = true;
  CHECK(pg::status_name(result) == "invalid");

  result = {};
  result.outcome.result.emplace();
  result.outcome.result->kind = pg::ResultKind::row_chunk;
  CHECK(pg::status_name(result) == "invalid");
  result.complete = false;
  CHECK(pg::status_name(result) == "row_chunk");
  result.transaction = pg::Transaction::idle;
  CHECK(pg::status_name(result) == "invalid");
  result.transaction.reset();
  result.kind = pg::PipelineKind::prepare;
  CHECK(pg::status_name(result) == "invalid");

  result.complete = true;
  result.outcome.result->kind = pg::ResultKind::acknowledgment;
  CHECK(pg::status_name(result) == "acknowledgment");
  result.kind = pg::PipelineKind::close;
  CHECK(pg::status_name(result) == "acknowledgment");
  result.kind = pg::PipelineKind::execute;
  CHECK(pg::status_name(result) == "invalid");
  result.kind = pg::PipelineKind::describe;
  result.outcome.result->kind = pg::ResultKind::description;
  CHECK(pg::status_name(result) == "description");
  result.kind = pg::PipelineKind::execute;
  CHECK(pg::status_name(result) == "invalid");
  result.outcome.result.reset();
  result.outcome.error.fields.emplace_back('C', "22012");
  CHECK(pg::status_name(result) == "sql_error");
  result.outcome.error.fields.clear();
  result.outcome.aborted = true;
  CHECK(pg::status_name(result) == "aborted");
  result.kind = static_cast<pg::PipelineKind>(-1);
  CHECK(pg::status_name(result) == "unknown");
}

class StatusForeignCategory final : public std::error_category {
public:
  mutable unsigned calls = 0;

  const char *name() const noexcept override
  {
    ++calls;
    return "postgres.sqlstate";
  }

  std::string message(int) const override
  {
    ++calls;
    return "private error";
  }

  bool equivalent(int, const std::error_condition &) const noexcept override
  {
    ++calls;
    return true;
  }
};

TEST_CASE("PostgreSQL error status uses category identity and never calls foreign category callbacks")
{
  struct Failure {
    std::error_code error;
    std::string_view name;
  };

  const std::array failures{
    Failure{{}, "success"},
    Failure{std::make_error_code(std::errc::operation_canceled), "canceled"},
    Failure{std::make_error_code(std::errc::connection_reset), "error"},
    Failure{pg::sql_error("22012"), "sql_error"},
    Failure{pg::Error::protocol, "protocol_error"},
    Failure{pg::Error::authentication, "authentication_error"},
    Failure{pg::Error::unsupported_authentication, "unsupported_authentication"},
    Failure{pg::Error::resource_limit, "resource_limit"},
    Failure{pg::Error::closed, "closed"},
    Failure{pg::Error::busy, "busy"},
    Failure{pg::Error::unexpected_copy, "unexpected_copy"},
    Failure{pg::Error::target_session, "target_session"},
    Failure{static_cast<pg::Error>(-1), "unknown"}};
  for (const auto &failure : failures)
    CHECK(pg::status_name(failure.error) == failure.name);

  auto sql = pg::sql_error("22012");
  CHECK(pg::status_name(std::error_code{-1, sql.category()}) == "unknown");
  CHECK(pg::status_name(std::error_code{60466176, sql.category()}) == "unknown");
  StatusForeignCategory foreign;
  CHECK(pg::status_name(std::error_code{42, foreign}) == "error");
  CHECK(foreign.calls == 0);
}

TEST_CASE("PostgreSQL status names support concurrent immutable snapshots without a Context")
{
  pg::Outcome outcome;
  outcome.result.emplace();
  outcome.result->kind = pg::ResultKind::tuples;
  pg::PipelineResult barrier;
  barrier.kind = pg::PipelineKind::sync;
  barrier.transaction = pg::Transaction::failed;
  pg::ExchangeEvent data{std::vector<std::byte>{std::byte{'x'}}};
  std::atomic<bool> failed = false;
  std::array<std::thread, 4> readers;
  for (auto &reader : readers) {
    reader = std::thread([&] {
      for (unsigned index = 0; index < 2000; ++index) {
        if (pg::status_name(outcome) != "tuples" || pg::status_name(barrier) != "pipeline_sync" ||
          pg::status_name(data) != "copy_data")
          failed.store(true);
      }
    });
  }
  for (auto &reader : readers)
    reader.join();
  CHECK_FALSE(failed.load());
}
