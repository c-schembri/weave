#include <weave/sync.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>

static weave::Task<void> produce(weave::Channel<int> &channel)
{
  for (int value = 1; value <= 8; ++value)
    co_await channel.send(value);

  channel.close();
}

static weave::Task<void> consume(weave::Channel<int> &channel, int &total)
{
  while (auto value = co_await channel.receive()) {
    WEAVE_LOG_INFO("Received: %d", *value);
    total += *value;
  }
}

int main()
{
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  weave::Channel<int> channel(2);
  int total = 0;

  auto result = ctx->run(weave::when_all(produce(channel), consume(channel, total)));
  if (!result)
    return weave::report_error(result.error());

  WEAVE_LOG_INFO("Total: %d", total);
  return total == 36 ? 0 : 1;
}
