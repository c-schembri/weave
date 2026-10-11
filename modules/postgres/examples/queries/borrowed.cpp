#include <weave/postgres.hpp>
#include <weave/log.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace pg = weave::pg;

static void print_result(const pg::ResultSet &result)
{
  for (const auto &row : result.rows) {
    for (std::size_t column = 0; column < row.size(); ++column) {
      if (column != 0)
        std::fputc('\t', stdout);

      const auto &value = row[column];
      auto text = value.is_null() ? std::string_view{"NULL"} : value.bytes();
      std::fwrite(text.data(), 1, text.size(), stdout);
    }
    std::fputc('\n', stdout);
  }
}

static void print_outcome(const pg::Outcome &outcome)
{
  if (outcome.result) {
    print_result(*outcome.result);
    return;
  }

  auto state = outcome.error.sqlstate();
  auto message = outcome.error.message();
  std::fwrite(state.data(), 1, state.size(), stdout);
  std::fputs(": ", stdout);
  std::fwrite(message.data(), 1, message.size(), stdout);
  std::fputc('\n', stdout);
}

static weave::Task<void> example(pg::Options options)
{
  auto db = co_await pg::connect(std::move(options));

  // These arrays and their referenced data survive every corresponding await.
  const std::array parameters{
    pg::ParameterView{.data = "42", .type = 23},
    pg::ParameterView{.data = "hello", .type = 25},
    pg::ParameterView{.data = std::nullopt, .type = 25}};
  constexpr std::string_view sql = "SELECT $1::integer, $2::text, $3::text";
  const std::array<weave::u32, 3> types{23, 25, 25};

  std::puts("Array-backed parameters:");
  auto result = co_await db.execute_borrowed(sql, parameters);
  print_result(result);

  std::puts("Prepared query:");
  co_await db.prepare_borrowed("lookup", sql, types);
  result = co_await db.execute_prepared_borrowed("lookup", parameters);
  print_result(result);

  std::puts("Prepared outcome:");
  auto prepared = co_await db.execute_prepared_outcome_borrowed("lookup", parameters);
  print_outcome(prepared);
  co_await db.close_prepared("lookup");

  // SQL errors become outcomes; transport and cancellation errors still propagate.
  std::puts("Recoverable SQL error:");
  auto outcome = co_await db.execute_outcome_borrowed("SELECT 1 / 0");
  print_outcome(outcome);

  const std::array binary_parameters{
    pg::ParameterView{.data = std::string_view{"\0\0\0\x2a", 4}, .type = 23, .format = pg::Format::binary}};

  result = co_await db.execute_borrowed("SELECT $1::integer", binary_parameters, pg::Format::binary);
  auto number = result.rows.front().get<weave::i32>(0);
  if (!number)
    co_await weave::fail(number.error());

  std::printf("Binary integer: %d\n", static_cast<int>(*number));

  // A small buffer demonstrates partial CopyData delivery without a payload vector.
  std::puts("Caller-buffer COPY:");
  co_await db.start_copy("COPY (SELECT repeat('x', 80) FROM generate_series(1, 2)) TO STDOUT");
  std::array<std::byte, 32> buffer;
  std::size_t messages = 0;

  while (auto received = co_await db.read_copy(buffer)) {
    auto bytes = std::span{buffer}.first(received->size);
    if (std::fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size())
      co_await weave::fail(std::make_error_code(std::errc::io_error));

    if (received->end_of_message)
      ++messages;
  }

  auto terminal = db.copy_result();
  if (!terminal)
    co_await weave::fail(terminal.error());

  std::printf("COPY messages: %zu\n", messages);
  co_await db.finish();
}

int main(int argc, char **argv)
{
  if (argc > 2) {
    WEAVE_LOG_ERROR("Usage: postgres_borrowed [connection-string]");
    return EXIT_FAILURE;
  }

  auto options = pg::Options::load(argc == 2 ? argv[1] : "");
  if (!options)
    return weave::report_error(options.error());

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(example(std::move(*options)));
  if (!result)
    return weave::report_error(result.error());
}
