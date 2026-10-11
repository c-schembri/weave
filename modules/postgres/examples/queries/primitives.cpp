#include <weave/postgres.hpp>
#include <weave/log.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace pg = weave::pg;

static weave::Task<void> example(pg::Options options)
{
  auto db = co_await pg::connect(std::move(options));
  const std::array formats{pg::Format::text, pg::Format::binary};

  for (auto format : formats) {
    auto result = co_await db.execute_borrowed(
      "SELECT 42::bigint AS id, 3::integer AS quantity, "
      "1.25::double precision AS weight, true AS delivered, "
      "'Ada'::text AS recipient, NULL::text AS notes",
      {},
      format);

    // Resolve once, then reuse the handle without column-name searches.
    auto weight_column = result.column("weight");
    if (!weight_column)
      co_await weave::fail(weight_column.error());

    for (const auto &row : result.rows) {
      auto id = row[0].as<weave::i64>();
      if (!id)
        co_await weave::fail(id.error());

      auto weight = row[*weight_column].floating();
      if (!weight)
        co_await weave::fail(weight.error());

      auto delivered = row["delivered"].boolean();
      if (!delivered)
        co_await weave::fail(delivered.error());

      auto recipient = row.get<std::string>("recipient");
      if (!recipient)
        co_await weave::fail(recipient.error());

      auto notes = row["notes"].optional<std::string_view>();
      if (!notes)
        co_await weave::fail(notes.error());

      // Each reader has its own position. Both sequential styles work together.
      auto fields = row.reader();
      auto skipped = fields.skip(1);
      if (!skipped)
        co_await weave::fail(skipped.error());

      auto quantity = fields.next<int>();
      if (!quantity)
        co_await weave::fail(quantity.error());

      auto sequential_weight = fields.next().as<double>();
      if (!sequential_weight)
        co_await weave::fail(sequential_weight.error());

      std::printf(
        "%s: shipment %lld, %d packages, %.2f kg, delivered=%s, recipient=%s, notes=%s\n",
        format == pg::Format::text ? "text" : "binary",
        static_cast<long long>(*id),
        *quantity,
        *weight,
        *delivered ? "true" : "false",
        recipient->c_str(),
        *notes ? "present" : "NULL");
    }
  }

  co_await db.finish();
}

int main(int argc, char **argv)
{
  if (argc > 2) {
    WEAVE_LOG_ERROR("Usage: postgres_primitives [connection-string]");
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
