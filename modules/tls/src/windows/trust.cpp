#include "../trust.hpp"
#include <windows.h>
#include <wincrypt.h>
#include <openssl/x509.h>
#include <openssl/err.h>

namespace weave {

bool detail::system_roots(X509_STORE *store) noexcept
{
  auto roots = CertOpenSystemStoreW(0, L"ROOT");
  if (!roots)
    return false;

  PCCERT_CONTEXT certificate = nullptr;
  bool added = false;

  while ((certificate = CertEnumCertificatesInStore(roots, certificate))) {
    const auto *encoded = certificate->pbCertEncoded;
    auto *decoded = d2i_X509(nullptr, &encoded, certificate->cbCertEncoded);
    if (decoded) {
      added |= X509_STORE_add_cert(store, decoded) == 1;
      X509_free(decoded);
    }

    ERR_clear_error();
  }

  CertCloseStore(roots, 0);

  return added;
}

} // namespace weave
