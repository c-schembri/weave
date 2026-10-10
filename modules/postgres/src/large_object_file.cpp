#include <weave/postgres/large_object.hpp>
#include "large_object.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>

namespace weave::pg::lo {

namespace {

struct FileCloser {
  void operator()(std::FILE *file) const noexcept
  {
    static_cast<void>(std::fclose(file));
  }
};

using File = std::unique_ptr<std::FILE, FileCloser>;

Result<File> open_file(const std::filesystem::path &path, bool output)
{
  if (path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

#ifdef _WIN32
  auto file = _wfopen(path.c_str(), output ? L"wb" : L"rb");
#else
  auto file = std::fopen(path.c_str(), output ? "wb" : "rb");
#endif
  if (!file)
    return std::unexpected(std::error_code{errno, std::generic_category()});

  return File{file};
}

Result<void> transaction(const BlockingConnection &connection)
{
  if (!connection.open())
    return std::unexpected(make_error_code(Error::closed));
  if (connection.transaction() != Transaction::active)
    return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));

  return {};
}

std::error_code file_error() noexcept
{
  return errno ? std::error_code{errno, std::generic_category()} : std::make_error_code(std::errc::io_error);
}

void close_on_failure(BlockingConnection &connection, i32 descriptor)
{
  if (connection.open() && connection.transaction() == Transaction::active)
    static_cast<void>(close(connection, descriptor));
}

} // namespace

Result<u32> import_file(BlockingConnection &connection, const std::filesystem::path &path, u32 requested)
{
  if (auto status = transaction(connection); !status)
    return std::unexpected(status.error());
  auto file = open_file(path, false);
  if (!file)
    return std::unexpected(file.error());

  auto object = create(connection, requested);
  if (!object)
    return std::unexpected(object.error());
  auto descriptor = open(connection, *object, Access::write);
  if (!descriptor)
    return std::unexpected(descriptor.error());

  std::array<std::byte, 65536> buffer;
  for (;;) {
    errno = 0;
    auto size = std::fread(buffer.data(), 1, buffer.size(), file->get());
    auto native_error = errno;
    if (std::ferror(file->get())) {
      auto error = native_error ? std::error_code{native_error, std::generic_category()}
                                : std::make_error_code(std::errc::io_error);
      close_on_failure(connection, *descriptor);
      return std::unexpected(error);
    }
    if (!size)
      break;

    if (auto status = write_all(connection, *descriptor, std::span{buffer}.first(size)); !status) {
      auto error = status.error();
      close_on_failure(connection, *descriptor);
      return std::unexpected(error);
    }
  }

  if (auto status = close(connection, *descriptor); !status)
    return std::unexpected(status.error());

  return *object;
}

Result<void> export_file(BlockingConnection &connection, u32 object, const std::filesystem::path &path)
{
  if (auto status = transaction(connection); !status)
    return std::unexpected(status.error());
  auto file = open_file(path, true);
  if (!file)
    return std::unexpected(file.error());
  auto descriptor = open(connection, object);
  if (!descriptor)
    return std::unexpected(descriptor.error());

  std::array<std::byte, 65536> buffer;
  auto limit = detail::LargeObjectAccess::chunk_limit(detail::LargeObjectAccess::session(connection));
  auto chunk = std::span{buffer}.first(std::min(limit, buffer.size()));
  for (;;) {
    auto size = read(connection, *descriptor, chunk);
    if (!size) {
      auto error = size.error();
      close_on_failure(connection, *descriptor);
      return std::unexpected(error);
    }
    if (!*size)
      break;

    errno = 0;
    if (std::fwrite(buffer.data(), 1, *size, file->get()) != *size) {
      auto error = file_error();
      close_on_failure(connection, *descriptor);
      return std::unexpected(error);
    }
  }

  if (auto status = close(connection, *descriptor); !status)
    return std::unexpected(status.error());
  errno = 0;
  if (std::fclose(file->release()) != 0)
    return std::unexpected(file_error());

  return {};
}

} // namespace weave::pg::lo
