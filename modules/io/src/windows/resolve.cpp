#include "resolve.hpp"
#include "iocp.hpp"
#include <weave/resolve.hpp>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <type_traits>

namespace weave {

namespace {

Error native_error(int code)
{
  return {code, std::system_category()};
}

struct WinsockLease {
  bool started = false;

  ~WinsockLease()
  {
    if (started)
      WSACleanup();
  }
};

Result<std::wstring> wide_host(const std::string &host)
{
  const auto invalid_character = std::any_of(host.begin(), host.end(), [](unsigned char character) {
    return character <= 32 || character == 127 || character == '/' || character == '\\' || character == ':' ||
      character == '[' || character == ']' || character == '%';
  });
  const auto numeric_candidate = std::all_of(host.begin(), host.end(), [](unsigned char character) {
    return (character >= '0' && character <= '9') || character == '.';
  });
  if (host.empty() || host.size() > 65535 || invalid_character || numeric_candidate)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  const auto size = static_cast<int>(host.size());
  const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host.data(), size, nullptr, 0);
  if (!length)
    return std::unexpected(native_error(static_cast<int>(GetLastError())));
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host.data(), size, result.data(), length))
    return std::unexpected(native_error(static_cast<int>(GetLastError())));
  return result;
}

struct QueryRecord {
  WSAOVERLAPPED overlapped{};
  detail::Posted event{};
  Context *context = nullptr;
  PADDRINFOEXW results = nullptr;
  DWORD error = 0;
  std::atomic<bool> completed = false;
};

static_assert(std::is_standard_layout_v<QueryRecord>);
static_assert(offsetof(QueryRecord, overlapped) == 0);

void CALLBACK query_completed(DWORD error, DWORD, LPWSAOVERLAPPED overlapped)
{
  auto *query = reinterpret_cast<QueryRecord *>(overlapped);
  query->error = error;
  query->completed.store(true, std::memory_order_release);
  // Publication is the last access: resumption can destroy the query on another worker.
  detail::post(*query->context, query->event);
}

struct ResolverAwaiter {
  Context &context;
  const std::wstring &host;
  const std::wstring &service;
  detail::ResolverApi api;
  ADDRINFOEXW hints{};
  QueryRecord query{};
  HANDLE cancel_handle = nullptr;
  std::mutex cancel_mutex;
  CancelToken token;
  CancelToken stopping;

  struct Cancel {
    ResolverAwaiter *operation;

    void operator()() const noexcept
    {
      std::lock_guard lock(operation->cancel_mutex);
      if (!operation->query.completed.load(std::memory_order_acquire) && operation->cancel_handle) {
        // Completion may win this race; WSA_INVALID_HANDLE is documented for a finished query.
        const auto error = operation->api.cancel(&operation->cancel_handle);
        detail::require(error == 0 || error == WSA_INVALID_HANDLE);
      }
    }
  };

  std::optional<std::stop_callback<Cancel>> cancellation;
  std::optional<std::stop_callback<Cancel>> shutdown;

  ~ResolverAwaiter()
  {
    if (query.results)
      api.release(query.results);
  }

  bool await_ready() const noexcept
  {
    return false;
  }

  template <class P>
  bool await_suspend(std::coroutine_handle<P> continuation)
  {
    detail::IoAccess::check_execution(context);
    token = continuation.promise().cancellation;
    stopping = detail::context_cancellation(context);
    if (token.stop_requested() || stopping.stop_requested()) {
      query.error = WSA_E_CANCELLED;
      query.completed.store(true, std::memory_order_release);
      return false;
    }
    query.context = &context;
    query.event.executor = detail::current_executor;
    query.event.state = continuation.address();
    query.event.invoke = [](void *address) noexcept { std::coroutine_handle<>::from_address(address).resume(); };
    const auto error = api.query(
      host.c_str(),
      service.c_str(),
      NS_DNS,
      nullptr,
      &hints,
      &query.results,
      nullptr,
      &query.overlapped,
      query_completed,
      &cancel_handle);
    if (error != WSA_IO_PENDING) {
      query.error = static_cast<DWORD>(error);
      query.completed.store(true, std::memory_order_release);
      return false;
    }
    cancellation.emplace(token.native_token(), Cancel{this});
    shutdown.emplace(stopping.native_token(), Cancel{this});
    return true;
  }

  Result<void> await_resume() noexcept
  {
    cancellation.reset();
    shutdown.reset();
    detail::require(query.completed.load(std::memory_order_acquire));
    if (query.error == WSA_E_CANCELLED || query.error == WSAECANCELLED || query.error == WSA_OPERATION_ABORTED)
      return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    if (query.error)
      return std::unexpected(native_error(static_cast<int>(query.error)));
    return {};
  }
};

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
  auto name = wide_host(host);
  if (!name)
    co_await fail(name.error());

  WinsockLease lease;
  WSADATA data{};
  auto started = WSAStartup(MAKEWORD(2, 2), &data);
  if (started != 0)
    co_await fail(native_error(started));
  lease.started = true;

  auto service = std::to_wstring(port);
  ResolverAwaiter operation{context, *name, service, api};
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
  for (auto *result = operation.query.results; result; result = result->ai_next) {
    auto endpoint = detail::socket_endpoint(result->ai_addr, result->ai_addrlen);
    if (endpoint && std::find(endpoints.begin(), endpoints.end(), *endpoint) == endpoints.end())
      endpoints.push_back(*endpoint);
  }
  if (endpoints.empty())
    co_await fail(native_error(WSANO_DATA));
  co_return endpoints;
}

Task<std::vector<Endpoint>> resolve(Context &context, std::string host, u16 port, ResolveOptions options)
{
  return detail::resolve_with(context, std::move(host), port, options, {});
}

Task<std::vector<Endpoint>> resolve(std::string host, u16 port, ResolveOptions options)
{
  auto *context = detail::current_context;
  detail::require(context != nullptr);
  co_return co_await resolve(*context, std::move(host), port, options);
}

} // namespace weave
