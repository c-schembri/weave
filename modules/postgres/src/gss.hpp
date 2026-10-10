#pragma once

#include "auth.hpp"

namespace weave::pg::detail {

// PostgreSQL's 16 KiB protected-packet limit includes the four-byte wire length.
inline constexpr std::size_t gss_record_limit = 16384 - 4;

struct GssOptions {
  std::string service = "postgres";
  std::string credential_cache;
  bool delegate = false;
  bool require_mutual = true;
  bool protect = false;
};

struct GssToken {
  enum class Mechanism {
    unknown,
    kerberos,
    ntlm
  };

  SecretStorage<std::byte> bytes;
  Mechanism mechanism = Mechanism::unknown;
  bool complete = false;
  bool mutual = false;
  bool delegated = false;
};

// Native provider calls can contact a KDC. The async adapter must offload and drain them.
class Gss {
  struct State;
  std::unique_ptr<State> state_;

public:
  Gss();
  ~Gss();
  Gss(const Gss &) = delete;
  Gss &operator=(const Gss &) = delete;

  static bool available() noexcept;
  Result<GssToken> start(std::string_view host, Authentication method, const GssOptions &options);
  Result<GssToken> next(std::span<const std::byte> input);
  Result<SecretStorage<std::byte>> wrap(std::span<const std::byte> input);
  Result<SecretStorage<std::byte>> unwrap(std::span<const std::byte> input);
  std::size_t plaintext_limit() const noexcept;
  bool complete() const noexcept;
  std::string_view diagnostic() const noexcept;
};

bool valid_gss_target(std::string_view host, std::string_view service) noexcept;

} // namespace weave::pg::detail
