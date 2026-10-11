#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <weave/database/detail/pool_admission.hpp>
#include <doctest/doctest.h>

namespace fixture {

struct Entry {
  weave::Semaphore &capacity;
  bool &destroyed_before_refund;

  ~Entry()
  {
    auto permit = capacity.try_acquire();
    destroyed_before_refund = !permit && permit.error() == std::errc::resource_unavailable_try_again;
  }
};

} // namespace fixture

TEST_CASE("pool failure cleanup retires entries before refunding admission")
{
  weave::Semaphore capacity{1};
  weave::db::detail::PoolInventory<fixture::Entry> inventory{1};
  bool destroyed_before_refund = false;
  const bool admitted = inventory.begin_acquire();
  REQUIRE(admitted);
  if (!admitted)
    return;
  {
    weave::db::detail::PoolAdmission<fixture::Entry> admission{inventory};
    auto permit = capacity.try_acquire();
    REQUIRE(permit);
    if (!permit)
      return;
    admission.permit.emplace(std::move(*permit));
    admission.entry = std::unique_ptr<fixture::Entry>{new fixture::Entry{capacity, destroyed_before_refund}};
    CHECK(inventory.stats().pending == 1);
  }
  CHECK(destroyed_before_refund);
  CHECK(inventory.stats().pending == 0);
  CHECK(inventory.stats().discarded == 1);
  CHECK(capacity.try_acquire());
}

TEST_CASE("pool close rejects publication without revoking existing leases")
{
  weave::Semaphore capacity{1};
  weave::db::detail::PoolInventory<fixture::Entry> inventory{1};
  bool destroyed_before_refund = false;
  auto entry = std::unique_ptr<fixture::Entry>{new fixture::Entry{capacity, destroyed_before_refund}};
  inventory.add_idle(std::move(entry));
  const bool admitted = inventory.begin_acquire();
  REQUIRE(admitted);
  if (!admitted)
    return;
  if (!inventory.take_idle(entry))
    return;
  REQUIRE(entry);
  if (!entry)
    return;
  const bool published = inventory.publish_borrow();
  REQUIRE(published);
  if (!published)
    return;
  inventory.finish_acquire(false);

  auto retired = inventory.retire_idle();
  CHECK(retired.empty());
  CHECK(!inventory.begin_acquire());
  CHECK(!inventory.publish_borrow());
  std::unique_ptr<fixture::Entry> unavailable;
  CHECK(!inventory.take_idle(unavailable));
  bool prepared = false;
  inventory.return_entry(entry, true, [&](fixture::Entry &) noexcept {
    prepared = true;
  });
  CHECK(!prepared);
  CHECK(entry != nullptr);
  auto stats = inventory.stats();
  CHECK(stats.closed);
  CHECK(stats.borrowed == 0);
  CHECK(stats.idle == 0);
  CHECK(stats.created == 1);
  CHECK(stats.discarded == 1);
}

TEST_CASE("shared pool limits match driver admission contracts")
{
  weave::db::PoolOptions limits;
  CHECK(weave::db::valid_pool_options(limits));
  limits.max_connections = 0;
  CHECK(!weave::db::valid_pool_options(limits));
  limits.max_connections = 65536;
  limits.min_connections = 0;
  limits.idle_timeout = std::chrono::milliseconds{0};
  limits.max_lifetime = std::chrono::milliseconds{0};
  CHECK(weave::db::valid_pool_options(limits));
  limits.acquire_timeout = std::chrono::milliseconds{0};
  CHECK(!weave::db::valid_pool_options(limits));
}
