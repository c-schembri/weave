#pragma once

#include <weave/core.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace weave::pg {

/// Stored password verifier algorithm. SCRAM is preferred; MD5 is an explicit legacy choice.
enum class PasswordAlgorithm {
  scram_sha256,
  md5
};

/// Session password-operation policy; absent algorithm queries server policy, MD5 requires opt-in.
struct PasswordOptions {
  std::optional<PasswordAlgorithm> algorithm;
  u32 iterations = 4096;
  bool allow_md5 = false;
};

/// Generate an owning stored verifier synchronously, borrowing user/password only during this call.
/// No session needed; defaults to SCRAM. Output is credential material and must not be logged.
Result<std::string> password_verifier(
  std::string_view user,
  std::string_view password,
  PasswordAlgorithm algorithm = PasswordAlgorithm::scram_sha256,
  u32 iterations = 4096);

} // namespace weave::pg
