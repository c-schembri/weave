#pragma once

#include <weave/postgres.hpp>
#include <filesystem>

namespace fixture {

weave::Task<void> replication(weave::pg::Options options);
weave::Task<void> replication_loaded(weave::pg::Options options, std::filesystem::path directory);
weave::Result<void> replication_blocking(weave::pg::Options options);

} // namespace fixture
