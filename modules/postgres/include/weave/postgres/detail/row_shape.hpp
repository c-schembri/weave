#pragma once

#include <weave/postgres/mapping.hpp>
#include <weave/postgres/row.hpp>
#include <weave/database/detail/row_shape.hpp>

namespace weave::pg::detail {

struct RowCodec {
  template <class Member>
  static constexpr bool supports = primitive_value<Member>;
};

template <class T>
using RowShape = db::detail::RowShape<T, RowTraits<T>, RowCodec>;

} // namespace weave::pg::detail
