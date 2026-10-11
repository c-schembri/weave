#include <weave/postgres.hpp>
#include <weave/database.hpp>
#include <weave/log.hpp>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

struct Shipment {
  weave::i64 id;
  std::string recipient;
};

// Only needed for aliases: shipment_id does not match the id member.
template <>
struct weave::db::RowTraits<Shipment> {
  static constexpr auto fields = std::tuple{
    weave::db::field("shipment_id", &Shipment::id),
    weave::db::field("recipient", &Shipment::recipient)};
};

static weave::Task<void> example(weave::pg::Options options)
{
  auto db = co_await weave::pg::connect(std::move(options));
  auto result = co_await db.execute_borrowed("SELECT 42::bigint AS shipment_id, 'Ada'::text AS recipient");

  for (const auto &row : result.rows) {
    auto shipment = weave::db::decode<Shipment>(row); // row.as<Shipment>() also uses the shared mapping.
    if (!shipment)
      co_await weave::fail(shipment.error());

    std::printf("Shipment %lld for %s\n", static_cast<long long>(shipment->id), shipment->recipient.c_str());
  }
  co_await db.finish();
}

int main(int argc, char **argv)
{
  if (argc > 2) {
    WEAVE_LOG_ERROR("Usage: postgres_shared_mapping [connection-string]");
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
