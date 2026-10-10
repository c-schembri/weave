#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <source_location>

namespace pg = weave::pg;
using namespace std::chrono_literals;

static_assert(std::constructible_from<pg::NotificationHandler, decltype([](const pg::Notification &) noexcept {
})>);
static_assert(!std::constructible_from<pg::NotificationHandler, decltype([](const pg::Notification &) {
})>);

static void check(bool value, std::source_location where = std::source_location::current())
{
  if (!value) {
    WEAVE_LOG_ERROR("Notification: %s:%u", where.file_name(), where.line());
    std::abort();
  }
}

struct Lifetime {
  unsigned &destroyed;

  ~Lifetime()
  {
    ++destroyed;
  }
};

static weave::Task<void> copy_send(pg::Connection &connection)
{
  std::vector<std::byte> data(512, std::byte{'y'});
  co_await weave::when_all(connection.write_copy(data), connection.write_copy(data));
  co_await connection.finish_copy_send();
}

static weave::Task<void> copy_read(pg::Connection &connection)
{
  std::size_t size = 0;
  while (auto data = co_await connection.read_copy()) {
    check(data->front() == std::byte{'x'});
    size += data->size();
  }
  check(size == 1024);
}

static weave::Task<void> observe(pg::Options options)
{
  unsigned count = 0;
  unsigned destroyed = 0;
  std::vector<pg::Notification> retained;
  auto owner = std::make_unique<Lifetime>(destroyed);
  auto connection = co_await pg::connect(options);
  auto *active = &connection;
  auto previous = connection.on_notification(
    [capture = std::move(owner), &count, &retained, &active](const pg::Notification &notification) noexcept {
      ++count;
      retained.push_back(notification);
      check(notification.process == 42 && notification.channel == "events");
      auto rejected = active->on_notification({});
      auto closed = active->close();
      auto cancelled = active->cancel();
      check(!rejected && rejected.error() == pg::Error::busy);
      check(!closed && closed.error() == pg::Error::busy);
      check(!cancelled && cancelled.error() == pg::Error::busy);
    });
  check(previous && !*previous && count == 0);

  auto queued = connection.take_notifications();
  check(queued.size() == 1 && queued.front().payload == "startup" && count == 0);
  {
    auto unstarted = connection.wait_notification();
    auto rejected = connection.on_notification({});
    check(!rejected && rejected.error() == pg::Error::busy && destroyed == 0);
  }
  co_await connection.query("BURST");
  check(count == 8 && connection.take_notifications().empty());

  auto pipeline = connection.pipeline();
  check(bool(pipeline) && !connection.on_notification({}));
  for (unsigned index = 0; index < 8; ++index)
    check(bool(pipeline->execute({"SELECT 1"})));
  check(bool(pipeline->sync()));
  co_await weave::when_all(pipeline->send(), pipeline->receive());
  unsigned results = 0;
  while (auto event = co_await pipeline->next()) {
    check(event->kind == pg::PipelineKind::sync || event->outcome.result.has_value());
    ++results;
  }
  check(results == 9 && pipeline->finish().has_value());
  check(count == 16 && connection.take_notifications().empty());

  auto format = co_await connection.start_copy("COPY BOTH");
  check(format.direction == pg::CopyDirection::both && count == 17);
  co_await weave::when_all(copy_send(connection), copy_read(connection));
  co_await connection.end_copy();
  check(count == 20 && connection.take_notifications().empty());

  auto error = co_await weave::as_result(connection.query("ERROR"));
  check(!error && pg::sqlstate(error.error()) == "22012" && count == 21 && connection.open());
  co_await connection.query("ARM");
  auto notification = co_await connection.wait_notification();
  check(notification.payload == "idle" && count == 22 && connection.take_notifications().empty());

  auto moved = std::move(connection);
  active = &moved;
  co_await moved.reset(options);
  check(count == 23 && destroyed == 0 && moved.take_notifications().empty());
  auto invalid = options;
  invalid.user = "bad";
  auto failed = co_await weave::as_result(moved.reset(std::move(invalid)));
  check(!failed && pg::sqlstate(failed.error()) == "28P01" && count == 24 && !moved.open());
  co_await moved.reset(options);
  check(count == 25 && destroyed == 0);

  auto saved = moved.on_notification({});
  check(saved && *saved && destroyed == 0);
  auto overflow = co_await weave::as_result(moved.query("BURST"));
  check(!overflow && overflow.error() == pg::Error::resource_limit && !moved.open() && count == 25);
  queued = moved.take_notifications();
  check(queued.size() == 2 && queued.front().payload == "notification 0");
  check(bool(moved.on_notification(std::move(*saved))));
  co_await moved.reset(options);
  check(count == 26);
  co_await moved.query("HOLD");
  auto cancelled = co_await weave::as_result(weave::timeout(20ms, moved.wait_notification()));
  check(!cancelled && cancelled.error() == std::errc::timed_out && count == 26 && !moved.open());
  check(retained.front().payload == "notification 0" && retained.back().payload == "startup");
  auto owner_back = moved.on_notification({});
  check(owner_back && *owner_back && destroyed == 0);
  *owner_back = {};
  check(destroyed == 1);
}

static weave::Task<void> invalid_request(pg::Connection &connection, const std::string &mode)
{
  if (mode.ends_with("_WAIT")) {
    co_await connection.query("ARM " + mode.substr(0, mode.size() - 5));
    static_cast<void>(co_await connection.wait_notification());
  } else {
    co_await connection.query(mode);
  }
}

static weave::Task<void> invalid_messages(pg::Options options, std::string mode)
{
  unsigned count = 0;
  auto connection = co_await pg::connect(options);
  auto initial = connection.take_notifications();
  check(initial.size() == 1);
  auto installed = connection.on_notification([&count](const pg::Notification &) noexcept {
    ++count;
  });
  check(bool(installed));
  auto result = co_await weave::as_result(invalid_request(connection, mode));
  auto expected = mode.starts_with("OVERSIZED") ? pg::Error::resource_limit : pg::Error::protocol;
  check(!result && result.error() == expected && count == 0 && !connection.open());
}

static weave::Task<void> backlog(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  unsigned old_count = 0;
  unsigned new_count = 0;
  check(bool(connection.on_notification([&old_count](const pg::Notification &) noexcept {
    ++old_count;
  })));
  auto old = co_await connection.wait_notification();
  check(old.payload == "startup" && old_count == 0);
  co_await connection.query("ARM");
  check(old_count == 0);
  auto saved = connection.on_notification([&new_count](const pg::Notification &) noexcept {
    ++new_count;
  });
  check(saved && *saved);
  auto value = co_await connection.wait_notification();
  check(value.payload == "idle" && old_count == 0 && new_count == 1 && connection.take_notifications().empty());
  co_await connection.finish();
}

static weave::Task<void> wait_once(pg::Connection &connection)
{
  static_cast<void>(co_await connection.wait_notification());
}

static weave::Task<void> observe_wait(pg::Connection &connection, weave::Result<pg::Notification> &outcome)
{
  outcome = co_await weave::as_result(connection.wait_notification());
}

static weave::Task<void> competing_waits(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  check(connection.take_notifications().size() == 1);
  unsigned count = 0;
  check(bool(connection.on_notification([&count](const pg::Notification &) noexcept {
    ++count;
  })));
  weave::Result<pg::Notification> interrupted = std::unexpected(pg::make_error_code(pg::Error::closed));
  auto competitor = wait_once(connection).on_error([&connection](weave::Error error) noexcept {
    check(error == pg::Error::busy);
    check(bool(connection.cancel()));
  });
  auto waits = co_await weave::as_result(weave::when_all(observe_wait(connection, interrupted), std::move(competitor)));
  check(!waits && waits.error() == pg::Error::busy && count == 0 && !connection.open());
  check(!interrupted && interrupted.error() == std::errc::operation_canceled);
  check(bool(connection.on_notification({})));
}

static weave::Task<void> dispatch(pg::Options options, const std::string &mode)
{
  if (mode.starts_with("MALFORMED") || mode.starts_with("OVERSIZED"))
    co_await invalid_messages(options, mode);
  else if (mode == "competing")
    co_await competing_waits(options);
  else if (mode == "backlog")
    co_await backlog(options);
  else if (mode == "observe")
    co_await observe(options);
  else
    check(false);
}

static void blocking(pg::Options options)
{
  auto connection = pg::BlockingConnection::connect(options);
  check(bool(connection) && connection->take_notifications().size() == 1);
  unsigned count = 0;
  auto installed = connection->on_notification([&count](const pg::Notification &notification) noexcept {
    check(notification.channel == "events" && notification.process == 42);
    ++count;
  });
  check(installed && !*installed);
  check(bool(connection->query("BURST")) && count == 8 && connection->take_notifications().empty());
  check(bool(connection->query("ARM")));
  auto event = connection->wait_notification();
  check(event && event->payload == "idle" && count == 9);
  check(bool(connection->reset(options)) && count == 10);
  auto old = connection->on_notification({});
  check(old && *old);
  check(bool(connection->query("ARM")));
  event = connection->wait_notification();
  check(event && event->payload == "idle" && count == 10);
  check(bool(connection->finish()));
}

int main(int argc, char **argv)
{
  check(argc == 3);
  auto port = weave::parse_port(argv[1]);
  check(bool(port));
  const std::string mode = argv[2];
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  options.limits.message_bytes = 1024;
  options.limits.result_bytes = 4096;
  options.limits.queued_notifications = 2;
  if (mode == "blocking") {
    blocking(options);
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  else if (mode == "affine" || mode == "stealing" || mode == "shared_affine" || mode == "shared_stealing") {
    auto scheduler = mode.ends_with("affine") ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing;
    auto layout = mode.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded;
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
    check(bool(runtime));
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 16; ++index) {
      auto job = runtime->spawn(weave::timeout(10s, observe(options)));
      check(bool(job));
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
  }
#endif
  else {
    auto ctx = weave::Context::create();
    check(bool(ctx));
    auto operation = dispatch(options, mode);
    auto result = ctx->run(weave::timeout(10s, std::move(operation)));
    if (!result)
      return weave::report_error(result.error());
  }
  WEAVE_LOG_INFO("Notification mode %s passed", mode.c_str());
}
