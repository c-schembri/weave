#include "auth.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <unicode/usprep.h>
#include <unicode/ustring.h>
#include <array>
#include <map>
#include <algorithm>
#include <cstring>

namespace weave::pg {

namespace {

using Digest = detail::SecretArray<32>;

std::string_view text(const detail::SecretText &value) noexcept
{
  return {value.data(), value.size()};
}

void append(detail::SecretText &destination, std::string_view value)
{
  destination.insert(destination.end(), value.begin(), value.end());
}

std::string base64(std::span<const unsigned char> input)
{
  std::string output(4 * ((input.size() + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char *>(output.data()), input.data(), static_cast<int>(input.size()));
  return output;
}

Result<std::vector<unsigned char>> unbase64(std::string_view input)
{
  if (input.empty() || input.size() % 4 || input.size() > 65536)
    return std::unexpected(make_error_code(Error::authentication));

  std::vector<unsigned char> output((input.size() / 4) * 3);
  auto count = EVP_DecodeBlock(
    output.data(),
    reinterpret_cast<const unsigned char *>(input.data()),
    static_cast<int>(input.size()));
  if (count < 0)
    return std::unexpected(make_error_code(Error::authentication));

  if (input.back() == '=')
    --count;
  if (input[input.size() - 2] == '=')
    --count;

  output.resize(static_cast<std::size_t>(count));
  if (base64(output) != input)
    return std::unexpected(make_error_code(Error::authentication));

  return output;
}

Result<Digest> hmac(std::span<const unsigned char> key, std::string_view input)
{
  Digest output;
  unsigned int size = 0;
  auto result = HMAC(
    EVP_sha256(),
    key.data(),
    static_cast<int>(key.size()),
    reinterpret_cast<const unsigned char *>(input.data()),
    input.size(),
    output.bytes.data(),
    &size);
  if (!result || size != output.bytes.size())
    return std::unexpected(make_error_code(Error::authentication));

  return output;
}

Result<detail::SecretText> md5_hash(std::string_view input)
{
  detail::SecretArray<16> digest;
  unsigned int size = 0;
  if (EVP_Digest(input.data(), input.size(), digest.bytes.data(), &size, EVP_md5(), nullptr) != 1 ||
    size != digest.bytes.size())
    return std::unexpected(make_error_code(Error::authentication));

  const std::string_view digits = "0123456789abcdef";
  detail::SecretText output;
  for (auto byte : digest.bytes) {
    output.push_back(digits[byte >> 4]);
    output.push_back(digits[byte & 15]);
  }

  return output;
}

detail::SecretText secret_base64(std::span<const unsigned char> input)
{
  detail::SecretText output(4 * ((input.size() + 2) / 3) + 1, '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char *>(output.data()), input.data(), static_cast<int>(input.size()));
  output.pop_back();
  return output;
}

Result<std::map<char, std::string>> attributes(std::span<const std::byte> bytes)
{
  if (bytes.empty() || bytes.size() > 65536)
    return std::unexpected(make_error_code(Error::authentication));

  std::string_view message{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
  std::map<char, std::string> result;
  while (!message.empty()) {
    auto comma = message.find(',');
    auto field = message.substr(0, comma);
    if (field.size() < 3 || field[1] != '=' || field[0] == 'm' || result.contains(field[0]) ||
      !detail::cstring_valid(field))
      return std::unexpected(make_error_code(Error::authentication));

    result.emplace(field[0], field.substr(2));
    if (comma == std::string_view::npos)
      break;

    message.remove_prefix(comma + 1);
    if (message.empty())
      return std::unexpected(make_error_code(Error::authentication));
  }

  return result;
}

detail::SecretText saslprep(std::string_view password)
{
  UErrorCode status = U_ZERO_ERROR;
  int32_t size = 0;
  u_strFromUTF8(nullptr, 0, &size, password.data(), static_cast<int32_t>(password.size()), &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status))
    return {password.begin(), password.end()};

  detail::SecretStorage<UChar> input(static_cast<std::size_t>(size) + 1);
  status = U_ZERO_ERROR;
  u_strFromUTF8(
    input.data(),
    static_cast<int32_t>(input.size()),
    nullptr,
    password.data(),
    static_cast<int32_t>(password.size()),
    &status);
  auto profile = usprep_openByType(USPREP_RFC4013_SASLPREP, &status);
  if (U_FAILURE(status)) {
    return {password.begin(), password.end()};
  }

  UParseError parse{};
  status = U_ZERO_ERROR;
  auto prepared_size = usprep_prepare(profile, input.data(), size, nullptr, 0, USPREP_DEFAULT, &parse, &status);
  detail::SecretStorage<UChar> prepared(static_cast<std::size_t>((std::max)(prepared_size, int32_t{0})) + 1);
  if (status == U_BUFFER_OVERFLOW_ERROR) {
    status = U_ZERO_ERROR;
    prepared_size = usprep_prepare(
      profile,
      input.data(),
      size,
      prepared.data(),
      static_cast<int32_t>(prepared.size()),
      USPREP_DEFAULT,
      &parse,
      &status);
  }

  usprep_close(profile);
  if (U_FAILURE(status)) {
    return {password.begin(), password.end()};
  }

  status = U_ZERO_ERROR;
  int32_t output_size = 0;
  u_strToUTF8(nullptr, 0, &output_size, prepared.data(), prepared_size, &status);
  detail::SecretText output(static_cast<std::size_t>(output_size), '\0');
  status = U_ZERO_ERROR;
  u_strToUTF8(output.data(), output_size, nullptr, prepared.data(), prepared_size, &status);
  if (U_FAILURE(status))
    return {password.begin(), password.end()};

  return output;
}

} // namespace

Result<ScramKey> ScramKey::parse(std::string_view encoded)
{
  auto invalid = std::make_error_code(std::errc::invalid_argument);
  if (encoded.size() != 44 || encoded.back() != '=')
    return std::unexpected(invalid);

  const auto alphabet = encoded.substr(0, encoded.size() - 1);
  bool valid = std::ranges::all_of(alphabet, [](char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
      value == '+' || value == '/';
  });
  if (!valid)
    return std::unexpected(invalid);

  detail::SecretArray<33> decoded;
  auto size = EVP_DecodeBlock(
    decoded.bytes.data(),
    reinterpret_cast<const unsigned char *>(encoded.data()),
    static_cast<int>(encoded.size()));
  if (size != 33)
    return std::unexpected(invalid);

  return ScramKey{std::span<const std::byte, 32>{reinterpret_cast<const std::byte *>(decoded.bytes.data()), 32}};
}

ScramKey::ScramKey(std::span<const std::byte, 32> bytes) noexcept
{
  std::ranges::copy(bytes, bytes_.begin());
}

ScramKey::ScramKey(ScramKey &&other) noexcept : bytes_(other.bytes_)
{
  detail::clear_secret(other.bytes_.data(), other.bytes_.size());
}

ScramKey &ScramKey::operator=(ScramKey &&other) noexcept
{
  if (this != &other) {
    detail::clear_secret(bytes_.data(), bytes_.size());
    bytes_ = other.bytes_;
    detail::clear_secret(other.bytes_.data(), other.bytes_.size());
  }
  return *this;
}

ScramKey::~ScramKey()
{
  detail::clear_secret(bytes_.data(), bytes_.size());
}

Result<std::string> password_verifier(
  std::string_view user,
  std::string_view password,
  PasswordAlgorithm algorithm,
  u32 iterations)
{
  auto verifier = detail::password_verifier(password, user, algorithm, iterations);
  if (!verifier)
    return std::unexpected(verifier.error());
  return std::string{verifier->begin(), verifier->end()};
}

namespace detail {

void clear_secret(void *data, std::size_t size) noexcept
{
  OPENSSL_cleanse(data, size);
}

struct Scram::State {
  SecretText password;
  std::string nonce;
  std::string first;
  std::string header;
  std::vector<unsigned char> binding;
  Digest signature;
  std::optional<ScramKey> client_key;
  std::optional<ScramKey> server_key;
  bool challenged = false;
  bool verified = false;
};

Scram::Scram() = default;
Scram::~Scram() = default;

bool Scram::started() const noexcept
{
  return static_cast<bool>(state_);
}

bool Scram::verified() const noexcept
{
  return state_ && state_->verified;
}

Result<SecretWriter> Scram::start(
  const Options &options,
  std::span<const std::byte> mechanisms,
  std::span<const std::byte> binding)
{
  if (state_)
    return std::unexpected(make_error_code(Error::authentication));

  Reader reader{mechanisms};
  bool plain = false;
  bool plus = false;
  for (;;) {
    auto name = reader.string();
    if (name.empty())
      break;

    plain |= name == "SCRAM-SHA-256";
    plus |= name == "SCRAM-SHA-256-PLUS";
  }

  if (!reader.empty())
    return std::unexpected(make_error_code(Error::protocol));

  bool use_plus = plus && !binding.empty() && options.channel_binding != ChannelBinding::disable;
  if ((!use_plus && !plain) || (!use_plus && options.channel_binding == ChannelBinding::require))
    return std::unexpected(make_error_code(Error::authentication));

  state_ = std::make_unique<State>();
  if (options.scram_client_key && !options.scram_server_key && options.password.empty())
    return std::unexpected(make_error_code(Error::authentication));

  state_->client_key = options.scram_client_key;
  state_->server_key = options.scram_server_key;
  if (!state_->client_key || !state_->server_key)
    state_->password = saslprep(options.password);
  state_->header = use_plus ? "p=tls-server-end-point,,"
                            : (!binding.empty() && options.channel_binding != ChannelBinding::disable ? "y,," : "n,,");
  if (use_plus) {
    auto data = reinterpret_cast<const unsigned char *>(binding.data());
    state_->binding.assign(data, data + binding.size());
  }

  std::array<unsigned char, 18> random{};
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
    return std::unexpected(make_error_code(Error::authentication));

  state_->nonce = base64(random);
  state_->first = "n=,r=" + state_->nonce;
  auto first = state_->header + state_->first;
  SecretWriter response;
  response.string(use_plus ? "SCRAM-SHA-256-PLUS" : "SCRAM-SHA-256");
  response.integer(static_cast<u32>(first.size()));
  response.raw(first);
  return response;
}

Result<SecretWriter> Scram::challenge(std::span<const std::byte> message, u32 max_iterations)
{
  if (!state_ || state_->challenged)
    return std::unexpected(make_error_code(Error::authentication));

  auto fields = attributes(message);
  if (!fields || !fields->contains('r') || !fields->contains('s') || !fields->contains('i'))
    return std::unexpected(make_error_code(Error::authentication));

  auto &nonce = fields->at('r');
  if (!nonce.starts_with(state_->nonce) || nonce.size() <= state_->nonce.size())
    return std::unexpected(make_error_code(Error::authentication));

  const auto &iterations_text = fields->at('i');
  u32 iterations = 0;
  auto parsed = std::from_chars(iterations_text.data(), iterations_text.data() + iterations_text.size(), iterations);
  if (parsed.ec != std::errc{} || parsed.ptr != iterations_text.data() + iterations_text.size() || iterations < 4096 ||
    iterations > max_iterations)
    return std::unexpected(make_error_code(Error::authentication));

  auto salt = unbase64(fields->at('s'));
  if (!salt)
    return std::unexpected(salt.error());

  Digest client_key;
  Digest server_key;
  if (state_->client_key) {
    auto bytes = state_->client_key->bytes();
    std::memcpy(client_key.bytes.data(), bytes.data(), bytes.size());
  }
  if (state_->server_key) {
    auto bytes = state_->server_key->bytes();
    std::memcpy(server_key.bytes.data(), bytes.data(), bytes.size());
  }

  if (!state_->client_key || !state_->server_key) {
    Digest salted;
    auto &password = state_->password;
    bool derived = PKCS5_PBKDF2_HMAC(
                     password.data(),
                     static_cast<int>(password.size()),
                     salt->data(),
                     static_cast<int>(salt->size()),
                     static_cast<int>(iterations),
                     EVP_sha256(),
                     static_cast<int>(salted.bytes.size()),
                     salted.bytes.data()) == 1;
    if (!derived)
      return std::unexpected(make_error_code(Error::authentication));

    if (!state_->client_key) {
      auto derived_key = hmac(salted.bytes, "Client Key");
      if (!derived_key)
        return std::unexpected(derived_key.error());
      client_key = std::move(*derived_key);
    }
    if (!state_->server_key) {
      auto derived_key = hmac(salted.bytes, "Server Key");
      if (!derived_key)
        return std::unexpected(derived_key.error());
      server_key = std::move(*derived_key);
    }
  }

  Digest stored;
  unsigned int size = 0;
  bool hashed = EVP_Digest(
                  client_key.bytes.data(),
                  client_key.bytes.size(),
                  stored.bytes.data(),
                  &size,
                  EVP_sha256(),
                  nullptr) == 1;
  auto binding = state_->binding;
  binding.insert(binding.begin(), state_->header.begin(), state_->header.end());
  auto final = "c=" + base64(binding) + ",r=" + nonce;
  auto server_first = std::string{reinterpret_cast<const char *>(message.data()), message.size()};
  auto transcript = state_->first + "," + server_first + "," + final;
  auto client_signature = hmac(stored.bytes, transcript);
  auto server_signature = hmac(server_key.bytes, transcript);
  if (!hashed || size != stored.bytes.size() || !client_signature || !server_signature) {
    return std::unexpected(make_error_code(Error::authentication));
  }

  Digest proof;
  for (std::size_t index = 0; index < proof.bytes.size(); ++index)
    proof.bytes[index] = client_key.bytes[index] ^ client_signature->bytes[index];

  state_->signature = std::move(*server_signature);
  state_->challenged = true;
  SecretText encoded(4 * ((proof.bytes.size() + 2) / 3) + 1, '\0');
  EVP_EncodeBlock(
    reinterpret_cast<unsigned char *>(encoded.data()),
    proof.bytes.data(),
    static_cast<int>(proof.bytes.size()));
  encoded.pop_back();

  SecretWriter response;
  response.raw(final);
  response.raw(",p=");
  response.raw(text(encoded));
  return response;
}

Result<void> Scram::verify(std::span<const std::byte> message)
{
  if (!state_ || !state_->challenged || state_->verified)
    return std::unexpected(make_error_code(Error::authentication));

  auto fields = attributes(message);
  if (!fields || fields->contains('e') || !fields->contains('v'))
    return std::unexpected(make_error_code(Error::authentication));

  auto signature = unbase64(fields->at('v'));
  if (!signature || signature->size() != state_->signature.bytes.size() ||
    CRYPTO_memcmp(signature->data(), state_->signature.bytes.data(), state_->signature.bytes.size()) != 0)
    return std::unexpected(make_error_code(Error::authentication));

  state_->verified = true;
  return {};
}

Result<SecretText> md5_password(std::string_view user, std::string_view password, std::span<const std::byte> salt)
{
  if (salt.size() != 4)
    return std::unexpected(make_error_code(Error::protocol));

  SecretText input;
  append(input, password);
  append(input, user);
  auto first = md5_hash(text(input));
  if (!first)
    return std::unexpected(first.error());

  SecretText salted = std::move(*first);
  append(salted, {reinterpret_cast<const char *>(salt.data()), salt.size()});
  auto second = md5_hash(text(salted));
  if (!second)
    return std::unexpected(second.error());

  SecretText response;
  append(response, "md5");
  append(response, text(*second));
  return response;
}

Result<SecretText> own_password(std::string_view password)
{
  if (password.size() > 65536 || !cstring_valid(password))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  return SecretText{password.begin(), password.end()};
}

Result<SecretText> password_verifier(
  std::string_view password,
  std::string_view user,
  PasswordAlgorithm algorithm,
  u32 iterations)
{
  if (password.size() > 65536 || user.size() > 65536 || !cstring_valid(password) || !cstring_valid(user) ||
    iterations == 0 || iterations > 1000000)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  if (algorithm == PasswordAlgorithm::md5) {
    SecretText input;
    append(input, password);
    append(input, user);
    auto hash = md5_hash(text(input));
    if (!hash)
      return std::unexpected(hash.error());
    SecretText output;
    append(output, "md5");
    append(output, text(*hash));
    return output;
  }
  if (algorithm != PasswordAlgorithm::scram_sha256)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto prepared = saslprep(password);
  SecretArray<16> salt;
  Digest salted;
  bool random = RAND_bytes(salt.bytes.data(), static_cast<int>(salt.bytes.size())) == 1;
  bool derived = random &&
    PKCS5_PBKDF2_HMAC(
      prepared.data(),
      static_cast<int>(prepared.size()),
      salt.bytes.data(),
      static_cast<int>(salt.bytes.size()),
      static_cast<int>(iterations),
      EVP_sha256(),
      static_cast<int>(salted.bytes.size()),
      salted.bytes.data()) == 1;
  if (!derived)
    return std::unexpected(make_error_code(Error::authentication));

  auto client_key = hmac(salted.bytes, "Client Key");
  auto server_key = hmac(salted.bytes, "Server Key");
  if (!client_key || !server_key)
    return std::unexpected(make_error_code(Error::authentication));
  Digest stored;
  unsigned int size = 0;
  bool hashed = EVP_Digest(
                  client_key->bytes.data(),
                  client_key->bytes.size(),
                  stored.bytes.data(),
                  &size,
                  EVP_sha256(),
                  nullptr) == 1;
  if (!hashed || size != stored.bytes.size())
    return std::unexpected(make_error_code(Error::authentication));

  SecretText output;
  append(output, "SCRAM-SHA-256$");
  append(output, std::to_string(iterations));
  append(output, ":");
  append(output, base64(salt.bytes));
  append(output, "$");
  append(output, text(secret_base64(stored.bytes)));
  append(output, ":");
  append(output, text(secret_base64(server_key->bytes)));
  return output;
}

} // namespace detail
} // namespace weave::pg
