#include "large_object.hpp"
#include <algorithm>

namespace weave::pg::lo {

namespace {

using Function = detail::LargeObjectFunction;
using Accessor = detail::LargeObjectAccess;
using Borrow = Accessor::Borrow;

Result<void> valid(const Connection &connection, i32 descriptor = 0) noexcept
{
  if (!connection.open())
    return std::unexpected(make_error_code(Error::closed));
  if (connection.transaction() != Transaction::active)
    return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
  if (descriptor < 0)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  return {};
}

template <class T>
Parameter integer(T value)
{
  auto bits = std::bit_cast<std::make_unsigned_t<T>>(value);
  std::string bytes(sizeof(T), '\0');
  for (std::size_t index = 0; index < sizeof(T); ++index)
    bytes[index] = static_cast<char>((bits >> ((sizeof(T) - index - 1) * 8)) & 255);

  return {std::move(bytes), 0, Format::binary};
}

template <class T>
Task<T> scalar(Connection &connection, Function function, std::vector<Parameter> parameters)
{
  auto oid = co_await Accessor::resolve(connection, function);
  auto value = co_await connection.call_function(oid, std::move(parameters));
  auto result = value.template binary_integer<T>();
  if (!result)
    co_await fail(result.error());

  co_return *result;
}

Task<Value> read_value(Connection &connection, i32 descriptor, std::size_t size)
{
  if (auto status = valid(connection, descriptor); !status)
    co_await fail(status.error());
  if (size > Accessor::chunk_limit(connection))
    co_await fail(Error::resource_limit);

  auto function = co_await Accessor::resolve(connection, Function::read);
  std::vector<Parameter> parameters{integer(descriptor), integer(static_cast<i32>(size))};
  auto value = co_await connection.call_function(function, std::move(parameters));
  if (value.is_null() || value.bytes().size() > size)
    co_await fail(Error::protocol);

  co_return value;
}

Task<u32> create_operation(Connection &connection, u32 requested, [[maybe_unused]] Borrow borrow)
{
  if (auto status = valid(connection); !status)
    co_await fail(status.error());

  std::vector<Parameter> parameters{integer(requested)};
  auto object = co_await scalar<u32>(connection, Function::create, std::move(parameters));
  if (object == 0)
    co_await fail(Error::protocol);

  co_return object;
}

Task<i32> open_operation(Connection &connection, u32 object, Access access, [[maybe_unused]] Borrow borrow)
{
  if (auto status = valid(connection); !status)
    co_await fail(status.error());
  if (object == 0)
    co_await fail(std::errc::invalid_argument);

  i32 mode = 0;
  switch (access) {
  case Access::read:
    mode = 0x40000;
    break;
  case Access::write:
    mode = 0x20000;
    break;
  case Access::read_write:
    mode = 0x60000;
    break;
  default:
    co_await fail(std::errc::invalid_argument);
  }

  std::vector<Parameter> parameters{integer(object), integer(mode)};
  auto descriptor = co_await scalar<i32>(connection, Function::open, std::move(parameters));
  if (descriptor < 0)
    co_await fail(Error::protocol);

  co_return descriptor;
}

Task<std::vector<std::byte>> read_operation(
  Connection &connection,
  i32 descriptor,
  std::size_t size,
  [[maybe_unused]] Borrow borrow)
{
  auto value = co_await read_value(connection, descriptor, size);
  auto bytes = std::as_bytes(std::span{value.bytes().data(), value.bytes().size()});
  co_return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

Task<std::size_t> read_operation(
  Connection &connection,
  i32 descriptor,
  std::span<std::byte> buffer,
  [[maybe_unused]] Borrow borrow)
{
  auto value = co_await read_value(connection, descriptor, buffer.size());
  auto bytes = std::as_bytes(std::span{value.bytes().data(), value.bytes().size()});
  std::copy(bytes.begin(), bytes.end(), buffer.begin());
  co_return bytes.size();
}

Task<std::size_t> write_operation(
  Connection &connection,
  i32 descriptor,
  std::span<const std::byte> bytes,
  [[maybe_unused]] Borrow borrow)
{
  if (auto status = valid(connection, descriptor); !status)
    co_await fail(status.error());
  if (bytes.size() > Accessor::chunk_limit(connection))
    co_await fail(Error::resource_limit);

  std::string data;
  if (!bytes.empty())
    data.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  std::vector<Parameter> parameters{integer(descriptor), {std::move(data), 0, Format::binary}};
  auto size = co_await scalar<i32>(connection, Function::write, std::move(parameters));
  if (size < 0 || static_cast<std::size_t>(size) > bytes.size())
    co_await fail(Error::protocol);

  co_return static_cast<std::size_t>(size);
}

Task<void> write_all_operation(
  Connection &connection,
  i32 descriptor,
  std::span<const std::byte> bytes,
  [[maybe_unused]] Borrow borrow)
{
  co_await cancellation_point();
  if (auto status = valid(connection, descriptor); !status)
    co_await fail(status.error());

  auto limit = std::min(Accessor::chunk_limit(connection), std::size_t{65536});
  while (!bytes.empty()) {
    auto chunk = bytes.first(std::min(bytes.size(), limit));
    auto written = co_await write(connection, descriptor, chunk);
    if (written == 0)
      co_await fail(Error::protocol);

    bytes = bytes.subspan(written);
  }
}

Task<i64> seek_operation(
  Connection &connection,
  i32 descriptor,
  i64 offset,
  Seek origin,
  [[maybe_unused]] Borrow borrow)
{
  if (auto status = valid(connection, descriptor); !status)
    co_await fail(status.error());
  if (origin != Seek::start && origin != Seek::current && origin != Seek::end)
    co_await fail(std::errc::invalid_argument);

  std::vector<Parameter> parameters{integer(descriptor), integer(offset), integer(static_cast<i32>(origin))};
  auto position = co_await scalar<i64>(connection, Function::seek, std::move(parameters));
  if (position < 0)
    co_await fail(Error::protocol);

  co_return position;
}

Task<i64> tell_operation(Connection &connection, i32 descriptor, [[maybe_unused]] Borrow borrow)
{
  if (auto status = valid(connection, descriptor); !status)
    co_await fail(status.error());

  std::vector<Parameter> parameters{integer(descriptor)};
  auto position = co_await scalar<i64>(connection, Function::tell, std::move(parameters));
  if (position < 0)
    co_await fail(Error::protocol);

  co_return position;
}

Task<void> truncate_operation(Connection &connection, i32 descriptor, i64 size, [[maybe_unused]] Borrow borrow)
{
  if (auto status = valid(connection, descriptor); !status)
    co_await fail(status.error());
  if (size < 0)
    co_await fail(std::errc::invalid_argument);

  std::vector<Parameter> parameters{integer(descriptor), integer(size)};
  if (co_await scalar<i32>(connection, Function::truncate, std::move(parameters)) != 0)
    co_await fail(Error::protocol);
}

Task<void> close_operation(Connection &connection, i32 descriptor, [[maybe_unused]] Borrow borrow)
{
  if (auto status = valid(connection, descriptor); !status)
    co_await fail(status.error());

  std::vector<Parameter> parameters{integer(descriptor)};
  if (co_await scalar<i32>(connection, Function::close, std::move(parameters)) != 0)
    co_await fail(Error::protocol);
}

Task<void> remove_operation(Connection &connection, u32 object, [[maybe_unused]] Borrow borrow)
{
  if (auto status = valid(connection); !status)
    co_await fail(status.error());
  if (object == 0)
    co_await fail(std::errc::invalid_argument);

  std::vector<Parameter> parameters{integer(object)};
  if (co_await scalar<i32>(connection, Function::remove, std::move(parameters)) != 1)
    co_await fail(Error::protocol);
}

} // namespace

Task<u32> create(Connection &connection, u32 requested)
{
  return create_operation(connection, requested, Accessor::borrow(connection));
}

Task<i32> open(Connection &connection, u32 object, Access access)
{
  return open_operation(connection, object, access, Accessor::borrow(connection));
}

Task<std::vector<std::byte>> read(Connection &connection, i32 descriptor, std::size_t size)
{
  return read_operation(connection, descriptor, size, Accessor::borrow(connection));
}

Task<std::size_t> read(Connection &connection, i32 descriptor, std::span<std::byte> buffer)
{
  return read_operation(connection, descriptor, buffer, Accessor::borrow(connection));
}

Task<std::size_t> write(Connection &connection, i32 descriptor, std::span<const std::byte> bytes)
{
  return write_operation(connection, descriptor, bytes, Accessor::borrow(connection));
}

Task<void> write_all(Connection &connection, i32 descriptor, std::span<const std::byte> bytes)
{
  return write_all_operation(connection, descriptor, bytes, Accessor::borrow(connection));
}

Task<i64> seek(Connection &connection, i32 descriptor, i64 offset, Seek origin)
{
  return seek_operation(connection, descriptor, offset, origin, Accessor::borrow(connection));
}

Task<i64> tell(Connection &connection, i32 descriptor)
{
  return tell_operation(connection, descriptor, Accessor::borrow(connection));
}

Task<void> truncate(Connection &connection, i32 descriptor, i64 size)
{
  return truncate_operation(connection, descriptor, size, Accessor::borrow(connection));
}

Task<void> close(Connection &connection, i32 descriptor)
{
  return close_operation(connection, descriptor, Accessor::borrow(connection));
}

Task<void> remove(Connection &connection, u32 object)
{
  return remove_operation(connection, object, Accessor::borrow(connection));
}

Result<u32> create(BlockingConnection &connection, u32 requested)
{
  return Accessor::context(connection).run(create(Accessor::session(connection), requested));
}

Result<i32> open(BlockingConnection &connection, u32 object, Access access)
{
  return Accessor::context(connection).run(open(Accessor::session(connection), object, access));
}

Result<std::vector<std::byte>> read(BlockingConnection &connection, i32 descriptor, std::size_t size)
{
  return Accessor::context(connection).run(read(Accessor::session(connection), descriptor, size));
}

Result<std::size_t> read(BlockingConnection &connection, i32 descriptor, std::span<std::byte> buffer)
{
  return Accessor::context(connection).run(read(Accessor::session(connection), descriptor, buffer));
}

Result<std::size_t> write(BlockingConnection &connection, i32 descriptor, std::span<const std::byte> bytes)
{
  return Accessor::context(connection).run(write(Accessor::session(connection), descriptor, bytes));
}

Result<void> write_all(BlockingConnection &connection, i32 descriptor, std::span<const std::byte> bytes)
{
  return Accessor::context(connection).run(write_all(Accessor::session(connection), descriptor, bytes));
}

Result<i64> seek(BlockingConnection &connection, i32 descriptor, i64 offset, Seek origin)
{
  return Accessor::context(connection).run(seek(Accessor::session(connection), descriptor, offset, origin));
}

Result<i64> tell(BlockingConnection &connection, i32 descriptor)
{
  return Accessor::context(connection).run(tell(Accessor::session(connection), descriptor));
}

Result<void> truncate(BlockingConnection &connection, i32 descriptor, i64 size)
{
  return Accessor::context(connection).run(truncate(Accessor::session(connection), descriptor, size));
}

Result<void> close(BlockingConnection &connection, i32 descriptor)
{
  return Accessor::context(connection).run(close(Accessor::session(connection), descriptor));
}

Result<void> remove(BlockingConnection &connection, u32 object)
{
  return Accessor::context(connection).run(remove(Accessor::session(connection), object));
}

} // namespace weave::pg::lo
