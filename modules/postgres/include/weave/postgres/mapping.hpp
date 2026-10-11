#pragma once

#include <weave/database/mapping.hpp>

namespace weave::pg {

/// Optional PostgreSQL override for aliases/non-aggregates; inherits shared db::RowTraits by default.
/// Plain matching public aggregates need no specialization.
template <class T>
struct RowTraits : db::RowTraits<T> {};

/// Borrowed column-name/member descriptor shared with weave::db.
template <class Owner, class Member>
using RowField = db::RowField<Owner, Member>;

/// Describe a mapped field without allocation; name must outlive descriptor use (normally a literal).
template <class Owner, class Member>
constexpr RowField<Owner, Member> field(std::string_view name, Member Owner::*member) noexcept
{
  return db::field(name, member);
}

} // namespace weave::pg
