// Copyright (c) 2016-2025 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef WEAVE_DB_PFR_DETAIL_STDTUPLE_HPP
#define WEAVE_DB_PFR_DETAIL_STDTUPLE_HPP
#pragma once

#include <weave/database/detail/vendor/pfr/detail/config.hpp>

#include <weave/database/detail/vendor/pfr/detail/sequence_tuple.hpp>

#if !defined(WEAVE_DB_PFR_INTERFACE_UNIT)
#include <utility>      // metaprogramming stuff
#include <tuple>
#endif

namespace weave_db_vendor { namespace pfr { namespace detail {

template <class T, std::size_t... I>
constexpr auto make_stdtuple_from_tietuple(const T& t, std::index_sequence<I...>) {
    (void)t;  // workaround for MSVC 14.1 `warning C4100: 't': unreferenced formal parameter`
    return std::make_tuple(
        weave_db_vendor::pfr::detail::sequence_tuple::get<I>(t)...
    );
}

template <class T, std::size_t... I>
constexpr auto make_stdtiedtuple_from_tietuple(const T& t, std::index_sequence<I...>) noexcept {
    (void)t;  // workaround for MSVC 14.1 `warning C4100: 't': unreferenced formal parameter`
    return std::tie(
        weave_db_vendor::pfr::detail::sequence_tuple::get<I>(t)...
    );
}

template <class T, std::size_t... I>
constexpr auto make_conststdtiedtuple_from_tietuple(const T& t, std::index_sequence<I...>) noexcept {
    (void)t;  // workaround for MSVC 14.1 `warning C4100: 't': unreferenced formal parameter`
    return std::tuple<
        std::add_lvalue_reference_t<std::add_const_t<
            std::remove_reference_t<decltype(weave_db_vendor::pfr::detail::sequence_tuple::get<I>(t))>
        >>...
    >(
        weave_db_vendor::pfr::detail::sequence_tuple::get<I>(t)...
    );
}

}}} // namespace weave_db_vendor::pfr::detail

#endif // WEAVE_DB_PFR_DETAIL_STDTUPLE_HPP
