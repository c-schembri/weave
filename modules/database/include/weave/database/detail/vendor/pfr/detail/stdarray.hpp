// Copyright (c) 2023 Denis Mikhailov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef WEAVE_DB_PFR_DETAIL_STDARRAY_HPP
#define WEAVE_DB_PFR_DETAIL_STDARRAY_HPP
#pragma once

#include <weave/database/detail/vendor/pfr/detail/config.hpp>

#include <weave/database/detail/vendor/pfr/detail/sequence_tuple.hpp>

#if !defined(WEAVE_DB_PFR_INTERFACE_UNIT)
#include <array>
#if WEAVE_DB_PFR_CORE_NAME_ENABLED
#   include <string_view>
#endif
#include <utility> // metaprogramming stuff
#endif

namespace weave_db_vendor { namespace pfr { namespace detail {

#if WEAVE_DB_PFR_CORE_NAME_ENABLED
template <class T, std::size_t... I>
constexpr auto make_stdarray_from_tietuple(const T& t, std::index_sequence<I...>) noexcept {
    return std::array<std::string_view, sizeof...(I)>{
        weave_db_vendor::pfr::detail::sequence_tuple::get<I>(t)...
    };
}
#else
template <class T, std::size_t... I>
constexpr auto make_stdarray_from_tietuple(const T&, std::index_sequence<I...>) noexcept {
    return nullptr;
}
#endif

}}} // namespace weave_db_vendor::pfr::detail

#endif // WEAVE_DB_PFR_DETAIL_STDARRAY_HPP

