#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/local.hpp>
#include <weave/timer.hpp>
#if defined(WEAVE_LOCAL_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace std::chrono_literals;

static std::filesystem::path native_path(const std::string &address)
{
  return std::filesystem::path(std::u8string(address.begin(), address.end()));
}

static weave::Result<bool> listed(const std::filesystem::path &path)
{
  std::error_code error;
  std::filesystem::directory_iterator iterator(path.parent_path(), error), end;
  while (!error && iterator != end) {
    if (iterator->path().filename() == path.filename())
      return true;
    iterator.increment(error);
  }
  if (error)
    return std::unexpected(error);
  return false;
}

class LocalFixture {
public:
  LocalFixture()
  {
    std::error_code error;
    auto temporary = std::filesystem::temp_directory_path(error);
    REQUIRE_FALSE(error);
    static std::atomic<unsigned> sequence = 0;
    auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto name = "wv-" + std::to_string(tick) + "-" + std::to_string(sequence.fetch_add(1));
    directory_ = temporary / name;
    REQUIRE(std::filesystem::create_directory(directory_, error));
    REQUIRE_FALSE(error);
  }

  ~LocalFixture()
  {
    // Only names allocated in this fixture's private directory are removed.
    std::error_code error;
    for (const auto &path : paths_) {
      std::filesystem::remove(path, error);
      CHECK_FALSE(error);
    }
    std::filesystem::remove(directory_, error);
    CHECK_FALSE(error);
  }

  std::string address(bool abstract = false)
  {
    auto name = "socket-" + std::to_string(paths_.size());
    auto path = directory_ / name;
    paths_.push_back(path);
    if (abstract)
      return "@" + directory_.filename().string() + "-" + name;
    auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
  }

private:
  std::filesystem::path directory_;
  std::vector<std::filesystem::path> paths_;
};

static weave::Result<void> metadata(weave::LocalStream &stream, const std::string &address, bool server)
{
  auto local = stream.local_address();
  auto peer = stream.peer_address();
  if (!local || !peer)
    return std::unexpected(local ? peer.error() : local.error());
  auto named = server ? *local : *peer;
  auto unnamed = server ? *peer : *local;
  if (named != address || !unnamed.empty())
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  auto credentials = stream.peer_credentials();
#if defined(_WIN32)
  if (credentials || credentials.error() != std::errc::operation_not_supported)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
#else
  if (!credentials)
    return std::unexpected(credentials.error());
  auto this_process = static_cast<weave::u64>(getpid());
  if (credentials->process != this_process || credentials->user != getuid() || credentials->group != getgid())
    return std::unexpected(std::make_error_code(std::errc::bad_message));
#endif
  return {};
}

static weave::Task<void> echo(weave::LocalListener &listener, std::size_t size)
{
  auto stream = co_await listener.accept();
  if (auto status = metadata(stream, listener.local_address(), true); !status)
    co_await weave::fail(status.error());

  std::vector<std::byte> buffer(size);
  co_await stream.read_exactly(buffer);
  co_await stream.write_all(buffer);
  std::array<std::byte, 1> tail;
  if (co_await stream.read(tail))
    co_await weave::fail(std::errc::bad_message);
  if (auto status = stream.shutdown_send(); !status)
    co_await weave::fail(status.error());
}

static weave::Task<void> exchange(std::string address, std::size_t size)
{
  auto connecting = weave::local::connect(address);
  auto expected = address;
  address.clear();
  auto stream = co_await std::move(connecting);
  if (auto status = metadata(stream, expected, false); !status)
    co_await weave::fail(status.error());

  std::vector<std::byte> sent(size), received(size);
  for (std::size_t index = 0; index < size; ++index)
    sent[index] = std::byte(index % 251);
  co_await stream.write_all(sent);
  if (auto status = stream.shutdown_send(); !status)
    co_await weave::fail(status.error());
  co_await stream.read_exactly(received);
  if (sent != received)
    co_await weave::fail(std::errc::bad_message);
  std::array<std::byte, 1> tail;
  if (co_await stream.read(tail))
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> roundtrip(std::string address, std::size_t size)
{
  auto listening = weave::local::listen(address);
  auto expected = address;
  address.clear();
  auto listener = co_await std::move(listening);
  if (listener.local_address() != expected)
    co_await weave::fail(std::errc::bad_message);
  co_await weave::when_all(echo(listener, size), exchange(expected, size));
}

static weave::Task<void> accept_into(weave::LocalListener &listener, std::optional<weave::LocalStream> &stream)
{
  stream.emplace(co_await listener.accept());
}

static weave::Task<void> connect_into(std::string address, std::optional<weave::LocalStream> &stream)
{
  stream.emplace(co_await weave::local::connect(std::move(address)));
}

static weave::Task<void> cancellation(weave::Context &ctx, std::string address)
{
  auto listener = co_await weave::local::listen(address);
  auto timed = co_await weave::as_result(weave::timeout(5ms, listener.accept()));
  if (timed || timed.error() != std::errc::timed_out)
    co_await weave::fail(std::errc::bad_message);

  std::optional<weave::LocalStream> accepted, connected;
  co_await weave::when_all(accept_into(listener, accepted), connect_into(address, connected));
  std::array<std::byte, 1> buffer{}, other{};
  auto pending = ctx.spawn(accepted->read(buffer));
  if (!pending)
    co_await weave::fail(pending.error());
  co_await ctx.yield();
  auto overlap = co_await weave::as_result(accepted->read(other));
  auto closing = accepted->close();
  pending->cancel();
  auto cancelled = co_await weave::as_result(std::move(*pending));

  // Drain the borrowed operation before checking failures or closing the stream.
  if (overlap || overlap.error() != std::errc::operation_in_progress || closing ||
    closing.error() != std::errc::operation_in_progress || cancelled ||
    cancelled.error() != std::errc::operation_canceled)
    co_await weave::fail(std::errc::bad_message);

  co_await connected->write_all(other);
  if (auto status = connected->shutdown_send(); !status)
    co_await weave::fail(status.error());
  std::array<std::byte, 2> incomplete{};
  auto short_read = co_await weave::as_result(accepted->read_exactly(incomplete));
  if (short_read || short_read.error() != std::errc::connection_reset)
    co_await weave::fail(std::errc::bad_message);

  if (auto status = accepted->close(); !status)
    co_await weave::fail(status.error());
  auto closed_read = co_await weave::as_result(accepted->read(buffer));
  if (!accepted->close() || closed_read || accepted->cancel() || accepted->shutdown_send() ||
    accepted->local_address() || accepted->peer_address() || accepted->peer_credentials())
    co_await weave::fail(std::errc::bad_message);
  if (auto status = listener.close(); !status)
    co_await weave::fail(status.error());
  auto closed_accept = co_await weave::as_result(listener.accept());
  if (!listener.close() || listener.cancel() || closed_accept || listener.local_address() != address)
    co_await weave::fail(std::errc::bad_message);
}

TEST_CASE("Local socket setup and metadata are synchronous where no I/O is pending")
{
  using Stream = weave::LocalStream;
  using Listener = weave::LocalListener;
  static_assert(std::is_same_v<decltype(std::declval<Stream &>().shutdown_send()), weave::Result<void>>);
  static_assert(std::is_same_v<decltype(std::declval<Stream &>().cancel()), weave::Result<void>>);
  static_assert(std::is_same_v<decltype(std::declval<Stream &>().close()), weave::Result<void>>);
  static_assert(std::is_same_v<decltype(std::declval<Stream &>().local_address()), weave::Result<std::string>>);
  static_assert(std::is_same_v<decltype(std::declval<Stream &>().peer_address()), weave::Result<std::string>>);
  static_assert(std::is_same_v<decltype(std::declval<Stream &>().peer_credentials()), weave::Result<weave::LocalPeer>>);
  static_assert(std::is_same_v<decltype(std::declval<Listener &>().cancel()), weave::Result<void>>);
  static_assert(std::is_same_v<decltype(std::declval<Listener &>().close()), weave::Result<void>>);
  static_assert(std::is_same_v<
    decltype(weave::local::listen(std::declval<weave::Context &>(), std::string{})),
    weave::Result<Listener>>);

  LocalFixture fixture;
  auto address = fixture.address();
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto listener = weave::local::listen(*ctx, address);
  REQUIRE(listener);
  auto moved = std::move(*listener);
  CHECK(moved.local_address() == address);
  REQUIRE(moved.close());
  CHECK(moved.local_address() == address);
  // Windows socket files are reparse points; stat-like exists() cannot follow them.
  auto retained = listed(native_path(address));
  REQUIRE(retained);
  CHECK(*retained);
}

TEST_CASE("Local addresses are validated before creating native sockets")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  struct Invalid {
    std::string address;
    std::errc error;
  };

  const std::array invalid{
    Invalid{"", std::errc::invalid_argument},
    Invalid{"@", std::errc::invalid_argument},
    Invalid{std::string("a\0b", 3), std::errc::invalid_argument},
    Invalid{std::string(108, 'x'), std::errc::filename_too_long},
    Invalid{"@" + std::string(108, 'x'), std::errc::filename_too_long}};
  for (const auto &input : invalid) {
    auto listener = weave::local::listen(*ctx, input.address);
    auto stream = ctx->run(weave::local::connect(input.address));
    REQUIRE_FALSE(listener);
    CHECK(listener.error() == input.error);
    REQUIRE_FALSE(stream);
    CHECK(stream.error() == input.error);
  }
  LocalFixture fixture;
  auto address = fixture.address();
  const std::array backlogs{0, -1};
  for (auto backlog : backlogs) {
    auto listener = weave::local::listen(*ctx, address, backlog);
    REQUIRE_FALSE(listener);
    CHECK(listener.error() == std::errc::invalid_argument);
  }
  CHECK(ctx->metrics().submitted == 0);
#if defined(_WIN32)
  auto listener = weave::local::listen(*ctx, "@not-supported");
  auto stream = ctx->run(weave::local::connect("@not-supported"));
  REQUIRE_FALSE(listener);
  REQUIRE_FALSE(stream);
  CHECK(listener.error() == std::errc::operation_not_supported);
  CHECK(stream.error() == std::errc::operation_not_supported);
#endif
}

TEST_CASE("Local bind failures never delete or replace caller-owned files")
{
  LocalFixture fixture;
  auto address = fixture.address();
  auto path = native_path(address);
  {
    std::ofstream output(path);
    REQUIRE(output);
    output << "keep me";
  }
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  CHECK_FALSE(weave::local::listen(*ctx, address));
  std::ifstream input(path);
  std::string contents;
  std::getline(input, contents);
  CHECK(contents == "keep me");

  auto missing = fixture.address();
  auto stream = ctx->run(weave::timeout(5s, weave::local::connect(missing)));
  REQUIRE_FALSE(stream);
  CHECK(stream.error() != std::errc::timed_out);
  auto created = listed(native_path(missing));
  REQUIRE(created);
  CHECK_FALSE(*created);
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
}

TEST_CASE("Local streams preserve payload, EOF, names and authenticated Linux peer IDs")
{
  const std::array skip_modes{false, true};
  const std::array sizes{std::size_t{1}, std::size_t{131072}};
#if defined(_WIN32)
  const std::array address_modes{false};
#else
  const std::array address_modes{false, true};
#endif
  for (auto skip : skip_modes) {
    for (auto abstract : address_modes) {
      for (auto size : sizes) {
        CAPTURE(skip);
        CAPTURE(abstract);
        CAPTURE(size);
        LocalFixture fixture;
        auto ctx = weave::Context::create({.skip_successful_completions = skip});
        REQUIRE(ctx);
        auto result = ctx->run(weave::timeout(5s, roundtrip(fixture.address(abstract), size)));
        CAPTURE(result ? 0 : result.error().value());
        REQUIRE(result);
        CHECK(ctx->metrics().submitted == ctx->metrics().completed);
      }
    }
  }
}

TEST_CASE("Local cancellation drains pending operations and rejects active close or overlapping reads")
{
  const std::array skip_modes{false, true};
  for (auto skip : skip_modes) {
    CAPTURE(skip);
    LocalFixture fixture;
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    auto result = ctx->run(weave::timeout(5s, cancellation(*ctx, fixture.address())));
    CAPTURE(result ? 0 : result.error().value());
    REQUIRE(result);
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
  }
}

#if defined(WEAVE_LOCAL_TEST_RUNTIME)
TEST_CASE("Local sockets retain their context under both multicore scheduling policies")
{
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
#if defined(_WIN32)
  const std::array layouts{weave::IoLayout::sharded, weave::IoLayout::shared};
#else
  const std::array layouts{weave::IoLayout::sharded};
#endif
  const std::array skip_modes{false, true};
  for (auto scheduler : schedulers) {
    for (auto layout : layouts) {
      for (auto skip : skip_modes) {
        CAPTURE(scheduler);
        CAPTURE(layout);
        CAPTURE(skip);
        LocalFixture fixture;
        auto runtime = weave::Runtime::create(
          {.workers = 4,
            .scheduler = scheduler,
            .context = {.skip_successful_completions = skip},
            .io_layout = layout});
        REQUIRE(runtime);
        std::vector<weave::JoinHandle<void>> jobs;
        for (int index = 0; index < 16; ++index) {
          auto job = runtime->spawn(weave::timeout(5s, roundtrip(fixture.address(), 131072)));
          REQUIRE(job);
          jobs.push_back(std::move(*job));
        }
        bool passed = true;
        for (auto &job : jobs) {
          auto result = std::move(job).get();
          CHECK(result);
          passed = passed && result.has_value();
        }
        REQUIRE(passed);
      }
    }
  }
}
#endif
