// Copyright (c) 2023 Bela Schaum, X-Ryl669, Denis Mikhailov.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


// Initial implementation by Bela Schaum, https://github.com/schaumb
// The way to make it union and UB free by X-Ryl669, https://github.com/X-Ryl669
//

#ifndef WEAVE_DB_PFR_DETAIL_CORE_NAME_HPP
#define WEAVE_DB_PFR_DETAIL_CORE_NAME_HPP
#pragma once

#include <weave/database/detail/vendor/pfr/detail/config.hpp>

// Each core_name provides `weave_db_vendor::pfr::detail::get_name` and
// `weave_db_vendor::pfr::detail::tie_as_names_tuple` functions.
//
// The whole functional of extracting field's names is build on top of those
// two functions.
#if WEAVE_DB_PFR_CORE_NAME_ENABLED
#include <weave/database/detail/vendor/pfr/detail/core_name20_static.hpp>
#else
#include <weave/database/detail/vendor/pfr/detail/core_name14_disabled.hpp>
#endif

#endif // WEAVE_DB_PFR_DETAIL_CORE_NAME_HPP

