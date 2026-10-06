#pragma once

#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <filesystem>
#include <memory>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <array>

namespace fixture {

inline void require(bool condition)
{
  if (!condition)
    std::abort();
}

using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;

inline Key key()
{
  Key value{EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"), EVP_PKEY_free};
  require(value != nullptr);

  return value;
}

inline void extension(X509 *certificate, X509 *issuer, int id, const char *value)
{
  X509V3_CTX context{};
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);

  auto *item = X509V3_EXT_conf_nid(nullptr, &context, id, value);
  require(item != nullptr);
  const auto added = X509_add_ext(certificate, item, -1);
  X509_EXTENSION_free(item);
  require(added == 1);
}

inline Certificate certificate(EVP_PKEY *key, X509 *issuer, EVP_PKEY *signer, long serial, bool expired)
{
  Certificate value{X509_new(), X509_free};
  require(value != nullptr);
  require(X509_set_version(value.get(), 2) == 1);
  require(ASN1_INTEGER_set(X509_get_serialNumber(value.get()), serial) == 1);
  require(X509_gmtime_adj(X509_getm_notBefore(value.get()), -86400) != nullptr);
  require(X509_gmtime_adj(X509_getm_notAfter(value.get()), expired ? -60 : 86400) != nullptr);
  require(X509_set_pubkey(value.get(), key) == 1);

  auto *name = X509_get_subject_name(value.get());
  const char *common_name = issuer ? "localhost" : "Weave test CA";
  require(
    X509_NAME_add_entry_by_txt(
      name,
      "CN",
      MBSTRING_ASC,
      reinterpret_cast<const unsigned char *>(common_name),
      -1,
      -1,
      0) == 1);
  require(X509_set_issuer_name(value.get(), issuer ? X509_get_subject_name(issuer) : name) == 1);

  auto *authority = issuer ? issuer : value.get();
  extension(value.get(), authority, NID_basic_constraints, issuer ? "critical,CA:FALSE" : "critical,CA:TRUE");
  extension(
    value.get(),
    authority,
    NID_key_usage,
    issuer ? "critical,digitalSignature" : "critical,keyCertSign,cRLSign");
  if (issuer) {
    extension(value.get(), authority, NID_ext_key_usage, "serverAuth");
    extension(value.get(), authority, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1,IP:::1");
  }

  require(X509_sign(value.get(), signer ? signer : key, EVP_sha256()) > 0);

  return value;
}

inline void write_certificate(const std::filesystem::path &path, X509 *certificate)
{
  Bio file{BIO_new_file(path.string().c_str(), "w"), BIO_free};
  require(file != nullptr);
  require(PEM_write_bio_X509(file.get(), certificate) == 1);
}

struct Certificates {
  std::filesystem::path directory;
  std::string ca;
  std::string untrusted;
  std::string leaf;
  std::string expired;
  std::string private_key;

  Certificates()
  {
    static std::atomic<unsigned> sequence = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto parent = std::filesystem::temp_directory_path();
    for (;;) {
      directory = parent / ("weave-tls-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
      std::error_code error;
      if (std::filesystem::create_directory(directory, error))
        break;
      require(!error);
    }

    ca = (directory / "ca.pem").string();
    untrusted = (directory / "other-ca.pem").string();
    leaf = (directory / "server.pem").string();
    expired = (directory / "expired.pem").string();
    private_key = (directory / "key.pem").string();

    auto root_key = key();
    auto root = certificate(root_key.get(), nullptr, nullptr, 1, false);
    auto other_key = key();
    auto other = certificate(other_key.get(), nullptr, nullptr, 2, false);
    auto server_key = key();
    auto server = certificate(server_key.get(), root.get(), root_key.get(), 3, false);
    auto stale = certificate(server_key.get(), root.get(), root_key.get(), 4, true);

    write_certificate(ca, root.get());
    write_certificate(untrusted, other.get());
    write_certificate(leaf, server.get());
    write_certificate(expired, stale.get());

    Bio file{BIO_new_file(private_key.c_str(), "w"), BIO_free};
    require(file != nullptr);
    require(PEM_write_bio_PrivateKey(file.get(), server_key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
  }

  Certificates(const Certificates &) = delete;

  ~Certificates()
  {
    const std::array files{ca, untrusted, leaf, expired, private_key};
    std::error_code error;
    for (const auto &file : files)
      std::filesystem::remove(file, error);
    std::filesystem::remove(directory, error);
  }
};

} // namespace fixture
