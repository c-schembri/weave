#include <weave/postgres.hpp>
#include <weave/log.hpp>
#include <cstdlib>
#include <string>
#include <utility>

struct Shipment {
  weave::i64 id;
  std::string recipient;
};

static weave::Task<void> example(weave::pg::Options options)
{
  auto db = co_await weave::pg::connect(std::move(options));
  auto result = co_await db.execute("SELECT 42 AS id, 'Ada' AS recipient");

  for (const auto &row : result.rows) {
    auto shipment = row.as<Shipment>();
    if (!shipment)
      co_await weave::fail(shipment.error());

    WEAVE_LOG_INFO("Shipment %lld for %s", static_cast<long long>(shipment->id), shipment->recipient.c_str());
  }

  co_await db.finish();
}

int main(int argc, char **argv)
{
  if (argc > 2) {
    WEAVE_LOG_ERROR("Usage: postgres_simple_struct [connection-string]");
    return EXIT_FAILURE;
  }

  auto options = weave::pg::Options::load(argc == 2 ? argv[1] : "");
  if (!options)
    return weave::report_error(options.error());

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(example(std::move(*options)));
  if (!result)
    return weave::report_error(result.error());
}
