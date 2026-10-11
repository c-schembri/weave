#pragma once

#include <weave/postgres/blocking.hpp>
#include <filesystem>

namespace weave::pg::lo {

/// Large-object descriptor access within an explicit SQL transaction.
enum class Access {
  read,
  write,
  read_write
};

/// Offset origin for large-object seek.
enum class Seek {
  start,
  current,
  end
};

/// Create an object in an explicit transaction; zero requests a server-assigned OID.
/// All async helpers borrow Connection through completion/cancellation; none starts a transaction.
Task<u32> create(Connection &connection, u32 requested = 0);
/// Open an object in the current transaction; descriptor is bound to this session and transaction.
Task<i32> open(Connection &connection, u32 object, Access access = Access::read);
/// Read up to size bytes into owned storage; empty means EOF. Requires a valid transaction descriptor.
Task<std::vector<std::byte>> read(Connection &connection, i32 descriptor, std::size_t size = 65536);
/// Read into borrowed caller storage, retained through completion/cancellation; zero means EOF/empty buffer.
Task<std::size_t> read(Connection &connection, i32 descriptor, std::span<std::byte> buffer);
/// Write borrowed bytes and return the accepted count; partial writes are possible. Retain input until drained.
Task<std::size_t> write(Connection &connection, i32 descriptor, std::span<const std::byte> bytes);
/// Write all borrowed bytes; retain input and session until completion/cancellation drain.
Task<void> write_all(Connection &connection, i32 descriptor, std::span<const std::byte> bytes);
/// Seek a transaction-bound descriptor using a 64-bit offset and return the new position.
Task<i64> seek(Connection &connection, i32 descriptor, i64 offset, Seek origin = Seek::start);
/// Query a transaction-bound descriptor's current 64-bit position through protocol I/O.
Task<i64> tell(Connection &connection, i32 descriptor);
/// Resize the object to a nonnegative 64-bit size within the descriptor's transaction.
Task<void> truncate(Connection &connection, i32 descriptor, i64 size);
/// Close a transaction-bound descriptor; does not delete its object or end the transaction.
Task<void> close(Connection &connection, i32 descriptor);
/// Delete an object by OID within an explicit transaction.
Task<void> remove(Connection &connection, u32 object);

/// Blocking create in an explicit transaction; zero requests a server-assigned OID. No implicit BEGIN.
Result<u32> create(BlockingConnection &connection, u32 requested = 0);
/// Blocking open; returned descriptor belongs to this session and current transaction.
Result<i32> open(BlockingConnection &connection, u32 object, Access access = Access::read);
/// Blocking read into owned bytes; an empty vector means EOF.
Result<std::vector<std::byte>> read(BlockingConnection &connection, i32 descriptor, std::size_t size = 65536);
/// Blocking caller-buffer read; borrows storage only during this call and returns the count.
Result<std::size_t> read(BlockingConnection &connection, i32 descriptor, std::span<std::byte> buffer);
/// Blocking partial write; input is borrowed only until return.
Result<std::size_t> write(BlockingConnection &connection, i32 descriptor, std::span<const std::byte> bytes);
/// Blocking complete write; input is borrowed only until return.
Result<void> write_all(BlockingConnection &connection, i32 descriptor, std::span<const std::byte> bytes);
/// Blocking 64-bit seek on a valid transaction-bound descriptor.
Result<i64> seek(BlockingConnection &connection, i32 descriptor, i64 offset, Seek origin = Seek::start);
/// Blocking 64-bit position query on a valid transaction-bound descriptor.
Result<i64> tell(BlockingConnection &connection, i32 descriptor);
/// Blocking resize to a nonnegative size within the current explicit transaction.
Result<void> truncate(BlockingConnection &connection, i32 descriptor, i64 size);
/// Close the descriptor, not its object or transaction, using blocking protocol I/O.
Result<void> close(BlockingConnection &connection, i32 descriptor);
/// Delete the object in the current explicit transaction using blocking protocol I/O.
Result<void> remove(BlockingConnection &connection, u32 object);
/// Import a caller-machine file into a large object, within an explicit transaction; path is call-scoped.
Result<u32> import_file(BlockingConnection &connection, const std::filesystem::path &path, u32 requested = 0);
/// Export an object to a caller-machine file within an explicit transaction, not a server filesystem path.
Result<void> export_file(BlockingConnection &connection, u32 object, const std::filesystem::path &path);

} // namespace weave::pg::lo
