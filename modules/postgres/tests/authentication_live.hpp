#pragma once

#include <weave/postgres.hpp>

namespace fixture {

weave::Task<void> authentication(weave::pg::Options options);
weave::Result<void> authentication_blocking(weave::pg::Options options);

} // namespace fixture
