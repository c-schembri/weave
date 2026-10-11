// Copyright (c) 2016-2025 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef WEAVE_PG_PFR_DETAIL_SIZE_T_HPP
#define WEAVE_PG_PFR_DETAIL_SIZE_T_HPP
#pragma once

#if !defined(WEAVE_PG_PFR_INTERFACE_UNIT)
#include <type_traits>
#include <cstddef>
#endif

namespace weave_pg_vendor { namespace pfr { namespace detail {

///////////////////// General utility stuff
template <std::size_t Index>
using size_t_ = std::integral_constant<std::size_t, Index >;

}}} // namespace weave_pg_vendor::pfr::detail

#endif // WEAVE_PG_PFR_DETAIL_SIZE_T_HPP
