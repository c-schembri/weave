#include "../gss_pool.hpp"
#include <pthread.h>
#include <vector>
#if defined(WEAVE_POSTGRES_GSSAPI)
extern "C" {
#include <com_err.h>
}
#include <krb5.h>
#endif

namespace weave::pg::detail {

namespace {

#if defined(WEAVE_POSTGRES_GSSAPI)
class KerberosCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "weave.postgres.kerberos";
  }

  std::string message(int value) const override
  {
    return error_message(value);
  }
};

const KerberosCategory errors;

struct Configuration {
  krb5_context value = nullptr;

  ~Configuration()
  {
    if (value)
      krb5_free_context(value);
  }
};
#endif

} // namespace

struct GssPool::Platform {
  std::vector<pthread_t> threads;

  static void *enter(void *state) noexcept
  {
    static_cast<GssPool *>(state)->worker({});
    return nullptr;
  }
};

GssPool::GssPool(GssContextOptions options) : platform_(std::make_unique<Platform>()), options_(std::move(options))
{
}

GssPool::~GssPool()
{
  stop();
  for (auto thread : platform_->threads)
    weave::detail::require(pthread_join(thread, nullptr) == 0);
  drained();
}

Result<void> GssPool::start()
{
#if defined(WEAVE_POSTGRES_GSSAPI)
  if (options_.credential_cache.empty()) {
    Configuration configuration;
    auto error = krb5_init_context(&configuration.value);
    if (error)
      return std::unexpected(std::error_code(static_cast<int>(error), errors));
    auto *cache = krb5_cc_default_name(configuration.value);
    if (!cache || !*cache)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    options_.credential_cache = cache;
    if (options_.credential_cache.size() > 65536)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }
  platform_->threads.reserve(options_.workers);
  for (std::size_t index = 0; index < options_.workers; ++index) {
    pthread_t thread{};
    const auto error = pthread_create(&thread, nullptr, Platform::enter, this);
    if (error)
      return std::unexpected(std::error_code(error, std::generic_category()));
    platform_->threads.push_back(thread);
  }
  return wait_started();
#else
  return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
}

} // namespace weave::pg::detail
