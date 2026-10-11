#pragma once

#include <weave/task.hpp>
#include <weave/database/detail/row_shape.hpp>

namespace weave::db {

namespace detail {

template <class T, class Shape, class Row, class Selectors>
Result<T> decode_record(const Row &row, const Selectors &selectors)
{
  T value{};
  auto members = Shape::tie(value);
  Result<void> status;

  auto decode = [&]<std::size_t Index>() {
    auto &member = std::get<Index>(members);
    using Member = std::remove_cvref_t<decltype(member)>;

    if (!status)
      return;

    auto decoded = row[selectors[Index]].template as<Member>();
    if (!decoded) {
      status = std::unexpected(decoded.error());
      return;
    }
    member = std::move(*decoded);
  };

  [&]<std::size_t... Index>(std::index_sequence<Index...>) {
    (decode.template operator()<Index>(), ...);
  }(std::make_index_sequence<Shape::names.size()>{});

  if (!status)
    return std::unexpected(status.error());
  return value;
}

} // namespace detail

/// Synchronously map an lvalue row to T; matching aggregate member/column names need no registration.
/// RowTraits<T> optionally supplies aliases. Selection/conversion come from the row's driver codec.
/// Owning members retain their values; driver-supported view members still borrow the driver's row storage.
/// This does not extend borrowed storage lifetimes, and temporary rows are rejected.
template <class T, class Row>
Result<T> decode(const Row &row)
{
  using Shape = detail::RowShape<T>;
  return detail::decode_record<T, Shape>(row, Shape::names);
}

// Decoded views must not silently borrow a temporary row.
template <class T, class Row>
  requires(!std::is_lvalue_reference_v<Row>)
Result<T> decode(Row &&) = delete;

} // namespace weave::db
