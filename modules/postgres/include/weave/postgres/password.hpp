#pragma once

#include <weave/core.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace weave::pg {

enum class PasswordAlgorithm {
  scram_sha256,
  md5
};

struct PasswordOptions {
  std::optional<PasswordAlgorithm> algorithm;
  u32 iterations = 4096;
  bool allow_md5 = false;
};

Result<std::string> password_verifier(
  std::string_view user,
  std::string_view password,
  PasswordAlgorithm algorithm = PasswordAlgorithm::scram_sha256,
  u32 iterations = 4096);

} // namespace weave::pg
