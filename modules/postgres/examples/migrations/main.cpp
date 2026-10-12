#include <weave/postgres.hpp>
#include <weave/log.hpp>
#include "app_migrations.hpp"
#include <cstdlib>

static weave::Task<void> example(weave::pg::Options options)
{
  auto db = co_await weave::pg::connect(std::move(options));
  auto migrations = app_migrations();
  auto report = co_await weave::db::migrate(db, migrations);

  WEAVE_LOG_INFO(
    "Applied %zu migrations; version %llu",
    report.applied,
    static_cast<unsigned long long>(report.version));
}

int main(int argc, char **argv)
{
  if (argc > 2) {
    WEAVE_LOG_ERROR("Usage: postgres_migrations [connection-string]");
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
