#pragma once

#include <string_view>
#include <tuple>

namespace weave::db {

/// Optional named-field customization for aliases or non-aggregate records.
/// Matching public aggregates need no specialization. Drivers may inherit this with vendor overrides.
template <class T>
struct RowTraits {};

/// Column-name/member mapping; the name is borrowed, typically from a static string literal.
template <class Owner, class Member>
struct RowField {
  std::string_view name;
  Member Owner::*member;
};

/// Describe a record field without allocation. The name must outlive every use of the descriptor.
template <class Owner, class Member>
constexpr RowField<Owner, Member> field(std::string_view name, Member Owner::*member) noexcept
{
  return {name, member};
}

} // namespace weave::db
