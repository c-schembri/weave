#pragma once

#include <weave/postgres/mapping.hpp>
#include <weave/postgres/row.hpp>
#include <weave/postgres/detail/vendor/pfr/core.hpp>
#include <weave/postgres/detail/vendor/pfr/core_name.hpp>
#include <array>
#include <concepts>
#include <type_traits>
#include <utility>

namespace weave::pg::detail {

template <class T, class Entry>
struct RowMappingEntry : std::false_type {};

template <class T, class Owner, class Member>
struct RowMappingEntry<T, RowField<Owner, Member>> {
  static constexpr bool value = !std::is_function_v<Member> &&
    requires(T &object, Member Owner::*member) { object.*member; };
};

template <class T, class Fields>
struct RowMapping {
  static constexpr bool entries_valid = false;
  static constexpr bool pointers_valid = false;
};

template <class T, class... Entry>
struct RowMapping<T, std::tuple<Entry...>> {
  static constexpr bool entries_valid = (RowMappingEntry<T, Entry>::value && ...);
  static constexpr bool pointers_valid = [] {
    if constexpr (entries_valid) {
      return std::apply(
        [](const auto &...entry) {
          return ((entry.member != nullptr) && ...);
        },
        RowTraits<T>::fields);
    } else {
      return false;
    }
  }();
};

template <class Member>
inline constexpr bool row_member = primitive_value<std::remove_cvref_t<Member>> &&
  std::is_assignable_v<Member, std::remove_cvref_t<Member>> && !std::is_volatile_v<std::remove_reference_t<Member>>;

template <class T>
consteval bool validate_row_mapping()
{
  if constexpr (requires { RowTraits<T>::fields; }) {
    using Mapping = RowMapping<T, std::remove_cvref_t<decltype(RowTraits<T>::fields)>>;
    static_assert(Mapping::entries_valid, "RowTraits<T>::fields must be a tuple of fields belonging to T");
    static_assert(
      !Mapping::entries_valid || Mapping::pointers_valid,
      "PostgreSQL row mappings cannot contain null member pointers");
    return Mapping::entries_valid && Mapping::pointers_valid;
  } else {
    return true;
  }
}

template <class T>
struct RowShape {
  static constexpr bool mapped = requires { RowTraits<T>::fields; };
  static constexpr bool type_valid = std::is_class_v<T> && !std::is_union_v<T> &&
    std::same_as<T, std::remove_cv_t<T>> && (mapped || std::is_aggregate_v<T>);
  static constexpr bool construction_valid = std::default_initializable<T> && std::move_constructible<T>;

  static_assert(
    std::is_class_v<T> && !std::is_union_v<T> && std::same_as<T, std::remove_cv_t<T>>,
    "PostgreSQL row decoding requires an unqualified struct type");
  static_assert(
    mapped || std::is_aggregate_v<T>,
    "PostgreSQL automatic row decoding requires a simple aggregate; use RowTraits<T> for other types");
  static_assert(construction_valid, "PostgreSQL row decoding requires a default-constructible, movable type");

  static constexpr bool mapping_valid = validate_row_mapping<T>();

  static auto tie(T &value) noexcept
  {
    // Avoid instantiating reflection/member access after a failed shape check.
    if constexpr (!type_valid || !construction_valid || !mapping_valid) {
      return std::tuple{};
    } else if constexpr (mapped) {
      return std::apply(
        [&](const auto &...entry) {
          return std::tie(value.*entry.member...);
        },
        RowTraits<T>::fields);
    } else {
      return weave_pg_vendor::pfr::structure_tie(value);
    }
  }

  using Members = decltype(tie(std::declval<T &>()));
  static constexpr bool members_valid = []<std::size_t... Index>(std::index_sequence<Index...>) {
    return (row_member<std::tuple_element_t<Index, Members>> && ...);
  }(std::make_index_sequence<std::tuple_size_v<Members>>{});
  static_assert(members_valid, "PostgreSQL struct members must be writable supported primitives or optionals");

  static constexpr auto names = [] {
    if constexpr (!type_valid || !construction_valid || !mapping_valid || !members_valid) {
      return std::array<std::string_view, 0>{};
    } else if constexpr (mapped) {
      return std::apply(
        [](const auto &...entry) {
          return std::array<std::string_view, sizeof...(entry)>{entry.name...};
        },
        RowTraits<T>::fields);
    } else {
      return weave_pg_vendor::pfr::names_as_array<T>();
    }
  }();
};

} // namespace weave::pg::detail
