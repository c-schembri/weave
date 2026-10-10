#include "gss.hpp"
#include <algorithm>

namespace weave::pg::detail {

bool valid_gss_target(std::string_view host, std::string_view service) noexcept
{
  const auto invalid_host = [](unsigned char value) {
    return value <= 32 || value == 127 || value == '/' || value == '\\' || value == '@';
  };
  const auto invalid_service = [](unsigned char value) {
    bool alpha = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
    bool digit = value >= '0' && value <= '9';
    return !alpha && !digit && value != '-' && value != '_' && value != '.';
  };
  return !host.empty() && host.size() <= 65535 && !service.empty() && service.size() <= 256 &&
    !std::ranges::any_of(host, invalid_host) && !std::ranges::any_of(service, invalid_service);
}

} // namespace weave::pg::detail
