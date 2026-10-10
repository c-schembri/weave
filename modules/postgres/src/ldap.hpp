#pragma once

#include "secret.hpp"
#include <weave/task.hpp>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace weave::pg::detail {

enum class LdapScope {
  base,
  one,
  sub
};

struct LdapQuery {
  std::string host;
  u16 port = 389;
  std::string base;
  std::string attribute;
  LdapScope scope = LdapScope::base;
  std::string filter;
  bool ipv6 = false;
};

using LdapValues = std::vector<SecretText>;

Result<LdapQuery> ldap_url(std::string_view url);
// Missing value means pre-search unavailability; search/data/policy failures are terminal.
Result<std::optional<LdapValues>> ldap_query(const LdapQuery &query, std::chrono::milliseconds timeout);

} // namespace weave::pg::detail
