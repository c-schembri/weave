#pragma once

#include <weave/postgres/connection.hpp>
#include <weave/postgres/detail/row_shape.hpp>
#include <weave/database/record.hpp>
#include <weave/database/selection.hpp>

namespace weave::pg {

namespace detail {

template <class T, class Selectors>
Result<T> decode_record(const Row &row, const Selectors &selectors)
{
  return db::detail::decode_record<T, RowShape<T>>(row, selectors);
}

} // namespace detail

template <class T>
Result<T> Row::as() const &
{
  return detail::decode_record<T>(*this, detail::RowShape<T>::names);
}

/// Cached fixed-size struct mapping, retaining schema identity but not field payloads.
/// Bind with ResultSet::decoder<T>(); automatic member names need no RowTraits specialization.
template <class T>
class RowDecoder {
  std::shared_ptr<const detail::RowSchema> schema_;
  std::array<std::size_t, detail::RowShape<T>::names.size()> indices_;
  friend struct ResultSet;

  RowDecoder(std::shared_ptr<const detail::RowSchema> schema, decltype(indices_) indices) noexcept
      : schema_(std::move(schema)), indices_(indices)
  {
  }

public:
  /// Decode only a row from the bound schema, or return schema_mismatch.
  /// Owning members copy; view members borrow row data. Temporary rows are rejected.
  Result<T> decode(const Row &row) const
  {
    if (!schema_ || schema_ != row.schema_)
      return std::unexpected(make_error_code(DecodeError::schema_mismatch));
    return detail::decode_record<T>(row, indices_);
  }

  Result<T> decode(Row &&) const = delete;
  Result<T> decode(const Row &&) const = delete;
};

template <class T>
Result<RowDecoder<T>> ResultSet::decoder() const
{
  if (!row_schema_)
    return std::unexpected(make_error_code(DecodeError::missing_column));

  constexpr auto &names = detail::RowShape<T>::names;
  auto indices = db::bind_columns(names, [&](std::string_view name) -> Result<std::size_t> {
    auto selected = column(name);
    if (!selected)
      return std::unexpected(selected.error());
    return selected->index();
  });
  if (!indices)
    return std::unexpected(indices.error());
  return RowDecoder<T>{row_schema_, *indices};
}

} // namespace weave::pg
