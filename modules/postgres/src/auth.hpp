#pragma once

#include "wire.hpp"
#include "secret.hpp"

namespace weave::pg::detail {

using SecretWriter = BasicWriter<SecretStorage<std::byte>>;

class Scram {
  struct State;
  std::unique_ptr<State> state_;

public:
  Scram();
  ~Scram();
  Scram(const Scram &) = delete;

  Result<SecretWriter> start(
    const Options &options,
    std::span<const std::byte> mechanisms,
    std::span<const std::byte> binding);
  Result<SecretWriter> challenge(std::span<const std::byte> message, u32 max_iterations);
  Result<void> verify(std::span<const std::byte> message);
  bool started() const noexcept;
  bool verified() const noexcept;
};

Result<SecretText> md5_password(std::string_view user, std::string_view password, std::span<const std::byte> salt);
Result<SecretText> own_password(std::string_view password);
Result<SecretText> password_verifier(
  std::string_view password,
  std::string_view user,
  PasswordAlgorithm algorithm,
  u32 iterations);

} // namespace weave::pg::detail
