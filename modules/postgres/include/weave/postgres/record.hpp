#pragma once

#include <weave/postgres/connection.hpp>
#include <weave/postgres/detail/row_shape.hpp>

namespace weave::pg {

namespace detail {

template <class T, class Selectors>
Result<T> decode_record(const Row &row, const Selectors &selectors)
{
  T value{};
  auto members = RowShape<T>::tie(value);
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
  }(std::make_index_sequence<RowShape<T>::names.size()>{});

  if (!status)
    return std::unexpected(status.error());
  return value;
}

} // namespace detail

template <class T>
Result<T> Row::as() const &
{
  return detail::decode_record<T>(*this, detail::RowShape<T>::names);
}

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
  std::array<std::size_t, names.size()> indices{};
  for (std::size_t index = 0; index < names.size(); ++index) {
    auto selected = column(names[index]);
    if (!selected)
      return std::unexpected(selected.error());
    indices[index] = selected->index();
  }
  return RowDecoder<T>{row_schema_, indices};
}

} // namespace weave::pg
