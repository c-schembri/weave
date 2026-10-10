#include "pipeline_live.hpp"
#include <weave/timer.hpp>
#include <weave/scope.hpp>
#include <array>

namespace pg = weave::pg;
using namespace std::chrono_literals;

static weave::Task<void> pipeline_check(bool valid)
{
  if (!valid)
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<std::vector<pg::PipelineResult>> pipeline_results(pg::Pipeline &pipeline)
{
  std::vector<pg::PipelineResult> results;
  while (auto result = co_await pipeline.next())
    results.push_back(std::move(*result));

  co_return results;
}

static weave::Task<void> pipeline_interleave(pg::Pipeline &pipeline)
{
  auto first = co_await pipeline.next();
  co_await pipeline_check(first && first->outcome.result && first->outcome.result->rows.front().front().bytes() == "1");
  co_await pipeline_check(pipeline.execute({"SELECT 3"}).has_value());
  co_await pipeline_check(pipeline.sync().has_value());

  auto second = co_await pipeline.next();
  co_await pipeline_check(
    second && second->outcome.result && second->outcome.result->rows.front().front().bytes() == "2");
}

weave::Task<void> fixture::pipeline(pg::Connection &connection)
{
  const std::array split_modes{false, true};
  for (auto split : split_modes) {
    auto pipeline = connection.pipeline();
    co_await pipeline_check(pipeline.has_value());
    co_await pipeline_check(pipeline
        ->prepare(
          "pipeline_rows",
          "SELECT g::int AS value, NULL::text AS empty FROM generate_series(1, $1::int) g",
          {23})
        .has_value());
    co_await pipeline_check(
      pipeline->execute_prepared("pipeline_rows", {{"9", 23}}, pg::Format::binary, {.chunk_rows = 2}).has_value());
    co_await pipeline_check(pipeline->execute({"SELECT 42"}).has_value());
    co_await pipeline_check(pipeline->execute({"SELECT 1 WHERE false"}, {.chunk_rows = 1}).has_value());
    co_await pipeline_check(pipeline->close_prepared("pipeline_rows").has_value());
    co_await pipeline_check(pipeline->sync().has_value());

    unsigned rows = 0;
    unsigned completed = 0;
    co_await weave::scope([&](weave::TaskScope &tasks) -> weave::Task<void> {
      auto operation = split ? weave::when_all(pipeline->send(), pipeline->receive()) : pipeline->flush();
      auto producer = tasks.spawn(std::move(operation));
      co_await pipeline_check(producer.has_value());
      while (auto event = co_await pipeline->next()) {
        if (!event->complete) {
          co_await pipeline_check(event->id == 2 && event->outcome.result.has_value());
          const auto &result = *event->outcome.result;
          co_await pipeline_check(result.columns.size() == 2 && !result.rows.empty() && result.rows.size() <= 2);
          co_await pipeline_check(result.columns.front().format == pg::Format::binary);
          for (const auto &row : result.rows) {
            co_await pipeline_check(row.size() == 2);
            co_await pipeline_check(row.front().binary_integer<int>() == static_cast<int>(++rows) && row[1].is_null());
          }
          continue;
        }

        co_await pipeline_check(event->id == ++completed);
        if (event->id == 2)
          co_await pipeline_check(event->outcome.result->rows.empty() && event->outcome.result->command == "SELECT 9");
        if (event->id == 3)
          co_await pipeline_check(event->outcome.result->rows.front().front().bytes() == "42");
        if (event->id == 4)
          co_await pipeline_check(event->outcome.result->rows.empty() && event->outcome.result->command == "SELECT 0");
        if (event->id == 6)
          co_await pipeline_check(event->transaction == pg::Transaction::idle);
      }
    });
    co_await pipeline_check(rows == 9 && completed == 6 && pipeline->finish());
  }

  {
    auto pipeline = connection.pipeline();
    co_await pipeline_check(pipeline.has_value());
    co_await pipeline_check(pipeline->execute({"SELECT 41"}).has_value());
    co_await pipeline_check(pipeline->sync().has_value());
    co_await pipeline->send();
    auto unread = pipeline->try_next();
    co_await pipeline_check(!unread && unread.error() == std::errc::resource_unavailable_try_again);
    co_await pipeline_check(pipeline->execute({"SELECT 42"}).has_value());
    co_await pipeline_check(pipeline->sync().has_value());
    co_await pipeline->send();
    co_await pipeline->receive();

    auto results = co_await pipeline_results(*pipeline);
    co_await pipeline_check(results.size() == 4);
    co_await pipeline_check(results[0].outcome.result->rows.front().front().integer<int>() == 41);
    co_await pipeline_check(results[2].outcome.result->rows.front().front().integer<int>() == 42);
    co_await pipeline_check(pipeline->finish().has_value());
  }

  {
    auto pipeline = connection.pipeline();
    if (!pipeline)
      co_await weave::fail(pipeline.error());

    co_await pipeline_check(pipeline->prepare("pipeline_answer", "SELECT $1::integer + 1", {23}).has_value());
    co_await pipeline_check(pipeline->describe("pipeline_answer").has_value());
    co_await pipeline_check(pipeline->execute_prepared("pipeline_answer", {{"41"}}, pg::Format::binary).has_value());
    co_await pipeline_check(pipeline->sync().has_value());
    co_await pipeline_check(pipeline->execute({"SELECT 1/0"}).has_value());
    co_await pipeline_check(pipeline->execute({"SELECT 99"}).has_value());
    co_await pipeline_check(pipeline->sync().has_value());
    co_await pipeline_check(pipeline->execute({"SELECT 43"}).has_value());
    co_await pipeline_check(pipeline->close_prepared("pipeline_answer").has_value());
    co_await pipeline_check(pipeline->sync().has_value());
    co_await pipeline->flush();

    auto results = co_await pipeline_results(*pipeline);
    co_await pipeline_check(results.size() == 10);
    for (std::size_t index = 0; index < results.size(); ++index)
      co_await pipeline_check(results[index].id == index + 1);

    co_await pipeline_check(results[1].outcome.result->parameter_types == std::vector<weave::u32>{23});
    co_await pipeline_check(results[2].outcome.result->rows.front().front().binary_integer<int>() == 42);
    co_await pipeline_check(results[4].outcome.error.sqlstate() == "22012");
    co_await pipeline_check(results[5].outcome.aborted && !results[5].outcome.result);
    co_await pipeline_check(results[7].outcome.result->rows.front().front().integer<int>() == 43);
    co_await pipeline_check(results[3].transaction == pg::Transaction::idle);
    co_await pipeline_check(results[6].transaction == pg::Transaction::idle);
    co_await pipeline_check(results[9].transaction == pg::Transaction::idle);
    co_await pipeline_check(pipeline->finish().has_value());
  }

  {
    auto pipeline = connection.pipeline();
    co_await pipeline_check(pipeline && pipeline->execute({"SELECT 1/0"}));
    co_await pipeline->flush();
    co_await pipeline_check(pipeline->aborted());
    auto error = co_await pipeline->next();
    co_await pipeline_check(error && error->outcome.error.sqlstate() == "22012");
    co_await pipeline_check(!pipeline->finish());

    co_await pipeline_check(pipeline->execute({"SELECT 1"}).has_value());
    co_await pipeline->flush();
    auto skipped = co_await pipeline->next();
    co_await pipeline_check(skipped && skipped->outcome.aborted);
    co_await pipeline_check(pipeline->sync().has_value());
    co_await pipeline_check(pipeline->execute({"SELECT 2"}).has_value());
    co_await pipeline_check(pipeline->sync().has_value());
    co_await pipeline->flush();

    auto recovered = co_await pipeline_results(*pipeline);
    co_await pipeline_check(recovered.size() == 3 && recovered[1].outcome.result);
    co_await pipeline_check(!pipeline->aborted() && pipeline->finish());
  }

  {
    auto pipeline = connection.pipeline();
    co_await pipeline_check(
      pipeline && pipeline->execute({"BEGIN"}) && pipeline->execute({"SELECT 1/0"}) && pipeline->sync());
    co_await pipeline_check(pipeline->execute({"SELECT 1"}) && pipeline->sync());
    co_await pipeline_check(pipeline->execute({"ROLLBACK"}) && pipeline->sync());
    co_await pipeline->flush();

    auto results = co_await pipeline_results(*pipeline);
    co_await pipeline_check(results.size() == 7);
    co_await pipeline_check(results[2].transaction == pg::Transaction::failed);
    co_await pipeline_check(results[3].outcome.error.sqlstate() == "25P02");
    co_await pipeline_check(results[4].transaction == pg::Transaction::failed);
    co_await pipeline_check(results[6].transaction == pg::Transaction::idle);
    co_await pipeline_check(pipeline->finish().has_value());
  }

  {
    auto pipeline = connection.pipeline();
    co_await pipeline_check(pipeline && pipeline->execute({"SELECT 1"}));
    co_await pipeline_check(pipeline->execute({"SELECT 2 FROM pg_sleep(0.002)"}).has_value());
    co_await weave::when_all(pipeline->flush(), pipeline_interleave(*pipeline));
    co_await pipeline->flush();

    auto results = co_await pipeline_results(*pipeline);
    co_await pipeline_check(results.size() == 2 && results[0].outcome.result->rows.front().front().integer<int>() == 3);
    co_await pipeline_check(pipeline->finish().has_value());
  }

  {
    auto pipeline = connection.pipeline();
    const std::string payload(65536, 'x');
    for (int index = 0; index < 64; ++index)
      co_await pipeline_check(pipeline->execute({"SELECT $1::text", {{payload}}}).has_value());
    co_await pipeline_check(pipeline->sync().has_value());
    co_await pipeline->flush();

    for (weave::u64 id = 1; id <= 64; ++id) {
      auto result = co_await pipeline->next();
      co_await pipeline_check(result && result->id == id && result->outcome.result);
      co_await pipeline_check(result->outcome.result->rows.front().front().bytes() == payload);
    }
    auto barrier = co_await pipeline->next();
    co_await pipeline_check(barrier && barrier->kind == pg::PipelineKind::sync);
    co_await pipeline_check(pipeline->finish().has_value());
  }

  co_await connection.query("BEGIN");
  co_await connection.open_portal("pipeline_portal", "SELECT 42");
  {
    auto pipeline = connection.pipeline();
    co_await pipeline_check(
      pipeline && pipeline->describe_portal("pipeline_portal") && pipeline->close_portal("pipeline_portal") &&
      pipeline->execute({""}) && pipeline->sync());
    co_await pipeline->flush();
    auto results = co_await pipeline_results(*pipeline);
    co_await pipeline_check(results.size() == 4 && results[0].outcome.result->columns.size() == 1);
    co_await pipeline_check(results[2].outcome.result->rows.empty() && results[2].outcome.result->command.empty());
    co_await pipeline_check(results[3].transaction == pg::Transaction::active && pipeline->finish());
  }
  co_await connection.query("ROLLBACK");
}

weave::Task<void> fixture::pipeline_terminal(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  {
    auto pipeline = connection.pipeline();
    co_await pipeline_check(pipeline && pipeline->execute({"SELECT pg_sleep(30)"}) && pipeline->sync());
    auto cancelled = co_await weave::as_result(weave::timeout(50ms, pipeline->flush()));
    co_await pipeline_check(!cancelled && cancelled.error() == std::errc::timed_out && !connection.open());
  }
  co_await connection.reset(options);

  {
    auto pipeline = connection.pipeline();
    co_await pipeline_check(pipeline && pipeline->execute({"SELECT 1"}));
    co_await pipeline->flush();
    co_await pipeline_results(*pipeline);
  }
  co_await pipeline_check(!connection.open());
  co_await connection.reset(options);

  {
    auto pipeline = connection.pipeline();
    co_await pipeline_check(pipeline && pipeline->execute({"COPY (SELECT 1) TO STDOUT"}));
    auto copy = co_await weave::as_result(pipeline->flush());
    co_await pipeline_check(!copy && copy.error() == pg::Error::unexpected_copy && !connection.open());
  }

  options.limits.message_bytes = 1024;
  options.limits.result_bytes = 1024;
  co_await connection.reset(options);
  {
    auto pipeline = connection.pipeline();
    co_await pipeline_check(pipeline && pipeline->execute({"SELECT repeat('x',512) FROM generate_series(1,10)"}));
    auto excessive = co_await weave::as_result(pipeline->flush());
    co_await pipeline_check(!excessive && excessive.error() == pg::Error::resource_limit && !connection.open());
  }

  co_await connection.reset(options);
  {
    auto pipeline = connection.pipeline();
    co_await pipeline_check(pipeline.has_value());
    co_await pipeline_check(
      pipeline->execute({"SELECT repeat('x',100) AS value FROM generate_series(1,501)"}, {.chunk_rows = 128})
        .has_value());
    co_await pipeline_check(pipeline->sync().has_value());

    std::size_t rows = 0;
    unsigned completed = 0;
    co_await weave::scope([&](weave::TaskScope &tasks) -> weave::Task<void> {
      auto producer = tasks.spawn(pipeline->flush());
      co_await pipeline_check(producer.has_value());
      while (auto event = co_await pipeline->next()) {
        if (!event->complete) {
          co_await pipeline_check(event->outcome.result && !event->outcome.result->rows.empty());
          rows += event->outcome.result->rows.size();
        } else {
          ++completed;
        }
      }
    });
    co_await pipeline_check(rows == 501 && completed == 2 && pipeline->finish());
  }
  co_await connection.query("SELECT 42");
}

weave::Result<void> fixture::pipeline(pg::BlockingConnection &connection)
{
  auto pipeline = connection.pipeline();
  if (!pipeline)
    return std::unexpected(pipeline.error());

  auto command = pipeline->execute({"SELECT 42"});
  auto barrier = pipeline->sync();
  if (!command || !barrier)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  auto unavailable = pipeline->next();
  if (unavailable || unavailable.error() != std::errc::resource_unavailable_try_again)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  if (auto flushed = pipeline->flush(); !flushed)
    return std::unexpected(flushed.error());

  auto answer = pipeline->next();
  auto synced = pipeline->next();
  if (!answer || !*answer || (**answer).id != *command || !(**answer).outcome.result ||
    (**answer).outcome.result->rows.front().front().integer<int>() != 42 || !synced || !*synced ||
    (**synced).id != *barrier || (**synced).transaction != pg::Transaction::idle)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  if (auto finished = pipeline->finish(); !finished)
    return std::unexpected(finished.error());
  if (pipeline->flush())
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  const std::array split_modes{false, true};
  for (auto split : split_modes) {
    auto stream = connection.pipeline();
    if (!stream)
      return std::unexpected(stream.error());
    if (!stream->execute({"SELECT g::int AS value FROM generate_series(1,71) g"}, {.chunk_rows = 8}) || !stream->sync())
      return std::unexpected(std::make_error_code(std::errc::bad_message));
    auto rejected = stream->flush();
    if (rejected || rejected.error() != std::errc::operation_not_supported)
      return std::unexpected(std::make_error_code(std::errc::bad_message));
    if (split) {
      if (auto written = stream->send(); !written)
        return written;
      auto buffered = stream->receive();
      if (buffered || buffered.error() != std::errc::operation_not_supported)
        return std::unexpected(std::make_error_code(std::errc::bad_message));
    }
    auto started = split ? stream->start_receive() : stream->start();
    if (!started)
      return started;
    if (stream->start() || stream->execute({"SELECT 2"}) || stream->finish())
      return std::unexpected(std::make_error_code(std::errc::bad_message));

    auto moved = std::move(*stream);
    unsigned rows = 0;
    unsigned completed = 0;
    for (;;) {
      auto event = moved.next();
      if (!event)
        return std::unexpected(event.error());
      if (!*event)
        break;
      const auto &result = **event;
      if (result.complete) {
        ++completed;
      } else {
        if (!result.outcome.result || result.outcome.result->columns.front().name != "value")
          return std::unexpected(std::make_error_code(std::errc::bad_message));
        for (const auto &row : result.outcome.result->rows) {
          if (row.front().integer<int>() != static_cast<int>(++rows))
            return std::unexpected(std::make_error_code(std::errc::bad_message));
        }
      }
    }
    if (rows != 71 || completed != 2)
      return std::unexpected(std::make_error_code(std::errc::bad_message));
    if (auto finished = moved.finish(); !finished)
      return finished;
  }

  {
    auto split = connection.pipeline();
    if (!split || !split->execute({"SELECT 42"}) || !split->sync())
      return std::unexpected(std::make_error_code(std::errc::bad_message));
    if (split->receive() || split->start_receive())
      return std::unexpected(std::make_error_code(std::errc::bad_message));
    if (auto written = split->send(); !written)
      return written;
    if (split->send() || split->flush() || split->finish() || split->execute({"SELECT 1"}))
      return std::unexpected(std::make_error_code(std::errc::bad_message));
    if (auto read = split->receive(); !read)
      return read;

    auto answer = split->next();
    auto barrier = split->next();
    auto ended = split->next();
    if (!answer || !*answer || !(**answer).outcome.result ||
      (**answer).outcome.result->rows.front().front().integer<int>() != 42 || !barrier || !*barrier ||
      (**barrier).kind != pg::PipelineKind::sync || !ended || *ended)
      return std::unexpected(std::make_error_code(std::errc::bad_message));
    if (auto finished = split->finish(); !finished)
      return finished;
  }

  {
    auto unstarted = connection.pipeline();
    if (!unstarted || !unstarted->execute({"SELECT 1"}, {.chunk_rows = 1}))
      return std::unexpected(std::make_error_code(std::errc::bad_message));
    if (auto started = unstarted->start(); !started)
      return started;
  }
  if (auto reused = connection.query("SELECT 42"); !reused)
    return std::unexpected(reused.error());

  return {};
}
