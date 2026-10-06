#include "../trust.hpp"
#include <openssl/x509.h>

namespace weave {

bool detail::system_roots(X509_STORE *store) noexcept
{
  return X509_STORE_set_default_paths(store) == 1;
}

} // namespace weave
