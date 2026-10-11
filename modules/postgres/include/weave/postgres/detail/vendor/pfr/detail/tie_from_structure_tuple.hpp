// Copyright (c) 2018 Adam Butcher, Antony Polukhin
// Copyright (c) 2019-2025 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef WEAVE_PG_PFR_DETAIL_TIE_FROM_STRUCTURE_TUPLE_HPP
#define WEAVE_PG_PFR_DETAIL_TIE_FROM_STRUCTURE_TUPLE_HPP
#pragma once

#include <weave/postgres/detail/vendor/pfr/detail/config.hpp>

#include <weave/postgres/detail/vendor/pfr/detail/core.hpp>

#include <weave/postgres/detail/vendor/pfr/detail/stdtuple.hpp>
#include <weave/postgres/detail/vendor/pfr/tuple_size.hpp>
#include <weave/postgres/detail/vendor/pfr/detail/make_integer_sequence.hpp>

#if !defined(WEAVE_PG_PFR_INTERFACE_UNIT)
#include <tuple>
#endif

namespace weave_pg_vendor { namespace pfr { namespace detail {

/// \brief A `std::tuple` capable of de-structuring assignment used to support
/// a tie of multiple lvalue references to fields of an aggregate T.
///
/// \sa weave_pg_vendor::pfr::tie_from_structure
template <typename... Elements>
struct tie_from_structure_tuple : std::tuple<Elements&...> {
    using base = std::tuple<Elements&...>;
    using base::base;

    template <typename T>
    constexpr tie_from_structure_tuple& operator= (T const& t) {
        base::operator=(
            detail::make_stdtiedtuple_from_tietuple(
                detail::tie_as_tuple(t),
                detail::make_index_sequence<tuple_size_v<T>>()));
        return *this;
    }
};

}}} // namespace weave_pg_vendor::pfr::detail

#endif // WEAVE_PG_PFR_DETAIL_TIE_FROM_STRUCTURE_TUPLE_HPP
