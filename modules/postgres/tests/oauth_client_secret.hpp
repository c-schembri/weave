#pragma once

#include "options.hpp"
#if defined(WEAVE_POSTGRES_TEST_LIBPQ)
#include <libpq-fe.h>
#endif

static constexpr std::string_view client_secret = "client-secret-owned-0123456789-client-secret-owned";

static void configuration_checks(const std::string &path)
{
  const std::array inputs{
    "oauth_client_secret='client-secret-owned-0123456789-client-secret-owned'",
    "postgresql://localhost/postgres?oauth_client_secret=client-secret-owned-0123456789-client-secret-owned",
    "oauth_client_secret='first' oauth_client_secret='client-secret-owned-0123456789-client-secret-owned'"};
  for (const auto *text : inputs) {
    fixture::CredentialPattern pattern{client_secret};
    {
      auto options = pg::Options::parse(text);
      check(
        bool(options) && options->oauth && options->oauth->client_secret,
        "Connection parser accepts owned client secret");
      check(options->oauth->client_secret->value() == client_secret, "Exact parsed client secret");
      auto copy = *options;
      check(
        copy.oauth->client_secret->value().data() == options->oauth->client_secret->value().data(),
        "Options copies share immutable secret allocation");
      fixture::CredentialAllocation witness{copy.oauth->client_secret->value()};
      options = std::unexpected(std::make_error_code(std::errc::operation_canceled));
      check(!witness.released, "Secret survives original Options destruction");
      copy.oauth.reset();
      check(witness.cleansed(), "Last Options owner cleanses secret");
    }
    check(pattern.dirty_releases() == 0, "No owned parser credential allocation discarded dirty");
#if defined(WEAVE_POSTGRES_TEST_LIBPQ)
    char *error = nullptr;
    auto *baseline = PQconninfoParse(text, &error);
    check(baseline != nullptr, "libpq parser control accepts the same secret configuration");
    bool matched = false;
    for (auto *field = baseline; field->keyword; ++field) {
      if (std::string_view{field->keyword} == "oauth_client_secret")
        matched = field->val && std::string_view{field->val} == client_secret;
    }
    PQconninfoFree(baseline);
    if (error)
      PQfreemem(error);
    check(matched, "libpq decoded value matches");
#endif
  }
  auto blank = pg::Options::parse("oauth_client_secret=''");
  check(
    bool(blank) && blank->oauth && !blank->oauth->client_secret,
    "Empty parsed secret selects no connection credential");
  blank = pg::Options::parse("oauth_client_secret=first oauth_client_secret=");
  check(bool(blank) && !blank->oauth->client_secret, "Explicit empty replaces an earlier secret");
  const std::array invalid{
    "oauth_client_secret='\xff'",
    "postgresql://localhost/?oauth_client_secret=%00",
    "postgresql://localhost/?oauth_client_secret=%ff"};
  for (const auto *text : invalid)
    check(!pg::Options::parse(text), "Invalid credential text rejected");

  {
    std::ofstream service(path, std::ios::binary);
    service << "[oauth]\nuser=weave\ndbname=postgres\noauth_issuer=https://issuer.example/tenant\n"
               "oauth_client_id=client\noauth_client_secret="
            << client_secret << "\n";
    // Force multiple service-file growth steps while the credential lives in its early allocation.
    for (unsigned index = 0; index < 12000; ++index)
      service << "# ignored comment\n";
    check(bool(service), "Owned service fixture written");
  }
  {
    fixture::CredentialPattern pattern{client_secret};
    {
      auto loaded = pg::Options::load(
        "service=oauth",
        {.environment = false, .user_files = false, .service_file = path});
      check(bool(loaded) && loaded->oauth->client_secret->value() == client_secret, "Service secret loaded explicitly");
      loaded = pg::Options::load(
        "service=oauth oauth_client_secret=override",
        {.environment = false, .user_files = false, .service_file = path});
      check(bool(loaded) && loaded->oauth->client_secret->value() == "override", "Connection secret overrides service");
      loaded = pg::Options::load(
        "service=oauth oauth_client_secret=''",
        {.environment = false, .user_files = false, .service_file = path});
      check(bool(loaded) && !loaded->oauth->client_secret, "Empty override removes service secret");
    }
    check(pattern.dirty_releases() == 0, "Service growth/decoded storage not discarded dirty");
  }
  std::error_code error;
  check(std::filesystem::remove(path, error) && !error, "Owned service fixture removed");

  const std::array malformed_suffix{"'", "%", "%00", "%GG"};
  for (auto suffix : malformed_suffix) {
    auto input = std::string("postgresql://localhost/?oauth_client_secret=") + std::string(client_secret) + suffix;
    if (std::string_view{suffix} == "'")
      input = std::string("oauth_client_secret='") + std::string(client_secret);
    fixture::CredentialPattern pattern{client_secret};
    check(!pg::Options::parse(input), "Malformed credential input fails");
    check(pattern.dirty_releases() == 0, "Partial malformed credential buffers cleansed");
  }
}

static weave::Task<void> request_secret_lifetime()
{
  auto provider = pg::OAuthProvider::create([](pg::OAuthRequest request) noexcept -> weave::Task<pg::OAuthToken> {
    check(
      request.client_secret && request.client_secret->value() == client_secret,
      "Custom provider receives secret owner");
    co_await weave::sleep_for(5ms);
    auto token = pg::OAuthToken::parse("abc");
    check(bool(token), "Test token");
    co_return std::move(*token);
  });
  check(bool(provider), "Custom provider factory");
  auto parsed = pg::Options::parse("oauth_client_secret=client-secret-owned-0123456789-client-secret-owned");
  check(bool(parsed), "Deferred request parsed options");
  fixture::CredentialAllocation witness{parsed->oauth->client_secret->value()};
  auto deferred = provider->request({.client_secret = parsed->oauth->client_secret});
  parsed = std::unexpected(std::make_error_code(std::errc::operation_canceled));
  check(!witness.released, "Lazy request retains secret before initial suspension");
  co_await std::move(deferred);
  check(witness.cleansed(), "Completed request releases last secret owner");
}

static void shared_owner_checks()
{
  fixture::CredentialPattern pattern{client_secret};
  std::optional<fixture::CredentialAllocation> witness;
  {
    auto parsed = pg::Options::parse("oauth_client_secret=client-secret-owned-0123456789-client-secret-owned");
    check(bool(parsed), "Unstarted connect options");
    witness.emplace(parsed->oauth->client_secret->value());
    {
      auto deferred = pg::connect(std::move(*parsed));
      check(!witness->released, "Lazy connect retains secret");
    }
    check(witness->cleansed(), "Unstarted connect drops last secret owner");
  }
  auto ctx = weave::Context::create();
  check(bool(ctx), "Secret lifetime Context");
  check(bool(ctx->run(request_secret_lifetime())), "Custom provider secret lifetime completes");
  check(pattern.dirty_releases() == 0, "No discarded dirty owned secret storage");

  const std::array outcomes{false, true};
  for (bool cancelled : outcomes) {
    auto counts = std::make_shared<Counts>();
    auto provider = pg::OAuthProvider::create(
      [counts, cancelled](pg::OAuthRequest request) noexcept -> weave::Task<pg::OAuthToken> {
        check(
          request.client_secret && request.client_secret->value() == client_secret,
          "Fallible child has immutable credential");
        ++counts->prompts;
        if (!cancelled)
          co_await weave::fail(std::errc::permission_denied);
        co_await weave::sleep_for(1h);
        auto token = pg::OAuthToken::parse("abc");
        co_return std::move(*token);
      });
    auto parsed = pg::Options::parse("oauth_client_secret=client-secret-owned-0123456789-client-secret-owned");
    fixture::CredentialAllocation child{parsed->oauth->client_secret->value()};
    auto pending = provider->request({.client_secret = parsed->oauth->client_secret});
    parsed = std::unexpected(std::make_error_code(std::errc::operation_canceled));
    if (cancelled) {
      auto job = ctx->spawn(std::move(pending));
      check(bool(job), "Secret child admitted");
      check(bool(ctx->run(weave::timeout(10s, wait_for_prompt(counts)))), "Secret child begins");
      job->cancel();
      auto result = ctx->run(join(std::move(*job)));
      check(!result && result.error() == std::errc::operation_canceled, "Secret child cancellation drains");
    } else {
      auto result = ctx->run(std::move(pending));
      check(!result && result.error() == std::errc::permission_denied, "Secret child failure propagates");
    }
    check(child.cleansed(), "Failed/cancelled provider drops last secret snapshot");
  }

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(bool(runtime), "Concurrent secret Runtime");
    auto parsed = pg::Options::parse("oauth_client_secret=client-secret-owned-0123456789-client-secret-owned");
    auto address = parsed->oauth->client_secret->value().data();
    fixture::CredentialAllocation retained{parsed->oauth->client_secret->value()};
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto provider = pg::OAuthProvider::create(
      [address, release](pg::OAuthRequest request) noexcept -> weave::Task<pg::OAuthToken> {
        check(
          request.client_secret && request.client_secret->value().data() == address,
          "Concurrent copies use one immutable credential allocation");
        while (!release->load())
          co_await weave::sleep_for(1ms);
        auto token = pg::OAuthToken::parse("abc");
        co_return std::move(*token);
      });
    std::vector<weave::JoinHandle<pg::OAuthToken>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto job = runtime->spawn(provider->request({.client_secret = parsed->oauth->client_secret}));
      check(bool(job), "Shared credential request admitted");
      jobs.push_back(std::move(*job));
    }
    parsed = std::unexpected(std::make_error_code(std::errc::operation_canceled));
    check(!retained.released, "Admitted children own shared secret before release");
    release->store(true);
    for (auto &job : jobs)
      check(bool(std::move(job).get()), "Concurrent secret request drains");
    runtime->join();
    check(retained.cleansed(), "Final concurrent snapshot owner cleanses once");
  }
#endif
}
