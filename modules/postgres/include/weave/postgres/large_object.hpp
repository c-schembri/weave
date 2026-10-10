#pragma once

#include <weave/postgres/blocking.hpp>
#include <filesystem>

namespace weave::pg::lo {

enum class Access {
  read,
  write,
  read_write
};

enum class Seek {
  start,
  current,
  end
};

Task<u32> create(Connection &connection, u32 requested = 0);
Task<i32> open(Connection &connection, u32 object, Access access = Access::read);
Task<std::vector<std::byte>> read(Connection &connection, i32 descriptor, std::size_t size = 65536);
Task<std::size_t> read(Connection &connection, i32 descriptor, std::span<std::byte> buffer);
Task<std::size_t> write(Connection &connection, i32 descriptor, std::span<const std::byte> bytes);
Task<void> write_all(Connection &connection, i32 descriptor, std::span<const std::byte> bytes);
Task<i64> seek(Connection &connection, i32 descriptor, i64 offset, Seek origin = Seek::start);
Task<i64> tell(Connection &connection, i32 descriptor);
Task<void> truncate(Connection &connection, i32 descriptor, i64 size);
Task<void> close(Connection &connection, i32 descriptor);
Task<void> remove(Connection &connection, u32 object);

Result<u32> create(BlockingConnection &connection, u32 requested = 0);
Result<i32> open(BlockingConnection &connection, u32 object, Access access = Access::read);
Result<std::vector<std::byte>> read(BlockingConnection &connection, i32 descriptor, std::size_t size = 65536);
Result<std::size_t> read(BlockingConnection &connection, i32 descriptor, std::span<std::byte> buffer);
Result<std::size_t> write(BlockingConnection &connection, i32 descriptor, std::span<const std::byte> bytes);
Result<void> write_all(BlockingConnection &connection, i32 descriptor, std::span<const std::byte> bytes);
Result<i64> seek(BlockingConnection &connection, i32 descriptor, i64 offset, Seek origin = Seek::start);
Result<i64> tell(BlockingConnection &connection, i32 descriptor);
Result<void> truncate(BlockingConnection &connection, i32 descriptor, i64 size);
Result<void> close(BlockingConnection &connection, i32 descriptor);
Result<void> remove(BlockingConnection &connection, u32 object);
Result<u32> import_file(BlockingConnection &connection, const std::filesystem::path &path, u32 requested = 0);
Result<void> export_file(BlockingConnection &connection, u32 object, const std::filesystem::path &path);

} // namespace weave::pg::lo
