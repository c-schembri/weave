#include <doctest/doctest.h>
#include <array>
#include <cstdio>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

static std::FILE *log_test_stream = nullptr;
#define WEAVE_LOG_STREAM log_test_stream
#include <weave/log.hpp>

TEST_CASE("Logging macros format records and are safe in if/else statements")
{
  std::FILE *file = nullptr;
#ifdef _MSC_VER
  REQUIRE(tmpfile_s(&file) == 0);
#else
  file = std::tmpfile();
#endif
  std::unique_ptr<std::FILE, decltype(&std::fclose)> stream(file, &std::fclose);
  REQUIRE(stream != nullptr);
  log_test_stream = stream.get();

  int calls = 0;
  if (true)
    WEAVE_LOG_INFO("client=%d", ++calls);
  else
    CHECK(false);
  WEAVE_LOG_WARN("ready");
  WEAVE_LOG_ERROR("error=%s progress=100%%", "failed");
  WEAVE_LOG_DEBUG("debug=%d", ++calls);

  std::rewind(stream.get());
  std::array<char, 256> buffer{};
  const auto size = std::fread(buffer.data(), 1, buffer.size(), stream.get());
  log_test_stream = nullptr;
  const std::string_view output(buffer.data(), size);

#ifdef NDEBUG
  CHECK(calls == 1);
  CHECK(output == "[INFO] client=1\n[WARN] ready\n[ERROR] error=failed progress=100%\n");
#else
  CHECK(calls == 2);
  CHECK(output == "[INFO] client=1\n[WARN] ready\n[ERROR] error=failed progress=100%\n[DEBUG] debug=2\n");
#endif
}

TEST_CASE("Reporting an error prints its message and returns a failure exit code")
{
  std::FILE *file = nullptr;
#ifdef _MSC_VER
  REQUIRE(tmpfile_s(&file) == 0);
#else
  file = std::tmpfile();
#endif
  std::unique_ptr<std::FILE, decltype(&std::fclose)> stream(file, &std::fclose);
  REQUIRE(stream != nullptr);

  const auto error = std::make_error_code(std::errc::invalid_argument);
  CHECK(weave::report_error(error, stream.get()) == 1);

  std::rewind(stream.get());
  std::array<char, 1024> buffer{};
  const auto size = std::fread(buffer.data(), 1, buffer.size(), stream.get());
  CHECK(std::string_view(buffer.data(), size) == error.message() + "\n");
}
