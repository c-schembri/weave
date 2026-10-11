// Copyright (c) 2016-2025 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef WEAVE_PG_PFR_DETAIL_CORE_HPP
#define WEAVE_PG_PFR_DETAIL_CORE_HPP
#pragma once

#include <weave/postgres/detail/vendor/pfr/detail/config.hpp>

// Each core provides `weave_pg_vendor::pfr::detail::tie_as_tuple` and
// `weave_pg_vendor::pfr::detail::for_each_field_dispatcher` functions.
//
// The whole PFR library is build on top of those two functions.
#if WEAVE_PG_PFR_USE_CPP17
#   include <weave/postgres/detail/vendor/pfr/detail/core17.hpp>
#elif WEAVE_PG_PFR_USE_LOOPHOLE
#   include <weave/postgres/detail/vendor/pfr/detail/core14_loophole.hpp>
#else
#   include <weave/postgres/detail/vendor/pfr/detail/core14_classic.hpp>
#endif

#endif // WEAVE_PG_PFR_DETAIL_CORE_HPP
