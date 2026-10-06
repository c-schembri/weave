#include <doctest/doctest.h>
#include <weave/stream.hpp>
#include <vector>
#include <array>
#include <algorithm>

struct MemorySource {
  std::vector<std::byte> bytes;
  std::size_t position = 0;
  weave::Error error;

  weave::Task<std::size_t> read(std::span<std::byte> buffer)
  {
    if (error)
      co_await weave::fail(error);

    auto count = (std::min)({buffer.size(), bytes.size() - position, std::size_t{3}});
    std::copy_n(bytes.begin() + position, count, buffer.begin());
    position += count;

    co_return count;
  }
};

struct MemorySink {
  std::vector<std::byte> bytes;
  weave::Error error;

  weave::Task<void> write_all(std::span<const std::byte> buffer)
  {
    if (error)
      co_await weave::fail(error);

    bytes.insert(bytes.end(), buffer.begin(), buffer.end());

    co_return;
  }
};

template <class T>
static weave::Result<T> run_stream(weave::Task<T> task)
{
  weave::detail::TaskAccess::start(task);
  REQUIRE(weave::detail::TaskAccess::done(task));

  return weave::detail::TaskAccess::take(task);
}

TEST_CASE("Stream helpers compose independent implementations and preserve partial reads")
{
  static_assert(weave::ReadStream<MemorySource>);
  static_assert(weave::WriteStream<MemorySink>);
  static_assert(!weave::DuplexStream<MemorySink>);

  MemorySource source{std::vector<std::byte>(103, std::byte{42})};
  MemorySink sink;
  std::array<std::byte, 17> buffer;

  CHECK(run_stream(weave::stream::copy(source, sink, buffer)) == 103);
  CHECK(sink.bytes == source.bytes);

  source.position = 0;
  CHECK(run_stream(weave::stream::read_exactly(source, buffer)));
  CHECK(std::all_of(buffer.begin(), buffer.end(), [](auto byte) {
    return byte == std::byte{42};
  }));

  source.position = source.bytes.size();
  auto eof = run_stream(weave::stream::read_exactly(source, buffer));
  REQUIRE_FALSE(eof);
  CHECK(eof.error() == std::errc::connection_reset);

  CHECK(run_stream(weave::stream::read_exactly(source, {})));
}

TEST_CASE("Stream helpers preserve errors and reject an empty copy buffer")
{
  MemorySource source{std::vector<std::byte>(4)};
  MemorySink sink;
  std::array<std::byte, 4> buffer;

  auto invalid = run_stream(weave::stream::copy(source, sink, {}));
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error() == std::errc::invalid_argument);

  sink.error = std::make_error_code(std::errc::broken_pipe);
  auto failed = run_stream(weave::stream::copy(source, sink, buffer));
  REQUIRE_FALSE(failed);
  CHECK(failed.error() == sink.error);

  source.error = std::make_error_code(std::errc::operation_canceled);
  auto cancelled = run_stream(weave::stream::read_exactly(source, buffer));
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error() == source.error);
}
