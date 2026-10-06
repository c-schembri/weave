#pragma once

struct x509_store_st;

namespace weave::detail {

bool system_roots(x509_store_st *store) noexcept;

}
