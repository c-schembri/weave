#include "uring.hpp"
#include "resolve.hpp"
#include <weave/resolve.hpp>
#include <algorithm>
#include <netdb.h>
#include <signal.h>

namespace weave {

namespace {

class ResolverCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "weave.resolve";
  }

  std::string message(int code) const override
  {
    return gai_strerror(code);
  }
};

Error resolver_error(int code)
{
  static ResolverCategory category;
  return {code, category};
}

bool valid_host(const std::string &host)
{
  const bool invalid_character = std::any_of(host.begin(), host.end(), [](unsigned char character) {
    return character <= 32 || character == 127 || character == '/' || character == '\\' || character == ':' ||
      character == '[' || character == ']' || character == '%';
  });
  const bool numeric_candidate = std::all_of(host.begin(), host.end(), [](unsigned char character) {
    return (character >= '0' && character <= '9') || character == '.';
  });
  return !host.empty() && host.size() <= 65535 && !invalid_character && !numeric_candidate;
}

struct ResolverAwaiter {
  Context &context;
  detail::ResolverApi api;
  addrinfo hints{};
  detail::ResolverRequest query{};
  detail::Posted event{};
  std::mutex mutex;
  static constexpr unsigned notified = 1;
  static constexpr unsigned published = 2;
  std::atomic<unsigned> phase = 0;
  bool cancelled = false;
  int setup_error = 0;

  struct Cancel {
    ResolverAwaiter *operation;

    void operator()() const noexcept
    {
      std::lock_guard lock(operation->mutex);
      if (!(operation->phase.load(std::memory_order_acquire) & notified) && !operation->setup_error) {
        operation->cancelled = true;
        // Running glibc queries cannot always be cancelled. The notification must still drain.
        const auto result = operation->api.cancel(operation->api.state, operation->query);
        detail::require(result == EAI_CANCELED || result == EAI_NOTCANCELED || result == EAI_ALLDONE);
      }
    }
  };

  std::optional<std::stop_callback<Cancel>> cancellation;
  std::optional<std::stop_callback<Cancel>> shutdown;

  ResolverAwaiter(Context &context, detail::ResolverApi api) : context(context), api(api)
  {
  }

  ~ResolverAwaiter()
  {
    if (query.native.ar_result)
      api.release(api.state, query.native.ar_result);
  }

  bool await_ready() const noexcept
  {
    return false;
  }

  template <class P>
  bool await_suspend(std::coroutine_handle<P> continuation)
  {
    detail::IoAccess::check_execution(context);
    auto token = continuation.promise().cancellation;
    auto stopping = detail::context_cancellation(context);
    if (token.stop_requested() || stopping.stop_requested()) {
      cancelled = true;
      phase.store(notified, std::memory_order_relaxed);
      return false;
    }

    event.executor = detail::current_executor;
    event.state = continuation.address();
    event.invoke = [](void *address) noexcept {
      std::coroutine_handle<>::from_address(address).resume();
    };
    query.native.ar_request = &hints;
    query.state = this;
    query.notify = [](detail::ResolverRequest &query) noexcept {
      auto &operation = *static_cast<ResolverAwaiter *>(query.state);
      const auto previous = operation.phase.fetch_or(notified, std::memory_order_acq_rel);
      if (previous & published)
        detail::post(operation.context, operation.event);
    };

    setup_error = api.start(api.state, query);
    // glibc can enqueue the query but fail to allocate its notification record.
    // That allocation failure is fatal, like Weave's own frame allocation failures.
    detail::require(setup_error != EAI_AGAIN);
    cancellation.emplace(token.native_token(), Cancel{this});
    shutdown.emplace(stopping.native_token(), Cancel{this});
    const auto previous = phase.fetch_or(published, std::memory_order_acq_rel);
    return !(previous & notified);
  }

  Result<void> await_resume() noexcept
  {
    cancellation.reset();
    shutdown.reset();
    std::lock_guard lock(mutex);
    detail::require(phase.load(std::memory_order_acquire) & notified);
    if (cancelled)
      return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    const auto error = setup_error ? setup_error : api.status(api.state, query);
    if (error == EAI_CANCELED)
      return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    if (error)
      return std::unexpected(resolver_error(error));
    return {};
  }
};

int start_query(void *, detail::ResolverRequest &query) noexcept
{
  sigevent notification{};
  notification.sigev_notify = SIGEV_THREAD;
  notification.sigev_value.sival_ptr = &query;
  notification.sigev_notify_function = [](sigval value) {
    auto &query = *static_cast<detail::ResolverRequest *>(value.sival_ptr);
    query.notify(query);
  };
  gaicb *requests[] = {&query.native};
  return getaddrinfo_a(GAI_NOWAIT, requests, 1, &notification);
}

constexpr detail::ResolverApi native_api{
  nullptr,
  start_query,
  [](void *, detail::ResolverRequest &query) noexcept {
    return gai_cancel(&query.native);
  },
  [](void *, detail::ResolverRequest &query) noexcept {
    return gai_error(&query.native);
  },
  [](void *, addrinfo *result) noexcept {
    freeaddrinfo(result);
  }};

} // namespace

Task<std::vector<Endpoint>> detail::resolve_with(
  Context &context,
  std::string host,
  u16 port,
  ResolveOptions options,
  ResolverApi api)
{
  detail::IoAccess::check_execution(context);
  co_await cancellation_point();
  if (context.stop_requested())
    co_await fail(std::errc::operation_canceled);
  if (options.family != AddressFamily::any && options.family != AddressFamily::v4 &&
    options.family != AddressFamily::v6)
    co_await fail(std::errc::invalid_argument);

  auto numeric = IpAddress::parse(host);
  if (numeric) {
    if (options.family != AddressFamily::any && options.family != numeric->family())
      co_await fail(std::errc::address_family_not_supported);
    co_return std::vector<Endpoint>{{*numeric, port}};
  }
  if (!valid_host(host))
    co_await fail(std::errc::invalid_argument);

  auto service = std::to_string(port);
  ResolverAwaiter operation(context, api);
  operation.query.native.ar_name = host.c_str();
  operation.query.native.ar_service = service.c_str();
  operation.hints.ai_family = AF_UNSPEC;
  if (options.family == AddressFamily::v4)
    operation.hints.ai_family = AF_INET;
  else if (options.family == AddressFamily::v6)
    operation.hints.ai_family = AF_INET6;
  operation.hints.ai_socktype = SOCK_STREAM;
  operation.hints.ai_protocol = IPPROTO_TCP;
  auto completed = co_await operation;
  if (!completed)
    co_await fail(completed.error());

  std::vector<Endpoint> endpoints;
  for (auto *result = operation.query.native.ar_result; result; result = result->ai_next) {
    auto endpoint = detail::socket_endpoint(result->ai_addr, result->ai_addrlen);
    if (endpoint && std::find(endpoints.begin(), endpoints.end(), *endpoint) == endpoints.end())
      endpoints.push_back(*endpoint);
  }
  if (endpoints.empty())
    co_await fail(resolver_error(EAI_NONAME));
  co_return endpoints;
}

Task<std::vector<Endpoint>> resolve(Context &context, std::string host, u16 port, ResolveOptions options)
{
  return detail::resolve_with(context, std::move(host), port, options, native_api);
}

Task<std::vector<Endpoint>> resolve(std::string host, u16 port, ResolveOptions options)
{
  auto *context = detail::current_context;
  detail::require(context != nullptr);
  co_return co_await resolve(*context, std::move(host), port, options);
}

} // namespace weave
