#include "../ldap.hpp"
#include "../options.hpp"
#include <openssl/crypto.h>
#ifdef WEAVE_POSTGRES_LDAP
#include <windows.h>
#include <winldap.h>
#endif
#include <memory>

namespace weave::pg::detail {

#ifdef WEAVE_POSTGRES_LDAP
namespace {

class LdapCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "weave.postgres.ldap";
  }

  std::string message(int value) const override
  {
    return ldap_err2stringA(static_cast<ULONG>(value));
  }
};

std::error_code ldap_error(ULONG value)
{
  static LdapCategory category;
  return {static_cast<int>(value), category};
}

Result<std::wstring> wide(std::string_view value)
{
  int length = MultiByteToWideChar(
    CP_UTF8,
    MB_ERR_INVALID_CHARS,
    value.data(),
    static_cast<int>(value.size()),
    nullptr,
    0);
  if (length <= 0)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  std::wstring output(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        output.data(),
        length) != length)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  return output;
}

struct SessionCloser {
  void operator()(LDAP *value) const noexcept
  {
    ldap_unbind(value);
  }
};

struct MessageCloser {
  void operator()(LDAPMessage *value) const noexcept
  {
    ldap_msgfree(value);
  }
};

struct ValuesCloser {
  void operator()(berval **values) const noexcept
  {
    for (std::size_t index = 0; values[index]; ++index) {
      if (values[index]->bv_val)
        OPENSSL_cleanse(values[index]->bv_val, values[index]->bv_len);
    }
    ldap_value_free_len(values);
  }
};

l_timeval native_timeout(std::chrono::steady_clock::time_point deadline)
{
  auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
  if (remaining <= std::chrono::microseconds::zero())
    remaining = std::chrono::microseconds{1};
  return {static_cast<LONG>(remaining.count() / 1000000), static_cast<LONG>(remaining.count() % 1000000)};
}

bool unavailable(ULONG code)
{
  return code == LDAP_SERVER_DOWN || code == LDAP_CONNECT_ERROR || code == LDAP_TIMEOUT;
}

} // namespace
#endif

Result<std::optional<LdapValues>> ldap_query(const LdapQuery &query, std::chrono::milliseconds timeout)
{
#ifndef WEAVE_POSTGRES_LDAP
  static_cast<void>(query);
  static_cast<void>(timeout);
  return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#else
  auto deadline = std::chrono::steady_clock::now() + timeout;
  auto host = wide(query.ipv6 ? "[" + query.host + "]" : query.host);
  auto base = wide(query.base);
  auto attribute = wide(query.attribute);
  auto filter = wide(query.filter);
  if (!host || !base || !attribute || !filter)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  std::unique_ptr<LDAP, SessionCloser> session{ldap_initW(host->data(), query.port)};
  if (!session)
    return std::unexpected(ldap_error(LdapGetLastError()));
  auto handle = session.get();
  ULONG version = LDAP_VERSION3;
  ULONG dereference = LDAP_DEREF_NEVER;
  ULONG send_seconds = static_cast<ULONG>((timeout.count() + 999) / 1000);
  ULONG code = LDAP_SUCCESS;
  if ((code = ldap_set_optionW(handle, LDAP_OPT_PROTOCOL_VERSION, &version)) != LDAP_SUCCESS ||
    (code = ldap_set_optionW(handle, LDAP_OPT_REFERRALS, LDAP_OPT_OFF)) != LDAP_SUCCESS ||
    (code = ldap_set_optionW(handle, LDAP_OPT_AUTO_RECONNECT, LDAP_OPT_OFF)) != LDAP_SUCCESS ||
    (code = ldap_set_optionW(handle, LDAP_OPT_AREC_EXCLUSIVE, LDAP_OPT_ON)) != LDAP_SUCCESS ||
    (code = ldap_set_optionW(handle, LDAP_OPT_DEREF, &dereference)) != LDAP_SUCCESS ||
    (code = ldap_set_optionW(handle, LDAP_OPT_SEND_TIMEOUT, &send_seconds)) != LDAP_SUCCESS)
    return std::unexpected(ldap_error(code));
  auto limit = native_timeout(deadline);
  code = ldap_connect(handle, &limit);
  if (code != LDAP_SUCCESS) {
    if (unavailable(code))
      return std::optional<LdapValues>{};
    return std::unexpected(ldap_error(code));
  }
  if (std::chrono::steady_clock::now() >= deadline)
    return std::unexpected(std::make_error_code(std::errc::timed_out));

  ULONG scope = LDAP_SCOPE_BASE;
  if (query.scope == LdapScope::one)
    scope = LDAP_SCOPE_ONELEVEL;
  else if (query.scope == LdapScope::sub)
    scope = LDAP_SCOPE_SUBTREE;
  wchar_t *attributes[] = {attribute->data(), nullptr};
  LDAPMessage *response = nullptr;
  limit = native_timeout(deadline);
  code = ldap_search_ext_sW(
    handle,
    base->data(),
    scope,
    filter->data(),
    attributes,
    0,
    nullptr,
    nullptr,
    &limit,
    2,
    &response);
  std::unique_ptr<LDAPMessage, MessageCloser> message{response};
  if (code != LDAP_SUCCESS)
    return std::unexpected(ldap_error(code));
  if (ldap_count_entries(handle, response) != 1)
    return std::unexpected(std::make_error_code(std::errc::protocol_error));
  auto entry = ldap_first_entry(handle, response);
  if (!entry)
    return std::unexpected(std::make_error_code(std::errc::protocol_error));
  std::unique_ptr<berval *, ValuesCloser> values{ldap_get_values_lenW(handle, entry, attribute->data())};
  if (!values || !values.get()[0])
    return std::unexpected(std::make_error_code(std::errc::protocol_error));

  LdapValues result;
  std::size_t bytes = 0;
  for (std::size_t index = 0; values.get()[index]; ++index) {
    auto value = values.get()[index];
    if (index == 1024 || value->bv_len > maximum_connection_input - bytes)
      return std::unexpected(std::make_error_code(std::errc::value_too_large));
    bytes += value->bv_len;
    if (!value->bv_len)
      result.emplace_back();
    else if (value->bv_val)
      result.emplace_back(value->bv_val, value->bv_val + value->bv_len);
    else
      return std::unexpected(std::make_error_code(std::errc::protocol_error));
  }
  return std::optional<LdapValues>{std::move(result)};
#endif
}

} // namespace weave::pg::detail
