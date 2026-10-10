#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <openssl/crypto.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace pg = weave::pg;
static std::atomic<unsigned> checks{0};

template <class T>
static void check(const T &condition, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "Memory streaming check failed: %u\n", location.line());
    std::exit(1);
  }
}

struct Observations {
  unsigned rows = 0;
  unsigned chunks = 0;
  std::size_t first_bytes = 0;
  std::optional<pg::detail::ResultArena> previous;
  std::optional<pg::detail::ResultText> escaped;

  void accept(pg::ResultSet &result)
  {
    check(result.columns.size() == 3);
    check(result.memory_size() > 0);
    if (result.kind == pg::ResultKind::tuples) {
      check(result.rows.empty() && result.command == "SELECT 1024");
      return;
    }
    check(result.kind == pg::ResultKind::row_chunk && !result.rows.empty() && result.rows.size() <= 7);
    auto arena = result.rows.get_allocator().arena();
    check(arena.identity() != nullptr);
    if (previous)
      check(previous->identity() != arena.identity());
    previous = arena;

    const auto bytes = result.memory_size();
    if (!first_bytes)
      first_bytes = bytes;
    check(bytes < first_bytes * 4);
    for (const auto &row : result.rows) {
      check(row[0].bytes() == std::to_string(++rows));
      check(row[1].bytes() == std::string(120, 'x'));
      check(row.get_allocator().arena().identity() == arena.identity());
      check(row[1].data->get_allocator().arena().identity() == arena.identity());
    }
    auto copy = result.copy();
    check(copy.rows.size() == result.rows.size() && copy.memory_size() > 0);
    for (std::size_t index = 0; index != result.rows.size(); ++index) {
      for (std::size_t field = 0; field != 3; ++field)
        check(copy.rows[index][field].data == result.rows[index][field].data);
    }
    check(copy.rows.get_allocator().arena().identity() != arena.identity());
    if (!escaped)
      escaped.emplace(std::move(*result.rows[0][1].data));
    ++chunks;
  }

  void done()
  {
    check(rows == 1024 && chunks > 100 && escaped && escaped->size() == 120);
    check(escaped->front() == 'x');
  }
};

static weave::Task<void> session(pg::Options options)
{
  auto connection = co_await pg::connect(std::move(options));
  auto events = std::make_shared<std::array<unsigned, 2>>();
  check(connection.on_event("memory", [events](pg::Event &event) noexcept -> weave::Result<void> {
    if (event.kind == pg::EventKind::result_create || event.kind == pg::EventKind::result_copy) {
      check(event.result && event.result->memory_size() > 0);
      ++(*events)[0];
    } else if (event.kind == pg::EventKind::result_destroy) {
      ++(*events)[1];
    }
    return {};
  }));
  {
    auto results = co_await connection.query("BUFFERED");
    check(results.size() == 1 && results[0].rows.size() == 1 && results[0].memory_size() > 0);
  }
  Observations observations;
  {
    auto exchange = co_await connection.exchange("STREAM", {.chunk_rows = 7});
    while (auto event = co_await exchange.next()) {
      check(std::holds_alternative<pg::ResultSet>(*event));
      observations.accept(std::get<pg::ResultSet>(*event));
    }
    check(exchange.finish());
  }
  co_await connection.finish();
  observations.done();
  check((*events)[0] == (*events)[1]);
}

static void blocking(pg::Options options)
{
  auto connection = pg::BlockingConnection::connect(std::move(options));
  check(connection);
  {
    auto results = connection->query("BUFFERED");
    check(results && results->size() == 1 && (*results)[0].rows.size() == 1 && (*results)[0].memory_size() > 0);
  }
  Observations observations;
  {
    auto exchange = connection->exchange("STREAM", {.chunk_rows = 7});
    check(exchange);
    for (;;) {
      auto event = exchange->next();
      check(event);
      if (!*event)
        break;
      check(std::holds_alternative<pg::ResultSet>(**event));
      observations.accept(std::get<pg::ResultSet>(**event));
    }
    check(exchange->finish());
  }
  check(connection->finish());
  observations.done();
}

int main(int argc, char **argv)
{
  check(argc == 3);
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  auto port = weave::parse_port(argv[1]);
  check(port);
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  options.limits.message_bytes = 2048;
  options.limits.result_bytes = 2048;
  std::string_view mode = argv[2];
  if (mode == "context") {
    auto ctx = weave::Context::create();
    check(ctx);
    auto result = ctx->run(session(options));
    if (!result)
      std::fprintf(stderr, "%s\n", result.error().message().c_str());
    check(result);
  } else if (mode == "blocking") {
    blocking(options);
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  else {
    auto scheduler = mode.ends_with("stealing") ? weave::Scheduler::work_stealing : weave::Scheduler::worker_affine;
    auto io = mode.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded;
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = io});
    check(runtime);
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index != 16; ++index) {
      auto job = runtime->spawn(session(options));
      check(job);
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs)
      check(std::move(job).get());
  }
#else
  else {
    check(false);
  }
#endif
  std::printf("Result memory streaming: %u checks\n", checks.load());
}
