#pragma once

#include <string_view>
#include <tuple>

namespace weave::pg {

// Optional override for aliases and types that are not simple aggregates.
template <class T>
struct RowTraits {};

template <class Owner, class Member>
struct RowField {
  std::string_view name;
  Member Owner::*member;
};

template <class Owner, class Member>
constexpr RowField<Owner, Member> field(std::string_view name, Member Owner::*member) noexcept
{
  return {name, member};
}

} // namespace weave::pg
