#include <weave/postgres.hpp>
#include <weave/log.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace pg = weave::pg;

struct Shipment {
  weave::i64 id;
  double weight;
  bool delivered;
  std::string recipient;
  std::optional<std::string> notes;
};

static weave::Task<void> example(pg::Options options)
{
  auto db = co_await pg::connect(std::move(options));
  const std::array formats{pg::Format::text, pg::Format::binary};

  for (auto format : formats) {
    auto result = co_await db.execute_borrowed(
      "SELECT 'Ada'::text AS recipient, true AS delivered, "
      "42::bigint AS id, NULL::text AS notes, 1.25::double precision AS weight",
      {},
      format);

    // Bind once for repeated rows; row.as<Shipment>() also works directly.
    auto decoder = result.decoder<Shipment>();
    if (!decoder)
      co_await weave::fail(decoder.error());

    for (const auto &row : result.rows) {
      auto shipment = decoder->decode(row);
      if (!shipment)
        co_await weave::fail(shipment.error());

      std::printf(
        "%s: shipment %lld, %.2f kg, delivered=%s, recipient=%s, notes=%s\n",
        format == pg::Format::text ? "text" : "binary",
        static_cast<long long>(shipment->id),
        shipment->weight,
        shipment->delivered ? "true" : "false",
        shipment->recipient.c_str(),
        shipment->notes ? shipment->notes->c_str() : "NULL");
    }
  }

  co_await db.finish();
}

int main(int argc, char **argv)
{
  if (argc > 2) {
    WEAVE_LOG_ERROR("Usage: postgres_struct [connection-string]");
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
