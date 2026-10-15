#pragma once

#include "tls_certificates.hpp"
#include <openssl/pem.h>
#include <string_view>

namespace fixture {

struct EncryptedCertificates {
  Certificates certificates;
  std::string client_key;
  std::string server_key;
  std::string edge_key;

  explicit EncryptedCertificates(std::string_view password)
      : client_key((certificates.directory / "encrypted-client.pem").string()),
        server_key((certificates.directory / "encrypted-server.pem").string()),
        edge_key((certificates.directory / "edge-key.pem").string())
  {
    encrypt(certificates.client_key, client_key, password);
    encrypt(certificates.private_key, server_key, password);
  }

  EncryptedCertificates(const EncryptedCertificates &) = delete;

  ~EncryptedCertificates()
  {
    const std::array paths{client_key, server_key, edge_key};
    for (const auto &path : paths) {
      std::error_code error;
      std::filesystem::remove(path, error);
      require(!error);
    }
  }

  static void encrypt(const std::string &input, const std::string &output, std::string_view password)
  {
    Bio source{BIO_new_file(input.c_str(), "rb"), BIO_free};
    require(source != nullptr);
    Key key{PEM_read_bio_PrivateKey(source.get(), nullptr, no_password, nullptr), EVP_PKEY_free};
    require(key != nullptr);
    Bio destination{BIO_new_file(output.c_str(), "wb"), BIO_free};
    require(destination != nullptr);

    require(
      PEM_write_bio_PKCS8PrivateKey(
        destination.get(),
        key.get(),
        EVP_aes_256_cbc(),
        const_cast<char *>(password.data()),
        static_cast<int>(password.size()),
        nullptr,
        nullptr) == 1);
  }

private:
  static int no_password(char *, int, int, void *) noexcept
  {
    return -1;
  }
};

} // namespace fixture
