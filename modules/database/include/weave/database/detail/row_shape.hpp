#pragma once

#include <weave/database/mapping.hpp>
#include <weave/database/detail/vendor/pfr/core.hpp>
#include <weave/database/detail/vendor/pfr/core_name.hpp>
#include <array>
#include <concepts>
#include <type_traits>
#include <utility>

namespace weave::db::detail {

template <class T, class Entry>
struct RowMappingEntry : std::false_type {};

template <class T, class Owner, class Member>
struct RowMappingEntry<T, RowField<Owner, Member>> {
  static constexpr bool value = !std::is_function_v<Member> &&
    requires(T &object, Member Owner::*member) { object.*member; };
};

template <class T, class Traits, class Fields>
struct RowMapping {
  static constexpr bool entries_valid = false;
  static constexpr bool pointers_valid = false;
};

template <class T, class Traits, class... Entry>
struct RowMapping<T, Traits, std::tuple<Entry...>> {
  static constexpr bool entries_valid = (RowMappingEntry<T, Entry>::value && ...);
  static constexpr bool pointers_valid = [] {
    if constexpr (entries_valid) {
      return std::apply(
        [](const auto &...entry) {
          return ((entry.member != nullptr) && ...);
        },
        Traits::fields);
    } else {
      return false;
    }
  }();
};

template <class T, class Traits>
consteval bool validate_row_mapping()
{
  if constexpr (requires { Traits::fields; }) {
    using Mapping = RowMapping<T, Traits, std::remove_cvref_t<decltype(Traits::fields)>>;
    static_assert(Mapping::entries_valid, "RowTraits<T>::fields must be a tuple of fields belonging to T");
    static_assert(
      !Mapping::entries_valid || Mapping::pointers_valid,
      "Row mappings cannot contain null member pointers");
    return Mapping::entries_valid && Mapping::pointers_valid;
  } else {
    return true;
  }
}

struct WritableMember {
  template <class Member>
  static constexpr bool supports = true;
};

template <class T, class Traits = RowTraits<T>, class Codec = WritableMember>
struct RowShape {
  static constexpr bool mapped = requires { Traits::fields; };
  static constexpr bool type_valid = std::is_class_v<T> && !std::is_union_v<T> &&
    std::same_as<T, std::remove_cv_t<T>> && (mapped || std::is_aggregate_v<T>);
  static constexpr bool construction_valid = std::default_initializable<T> && std::move_constructible<T>;

  static_assert(
    std::is_class_v<T> && !std::is_union_v<T> && std::same_as<T, std::remove_cv_t<T>>,
    "Row decoding requires an unqualified struct type");
  static_assert(
    mapped || std::is_aggregate_v<T>,
    "Automatic row decoding requires a simple aggregate; use RowTraits<T> for other types");
  static_assert(construction_valid, "Row decoding requires a default-constructible, movable type");

  static constexpr bool mapping_valid = validate_row_mapping<T, Traits>();

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
        Traits::fields);
    } else {
      return weave_db_vendor::pfr::structure_tie(value);
    }
  }

  using Members = decltype(tie(std::declval<T &>()));
  static constexpr bool members_valid = []<std::size_t... Index>(std::index_sequence<Index...>) {
    return (
      (Codec::template supports<std::remove_cvref_t<std::tuple_element_t<Index, Members>>> &&
        std::is_assignable_v<
          std::tuple_element_t<Index, Members>,
          std::remove_cvref_t<std::tuple_element_t<Index, Members>>> &&
        !std::is_volatile_v<std::remove_reference_t<std::tuple_element_t<Index, Members>>>) &&
      ...);
  }(std::make_index_sequence<std::tuple_size_v<Members>>{});
  static_assert(members_valid, "Struct members must be writable and supported by the driver codec");

  static constexpr auto names = [] {
    if constexpr (!type_valid || !construction_valid || !mapping_valid || !members_valid) {
      return std::array<std::string_view, 0>{};
    } else if constexpr (mapped) {
      return std::apply(
        [](const auto &...entry) {
          return std::array<std::string_view, sizeof...(entry)>{entry.name...};
        },
        Traits::fields);
    } else {
      return weave_db_vendor::pfr::names_as_array<T>();
    }
  }();
};

} // namespace weave::db::detail
