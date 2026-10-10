#pragma once

#include <weave/postgres.hpp>

namespace fixture {

weave::Task<void> pipeline(weave::pg::Connection &connection);
weave::Task<void> pipeline_terminal(weave::pg::Options options);
weave::Result<void> pipeline(weave::pg::BlockingConnection &connection);

} // namespace fixture
