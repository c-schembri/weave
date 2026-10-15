#include <weave/mysql/connection.hpp>
#include <weave/tls.hpp>
#include <weave/tcp.hpp>
#include <weave/timer.hpp>
#include <weave/io/detail/context_access.hpp>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <bit>
#include "pool_access.hpp"
#include "migration_access.hpp"
#include <weave/database/migrations.hpp>

namespace weave::mysql {

namespace {

constexpr std::size_t packet_chunk = 0xffffff;
constexpr std::size_t staging_size = 16384;
constexpr u32 long_password = 1;
constexpr u32 long_flag = 1 << 2;
constexpr u32 connect_with_db = 1 << 3;
constexpr u32 protocol_41 = 1 << 9;
constexpr u32 ssl = 1 << 11;
constexpr u32 transactions = 1 << 13;
constexpr u32 secure_connection = 1 << 15;
constexpr u32 plugin_auth = 1 << 19;
constexpr u32 multi_statements = 1 << 16;
constexpr u32 multi_resultsets = 1 << 17;
constexpr u32 prepared_resultsets = 1 << 18;
constexpr u32 deprecate_eof = 1 << 24;
constexpr u16 more_results = 1 << 3;

struct SecretBytes {
  std::vector<std::byte> bytes;

  SecretBytes() = default;
  SecretBytes(const SecretBytes &) = delete;

  ~SecretBytes()
  {
    if (!bytes.empty())
      OPENSSL_cleanse(bytes.data(), bytes.size());
  }
};

class Reader {
  std::span<const std::byte> data_;
  bool valid_ = true;

public:
  explicit Reader(std::span<const std::byte> data) : data_(data)
  {
  }

  std::span<const std::byte> take(std::size_t count)
  {
    if (count > data_.size()) {
      valid_ = false;
      return {};
    }

    auto result = data_.first(count);
    data_ = data_.subspan(count);
    return result;
  }

  u64 integer(std::size_t count)
  {
    auto bytes = take(count);
    u64 result = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i)
      result |= u64{std::to_integer<u8>(bytes[i])} << (8 * i);
    return result;
  }

  u64 length()
  {
    auto prefix = integer(1);
    if (prefix < 0xfb)
      return prefix;
    if (prefix == 0xfc)
      return integer(2);
    if (prefix == 0xfd)
      return integer(3);
    if (prefix == 0xfe)
      return integer(8);

    valid_ = false;
    return 0;
  }

  std::string_view text(std::size_t size)
  {
    auto bytes = take(size);
    if (bytes.empty())
      return {};
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
  }

  std::string_view text()
  {
    auto size = length();
    if (size > data_.size()) {
      valid_ = false;
      return {};
    }
    return text(static_cast<std::size_t>(size));
  }

  std::string_view terminated()
  {
    auto end = std::find(data_.begin(), data_.end(), std::byte{0});
    if (end == data_.end()) {
      valid_ = false;
      return {};
    }
    auto result = text(static_cast<std::size_t>(end - data_.begin()));
    take(1);
    return result;
  }

  std::size_t remaining() const noexcept
  {
    return data_.size();
  }

  bool valid() const noexcept
  {
    return valid_;
  }

  bool done() const noexcept
  {
    return valid_ && data_.empty();
  }
};

void integer(std::vector<std::byte> &bytes, u64 value, std::size_t size)
{
  for (std::size_t i = 0; i < size; ++i)
    bytes.push_back(std::byte{static_cast<u8>(value >> (8 * i))});
}

void terminated(std::vector<std::byte> &bytes, std::string_view text)
{
  auto data = std::as_bytes(std::span{text});
  bytes.insert(bytes.end(), data.begin(), data.end());
  bytes.push_back(std::byte{0});
}

bool contains_nul(std::string_view text)
{
  return text.find('\0') != std::string_view::npos;
}

struct Greeting {
  u32 capabilities = 0;
  std::array<std::byte, 20> nonce{};
  std::string plugin;
};

Result<Greeting> greeting(std::span<const std::byte> bytes)
{
  Reader reader{bytes};
  if (reader.integer(1) != 10)
    return std::unexpected(make_error_code(ClientError::protocol));

  reader.terminated();
  reader.integer(4);
  auto first = reader.take(8);
  auto filler = reader.integer(1);
  auto low = reader.integer(2);
  reader.integer(1);
  reader.integer(2);
  auto high = reader.integer(2);
  auto nonce_size = reader.integer(1);
  reader.take(10);

  Greeting result;
  result.capabilities = static_cast<u32>(low | (high << 16));
  const auto required = protocol_41 | secure_connection | plugin_auth;
  if (!reader.valid() || filler != 0 || (result.capabilities & required) != required || nonce_size != 21)
    return std::unexpected(make_error_code(ClientError::protocol));

  auto second = reader.take(12);
  auto terminator = reader.integer(1);
  result.plugin = reader.terminated();
  if (!reader.done() || terminator != 0 || result.plugin.empty())
    return std::unexpected(make_error_code(ClientError::protocol));

  std::copy(first.begin(), first.end(), result.nonce.begin());
  std::copy(second.begin(), second.end(), result.nonce.begin() + 8);
  return result;
}

Result<Failure> failure(std::span<const std::byte> bytes, bool negotiated = true)
{
  Reader reader{bytes};
  if (reader.integer(1) != 0xff)
    return std::unexpected(make_error_code(ClientError::protocol));

  Failure result;
  result.number = static_cast<u16>(reader.integer(2));
  if (negotiated) {
    if (reader.integer(1) != '#')
      return std::unexpected(make_error_code(ClientError::protocol));
    result.sqlstate = reader.text(5);
    const bool valid_state = std::ranges::all_of(result.sqlstate, [](char value) {
      return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z');
    });
    if (!valid_state)
      return std::unexpected(make_error_code(ClientError::protocol));
  }
  result.message = reader.text(reader.remaining());
  if (!reader.done() || result.number == 0)
    return std::unexpected(make_error_code(ClientError::protocol));
  return result;
}

Result<void> ok(std::span<const std::byte> bytes, ResultSet &result, u8 header = 0)
{
  Reader reader{bytes};
  if (reader.integer(1) != header)
    return std::unexpected(make_error_code(ClientError::protocol));

  result.affected_rows = reader.length();
  result.last_insert_id = reader.length();
  result.status = static_cast<u16>(reader.integer(2));
  result.warnings = static_cast<u16>(reader.integer(2));
  result.info = reader.text(reader.remaining());
  if (!reader.done())
    return std::unexpected(make_error_code(ClientError::protocol));
  return {};
}

Result<ResultSet> ok(std::span<const std::byte> bytes)
{
  ResultSet result;
  if (auto parsed = ok(bytes, result); !parsed)
    return std::unexpected(parsed.error());
  return result;
}

Result<void> eof(std::span<const std::byte> bytes, ResultSet &result)
{
  Reader reader{bytes};
  if (reader.integer(1) != 0xfe)
    return std::unexpected(make_error_code(ClientError::protocol));
  result.warnings = static_cast<u16>(reader.integer(2));
  result.status = static_cast<u16>(reader.integer(2));
  if (!reader.done())
    return std::unexpected(make_error_code(ClientError::protocol));
  return {};
}

Result<void> column(Column &result, std::span<const std::byte> bytes)
{
  Reader reader{bytes};
  if (reader.text() != "def")
    return std::unexpected(make_error_code(ClientError::protocol));

  result.schema = reader.text();
  result.table = reader.text();
  result.original_table = reader.text();
  result.name = reader.text();
  result.original_name = reader.text();
  if (reader.length() != 12)
    return std::unexpected(make_error_code(ClientError::protocol));

  result.character_set = static_cast<u16>(reader.integer(2));
  result.length = static_cast<u32>(reader.integer(4));
  result.type = static_cast<u8>(reader.integer(1));
  result.flags = static_cast<u16>(reader.integer(2));
  result.decimals = static_cast<u8>(reader.integer(1));
  auto filler = reader.integer(2);
  if (!reader.done() || filler != 0)
    return std::unexpected(make_error_code(ClientError::protocol));
  return {};
}

bool same_column(const Column &left, const Column &right)
{
  return left.schema == right.schema && left.table == right.table && left.original_table == right.original_table &&
    left.name == right.name && left.original_name == right.original_name && left.character_set == right.character_set &&
    left.length == right.length && left.type == right.type && left.flags == right.flags &&
    left.decimals == right.decimals;
}

Result<void> append_column(db::detail::ResultList<Column> &columns, std::span<const std::byte> bytes)
{
  auto &result = columns.emplace_back();
  if (auto parsed = column(result, bytes); !parsed) {
    columns.pop_back();
    return std::unexpected(parsed.error());
  }
  return {};
}

void length(std::vector<std::byte> &bytes, u64 size)
{
  if (size < 251)
    integer(bytes, size, 1);
  else if (size <= 65535) {
    integer(bytes, 252, 1);
    integer(bytes, size, 2);
  } else if (size <= packet_chunk) {
    integer(bytes, 253, 1);
    integer(bytes, size, 3);
  } else {
    integer(bytes, 254, 1);
    integer(bytes, size, 8);
  }
}

struct ExecutePacket {
  std::vector<std::byte> header;
  std::vector<std::byte> values;

  void reset() noexcept
  {
    const std::array buffers{&header, &values};
    for (auto *buffer : buffers) {
      if (!buffer->empty())
        OPENSSL_cleanse(buffer->data(), buffer->size());
      buffer->clear();
      if (buffer->capacity() > 65536)
        std::vector<std::byte>{}.swap(*buffer);
    }
  }

  ~ExecutePacket()
  {
    reset();
  }

  struct Call {
    ExecutePacket &packet;

    ~Call()
    {
      packet.reset();
    }
  };
};

Result<void> execute_packet(
  ExecutePacket &packet,
  u32 id,
  std::span<const ParameterView> parameters,
  std::span<const std::size_t> long_sizes,
  std::size_t maximum)
{
  if (parameters.size() != long_sizes.size())
    return std::unexpected(make_error_code(ClientError::invalid_options));
  auto &header = packet.header;
  auto &values = packet.values;
  const auto metadata = 11 + (parameters.size() + 7) / 8 + 2 * parameters.size();
  if (metadata > maximum)
    return std::unexpected(make_error_code(ClientError::resource_limit));
  header.reserve(metadata);
  integer(header, 23, 1);
  integer(header, id, 4);
  integer(header, 0, 1);
  integer(header, 1, 4);
  if (parameters.empty())
    return {};

  const auto bitmap = header.size();
  header.resize(header.size() + (parameters.size() + 7) / 8, std::byte{0});
  integer(header, 1, 1);
  values.reserve(parameters.size() * 8);
  std::optional<std::error_code> error;

  for (std::size_t index = 0; index < parameters.size(); ++index) {
    std::visit(
      [&]<class T>(const T &input) {
        u8 type = 6;
        u8 flags = 0;
        std::array<std::byte, 13> scalar{};
        std::size_t scalar_size = 0;
        auto append_scalar = [&](u64 value, std::size_t size) {
          for (std::size_t position = 0; position < size; ++position)
            scalar[scalar_size++] = std::byte{static_cast<u8>(value >> (8 * position))};
        };
        std::span<const std::byte> data;
        bool variable = false;
        if constexpr (std::same_as<T, std::monostate>) {
          header[bitmap + index / 8] |= std::byte{static_cast<u8>(1 << (index % 8))};
        } else if constexpr (std::same_as<T, bool>) {
          type = 1;
          append_scalar(input, 1);
        } else if constexpr (std::same_as<T, i64> || std::same_as<T, u64>) {
          type = 8;
          flags = std::same_as<T, u64> ? 128 : 0;
          append_scalar(static_cast<u64>(input), 8);
        } else if constexpr (std::same_as<T, f32> || std::same_as<T, f64>) {
          if (!std::isfinite(input))
            error = make_error_code(ClientError::invalid_value);
          if constexpr (std::same_as<T, f32>) {
            type = 4;
            append_scalar(std::bit_cast<u32>(input), 4);
          } else {
            type = 5;
            append_scalar(std::bit_cast<u64>(input), 8);
          }
        } else if constexpr (std::same_as<T, std::string_view>) {
          type = 253;
          data = std::as_bytes(std::span{input});
          variable = true;
        } else if constexpr (std::same_as<T, std::span<const std::byte>>) {
          type = 252;
          data = input;
          variable = true;
        } else if constexpr (std::same_as<T, DecimalView>) {
          type = 246;
          data = std::as_bytes(std::span{input.value});
          variable = true;
          std::size_t digits = 0;
          bool dot = false;
          for (std::size_t position = 0; position < input.value.size(); ++position) {
            const auto character = input.value[position];
            if (character >= '0' && character <= '9')
              ++digits;
            else if (character == '.' && !dot)
              dot = true;
            else if (!(position == 0 && (character == '-' || character == '+')))
              error = make_error_code(ClientError::invalid_value);
          }
          if (!digits)
            error = make_error_code(ClientError::invalid_value);
        } else if constexpr (std::same_as<T, Date> || std::same_as<T, DateTime>) {
          const auto date = [&]() {
            if constexpr (std::same_as<T, Date>)
              return input;
            else
              return input.date;
          }();
          if (date.year > 9999 || date.month > 12 || date.day > 31)
            error = make_error_code(ClientError::invalid_value);
          type = std::same_as<T, Date> ? 10 : 12;
          append_scalar(std::same_as<T, Date> ? 4 : 11, 1);
          append_scalar(date.year, 2);
          append_scalar(date.month, 1);
          append_scalar(date.day, 1);
          if constexpr (std::same_as<T, DateTime>) {
            if (input.hour > 23 || input.minute > 59 || input.second > 59 || input.microsecond > 999999)
              error = make_error_code(ClientError::invalid_value);
            append_scalar(input.hour, 1);
            append_scalar(input.minute, 1);
            append_scalar(input.second, 1);
            append_scalar(input.microsecond, 4);
          }
        } else if constexpr (std::same_as<T, Time>) {
          if (input.days > 34 || input.days * 24 + input.hour > 838 || input.hour > 23 || input.minute > 59 ||
            input.second > 59 || input.microsecond > 999999)
            error = make_error_code(ClientError::invalid_value);
          type = 11;
          append_scalar(12, 1);
          append_scalar(input.negative, 1);
          append_scalar(input.days, 4);
          append_scalar(input.hour, 1);
          append_scalar(input.minute, 1);
          append_scalar(input.second, 1);
          append_scalar(input.microsecond, 4);
        }

        integer(header, type, 1);
        integer(header, flags, 1);
        if (long_sizes[index]) {
          if (type != 253 && type != 252)
            error = make_error_code(ClientError::invalid_value);
          return;
        }
        if (variable) {
          if (data.size() > maximum || values.size() > maximum - data.size()) {
            error = make_error_code(ClientError::resource_limit);
            return;
          }
          length(values, data.size());
          values.insert(values.end(), data.begin(), data.end());
        } else {
          values.insert(values.end(), scalar.begin(), scalar.begin() + scalar_size);
        }
      },
      parameters[index].value);
    if (error)
      return std::unexpected(*error);
    if (header.size() > maximum || values.size() > maximum - header.size())
      return std::unexpected(make_error_code(ClientError::resource_limit));
  }
  header.insert(header.end(), values.begin(), values.end());
  return {};
}

Task<ResultSet> one_result(Task<Results> operation)
{
  auto results = co_await std::move(operation);
  if (results.size() != 1)
    co_await fail(make_error_code(ClientError::multiple_results));
  co_return std::move(results[0]);
}

Result<std::vector<std::byte>> proof(
  std::string_view plugin,
  std::string_view password,
  std::span<const std::byte> nonce)
{
  const bool caching = plugin == "caching_sha2_password";
  if (!caching && plugin != "mysql_native_password")
    return std::unexpected(make_error_code(ClientError::unsupported_authentication));
  if (nonce.size() != 20)
    return std::unexpected(make_error_code(ClientError::protocol));
  if (password.empty())
    return std::vector<std::byte>{};

  struct Hashes {
    std::array<unsigned char, 32> first{};
    std::array<unsigned char, 32> second{};
    std::array<unsigned char, 32> mask{};

    ~Hashes()
    {
      OPENSSL_cleanse(this, sizeof(*this));
    }
  } hashes;

  const auto *algorithm = caching ? EVP_sha256() : EVP_sha1();
  unsigned size = 0;
  auto *context = EVP_MD_CTX_new();
  if (!context)
    return std::unexpected(std::make_error_code(std::errc::not_enough_memory));

  auto digest =
    [&](const void *first, std::size_t first_size, const void *second, std::size_t second_size, unsigned char *output) {
      return EVP_DigestInit_ex(context, algorithm, nullptr) == 1 && EVP_DigestUpdate(context, first, first_size) == 1 &&
        EVP_DigestUpdate(context, second, second_size) == 1 && EVP_DigestFinal_ex(context, output, &size) == 1;
    };

  bool valid = digest(password.data(), password.size(), "", 0, hashes.first.data());
  valid = valid && digest(hashes.first.data(), size, "", 0, hashes.second.data());
  if (caching)
    valid = valid && digest(hashes.second.data(), size, nonce.data(), nonce.size(), hashes.mask.data());
  else
    valid = valid && digest(nonce.data(), nonce.size(), hashes.second.data(), size, hashes.mask.data());
  EVP_MD_CTX_free(context);

  if (!valid)
    return std::unexpected(std::make_error_code(std::errc::io_error));

  std::vector<std::byte> result(size);
  for (std::size_t i = 0; i < size; ++i)
    result[i] = std::byte{static_cast<u8>(hashes.first[i] ^ hashes.mask[i])};
  return result;
}

} // namespace

struct Password::Storage {
  std::vector<char> bytes;

  explicit Storage(std::string_view password) : bytes(password.begin(), password.end())
  {
  }

  ~Storage()
  {
    if (!bytes.empty())
      OPENSSL_cleanse(bytes.data(), bytes.size());
  }
};

Password::Password(std::string_view password) : storage_(std::make_shared<Storage>(password))
{
}

Password::Password(const char *password) : Password(password ? std::string_view{password} : std::string_view{})
{
}

std::string_view Password::view() const noexcept
{
  if (!storage_ || storage_->bytes.empty())
    return {};
  return {storage_->bytes.data(), storage_->bytes.size()};
}

struct Connection::Impl {
  Context *context = nullptr;
  std::optional<TcpStream> plain;
  std::optional<TlsStream<TcpStream>> secure;
  std::atomic<std::size_t> borrowers{0};
  bool busy = false;
  bool closed = false;
  bool at_boundary = true;
  bool pooled = false;
  bool migrating = false;
  bool modern_eof = false;
  u8 sequence = 0;
  u16 status = 2;
  std::vector<u32> statements;
  std::vector<u32> pending_close;
  Options options;
  std::optional<Failure> diagnostic;
  std::vector<std::byte> input;
  std::size_t input_begin = 0;
  std::size_t input_end = 0;
  std::vector<std::byte> packet;
  std::vector<std::byte> send_buffer;
  std::shared_ptr<const db::detail::ResultList<Column>> cached_schema;

  explicit Impl(Options configuration) : options(std::move(configuration))
  {
  }

  Result<void> retire() noexcept
  {
    closed = true;
    if (secure)
      return secure->close();
    if (plain)
      return plain->close();
    return {};
  }

  struct Guard {
    Impl &impl;
    bool completed = false;
    bool keep_busy = false;

    ~Guard()
    {
      const bool recovered_sql = impl.at_boundary && impl.diagnostic.has_value();
      if (!completed && !recovered_sql)
        static_cast<void>(impl.retire());
      if (!keep_busy)
        impl.busy = false;
    }
  };

  Task<std::size_t> receive_into(std::span<std::byte> bytes)
  {
    if (secure)
      return secure->read(bytes);
    return plain->read(bytes);
  }

  Task<std::size_t> fill()
  {
    if (input.empty())
      input.resize(staging_size);
    input_begin = 0;
    return receive_into(input);
  }

  Task<void> read(std::span<std::byte> bytes)
  {
    while (!bytes.empty()) {
      if (input_begin == input_end && bytes.size() >= 16384) {
        const auto received = co_await receive_into(bytes);
        if (!received)
          co_await fail(std::errc::connection_reset);
        bytes = bytes.subspan(received);
        continue;
      }
      if (input_begin == input_end) {
        input_end = co_await fill();
        if (!input_end)
          co_await fail(std::errc::connection_reset);
      }
      const auto count = (std::min)(bytes.size(), input_end - input_begin);
      std::memcpy(bytes.data(), input.data() + input_begin, count);
      input_begin += count;
      bytes = bytes.subspan(count);
    }
  }

  Task<void> write(std::span<const std::byte> bytes)
  {
    if (secure)
      return secure->write_all(bytes);
    return plain->write_all(bytes);
  }

  // Packet views expire at the next receive. Retained rows, metadata and diagnostics still own their bytes.
  Task<std::span<const std::byte>> receive_slow(
    std::size_t limit,
    Row *destination = nullptr,
    std::size_t prefix = 0,
    const db::detail::ResultArena *storage = nullptr)
  {
    weave::detail::ContextAccess::check_execution(*context);
    co_await cancellation_point();
    const auto maximum = limit ? (std::min)(limit, options.max_packet_bytes) : options.max_packet_bytes;
    if (packet.capacity() > 65536)
      std::vector<std::byte>{}.swap(packet);
    packet.clear();
    std::size_t assembled = 0;
    for (;;) {
      std::array<std::byte, 4> header{};
      if (input_begin == input_end) {
        input_end = co_await fill();
        if (!input_end)
          co_await fail(std::errc::connection_reset);
      }
      if (input_end - input_begin >= header.size()) {
        std::memcpy(header.data(), input.data() + input_begin, header.size());
        input_begin += header.size();
      } else {
        co_await read(header);
      }
      Reader reader{header};
      auto count = static_cast<std::size_t>(reader.integer(3));
      if (reader.integer(1) != sequence++)
        co_await fail(make_error_code(ClientError::protocol));
      if (count > maximum - assembled || prefix > std::numeric_limits<std::size_t>::max() - assembled - count)
        co_await fail(make_error_code(ClientError::resource_limit));

      if (!assembled && count < packet_chunk && count <= input_end - input_begin) {
        auto bytes = std::span<const std::byte>{input}.subspan(input_begin, count);
        input_begin += count;
        co_return bytes;
      }

      std::span<std::byte> output;
      if (destination) {
        const auto size = prefix + assembled + count;
        if (!assembled && storage && storage->identity() && size > destination->storage_.capacity())
          destination->storage_ = db::detail::ResultText{db::detail::ResultAllocator<char>{*storage}};
        destination->storage_.resize(size);
        output = std::as_writable_bytes(std::span{destination->storage_.data(), size}).subspan(prefix + assembled);
      } else {
        packet.resize(assembled + count);
        output = std::span{packet}.subspan(assembled);
      }
      if (count)
        co_await read(output);
      assembled += count;
      if (count < packet_chunk) {
        if (destination)
          co_return std::as_bytes(std::span{destination->storage_.data(), destination->storage_.size()})
            .subspan(prefix);
        co_return std::span<const std::byte>{packet};
      }
    }
  }

  Result<std::optional<std::span<const std::byte>>> receive_cached(const CancelToken &cancel, std::size_t limit = 0)
  {
    weave::detail::ContextAccess::check_execution(*context);
    if (cancel.stop_requested())
      return std::unexpected(std::make_error_code(std::errc::operation_canceled));

    const auto available = input_end - input_begin;
    if (available < 4)
      return std::nullopt;
    Reader header{std::span{input}.subspan(input_begin, 4)};
    const auto count = static_cast<std::size_t>(header.integer(3));
    const auto packet_sequence = header.integer(1);
    const auto maximum = limit ? (std::min)(limit, options.max_packet_bytes) : options.max_packet_bytes;
    if (packet_sequence != sequence)
      return std::unexpected(make_error_code(ClientError::protocol));
    if (count > maximum)
      return std::unexpected(make_error_code(ClientError::resource_limit));
    if (count == packet_chunk || count > available - 4)
      return std::nullopt;

    if (packet.capacity() > 65536)
      std::vector<std::byte>{}.swap(packet);
    packet.clear();
    ++sequence;
    input_begin += 4;
    auto bytes = std::span<const std::byte>{input}.subspan(input_begin, count);
    input_begin += count;
    return bytes;
  }

  struct PacketAwaiter {
    Impl &impl;
    std::size_t limit;
    std::span<const std::byte> bytes;
    std::optional<weave::detail::TaskAwaiter<std::span<const std::byte>, false>> operation;
    const CancelToken *cancel = nullptr;

    bool await_ready()
    {
      operation.reset();
      if (cancel) {
        auto cached = impl.receive_cached(*cancel, limit);
        if (cached && *cached) {
          bytes = **cached;
          return true;
        }
      }
      return false;
    }

    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent)
    {
      static_assert(std::derived_from<P, weave::detail::PromiseBase>);
      auto cached = impl.receive_cached(parent.promise().cancellation, limit);
      if (!cached)
        return weave::detail::propagate(parent.promise(), cached.error());
      if (*cached) {
        bytes = **cached;
        return parent;
      }

      // Use the existing Task owner/failure chain when actual I/O or continuation assembly is needed.
      operation.emplace(impl.receive_slow(limit));
      return operation->await_suspend(parent);
    }

    std::span<const std::byte> await_resume()
    {
      return operation ? operation->await_resume() : bytes;
    }
  };

  PacketAwaiter receive(std::size_t limit = 0)
  {
    return {*this, limit, {}, {}, nullptr};
  }

  PacketAwaiter receive(const CancelToken &cancel, std::size_t limit = 0)
  {
    return {*this, limit, {}, {}, &cancel};
  }

  struct StagedPacket {
    std::span<std::byte> bytes;
    std::size_t prefix_count;
    std::size_t staged_prefix;
  };

  StagedPacket stage_packet(
    std::span<const std::byte> prefix,
    std::span<const std::byte> body,
    std::size_t offset,
    std::size_t count)
  {
    if (send_buffer.empty())
      send_buffer.resize(staging_size);

    std::array<std::byte, 4> header{
      std::byte{static_cast<u8>(count)},
      std::byte{static_cast<u8>(count >> 8)},
      std::byte{static_cast<u8>(count >> 16)},
      std::byte{sequence++}};
    const auto prefix_count = offset < prefix.size() ? (std::min)(count, prefix.size() - offset) : 0;
    const auto staged = (std::min)(count, send_buffer.size() - header.size());
    const auto staged_prefix = (std::min)(prefix_count, staged);
    std::memcpy(send_buffer.data(), header.data(), header.size());
    if (staged_prefix)
      std::memcpy(send_buffer.data() + header.size(), prefix.data() + offset, staged_prefix);
    if (staged > staged_prefix) {
      const auto body_offset = offset + staged_prefix - prefix.size();
      std::memcpy(
        send_buffer.data() + header.size() + staged_prefix,
        body.data() + body_offset,
        staged - staged_prefix);
    }
    return {std::span{send_buffer}.first(header.size() + staged), prefix_count, staged_prefix};
  }

  Task<void> send_slow(std::span<const std::byte> prefix, std::span<const std::byte> body)
  {
    if (prefix.size() > options.max_packet_bytes || body.size() > options.max_packet_bytes - prefix.size())
      co_await fail(make_error_code(ClientError::resource_limit));
    const auto total = prefix.size() + body.size();
    std::size_t offset = 0;
    for (;;) {
      const auto count = (std::min)(total - offset, packet_chunk);
      auto staged = stage_packet(prefix, body, offset, count);

      // Authentication can use this staging too. Wipe copied secrets after the write drains, including failure.
      struct Cleanse {
        std::span<std::byte> bytes;

        ~Cleanse()
        {
          OPENSSL_cleanse(bytes.data(), bytes.size());
        }
      } cleanse{staged.bytes};

      co_await write(cleanse.bytes);
      if (staged.prefix_count > staged.staged_prefix)
        co_await write(prefix.subspan(offset + staged.staged_prefix, staged.prefix_count - staged.staged_prefix));
      const auto sent = (std::max)(staged.bytes.size() - 4, staged.prefix_count);
      if (count > sent)
        co_await write(body.subspan(offset + sent - prefix.size(), count - sent));
      offset += count;
      if (count < packet_chunk)
        co_return;
    }
  }

  struct SendAwaiter {
    Impl &impl;
    std::span<const std::byte> prefix;
    std::span<const std::byte> body;
    std::optional<weave::detail::TaskAwaiter<void, false>> operation;
    std::size_t staged_bytes = 0;

    ~SendAwaiter()
    {
      // The transport child must release its staged authentication bytes before they are wiped.
      operation.reset();
      if (staged_bytes)
        OPENSSL_cleanse(impl.send_buffer.data(), staged_bytes);
    }

    bool await_ready() const noexcept
    {
      return false;
    }

    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent)
    {
      static_assert(std::derived_from<P, weave::detail::PromiseBase>);
      if (parent.promise().cancellation.stop_requested())
        return weave::detail::propagate(parent.promise(), std::make_error_code(std::errc::operation_canceled));
      if (prefix.size() > impl.options.max_packet_bytes || body.size() > impl.options.max_packet_bytes - prefix.size())
        return weave::detail::propagate(parent.promise(), make_error_code(ClientError::resource_limit));

      const auto total = prefix.size() + body.size();
      const auto capacity = impl.send_buffer.empty() ? staging_size : impl.send_buffer.size();
      if (total < packet_chunk && capacity >= 4 && total <= capacity - 4) {
        auto staged = impl.stage_packet(prefix, body, 0, total);
        staged_bytes = staged.bytes.size();
        operation.emplace(impl.write(staged.bytes));
      } else {
        operation.emplace(impl.send_slow(prefix, body));
      }
      return operation->await_suspend(parent);
    }

    void await_resume()
    {
      operation->await_resume();
    }
  };

  SendAwaiter send(std::span<const std::byte> prefix, std::span<const std::byte> body = {})
  {
    return {*this, prefix, body, {}, 0};
  }

  Result<void> charge(std::size_t &used, std::size_t bytes) const
  {
    if (bytes > options.max_result_bytes - used)
      return std::unexpected(make_error_code(ClientError::resource_limit));
    used += bytes;
    return {};
  }

  Result<void> inspect_error(std::span<const std::byte> bytes)
  {
    if (bytes.empty() || bytes[0] != std::byte{0xff})
      return {};

    auto parsed = failure(bytes);
    if (!parsed)
      return std::unexpected(parsed.error());
    diagnostic = std::move(*parsed);
    at_boundary = true;
    return std::unexpected(server_error(diagnostic->number));
  }

  Result<void> ready(bool migration_owner = false) const
  {
    if (closed)
      return std::unexpected(make_error_code(ClientError::closed));
    if (busy || (migrating && !migration_owner))
      return std::unexpected(make_error_code(ClientError::busy));
    return {};
  }

  Task<void> close_statement(u32 id)
  {
    sequence = 0;
    at_boundary = false;
    std::vector<std::byte> bytes;
    integer(bytes, 25, 1);
    integer(bytes, id, 4);
    co_await send(bytes);
    std::erase(statements, id);
    at_boundary = true;
  }

  Task<void> flush_closes()
  {
    while (!pending_close.empty()) {
      co_await close_statement(pending_close.back());
      pending_close.pop_back();
    }
  }

  struct Response {
    ResultSet metadata;
    std::shared_ptr<const db::detail::ResultList<Column>> schema;
    db::detail::ResultArena storage;
    std::size_t row_count = 0;
    bool rows_done = false;
  };

  static std::size_t row_prefix(const Response &response) noexcept
  {
    return (response.metadata.columns.size() - 1) * sizeof(Row::Cell);
  }

  bool end_packet(u8 first, std::size_t size) const noexcept
  {
    return first == 0xfe && size < (modern_eof ? packet_chunk : 9);
  }

  struct ResponseReader {
    enum class Stage {
      header,
      columns,
      metadata_eof,
      done
    };

    Format format;
    Stage stage = Stage::header;
    std::size_t remaining_columns = 0;
  };

  Result<void> begin_response_packet(
    std::span<const std::byte> bytes,
    std::size_t &used,
    Response &response,
    ResponseReader &reader)
  {
    auto &output = response.metadata;
    switch (reader.stage) {
    case ResponseReader::Stage::header: {
      if (auto charged = charge(used, bytes.size()); !charged)
        return std::unexpected(charged.error());
      if (bytes.empty())
        return std::unexpected(make_error_code(ClientError::protocol));
      if (bytes[0] == std::byte{0}) {
        if (auto parsed = ok(bytes, output); !parsed)
          return std::unexpected(parsed.error());
        status = output.status;
        at_boundary = !(status & more_results);
        output.format = reader.format;
        response.rows_done = true;
        reader.stage = ResponseReader::Stage::done;
        return {};
      }
      if (auto inspected = inspect_error(bytes); !inspected)
        return std::unexpected(inspected.error());
      // LOCAL INFILE is never negotiated or serviced; do not expose local files to the peer.
      if (bytes[0] == std::byte{0xfb})
        return std::unexpected(make_error_code(ClientError::protocol));

      Reader count_reader{bytes};
      auto count = count_reader.length();
      if (!count_reader.done() || count == 0)
        return std::unexpected(make_error_code(ClientError::protocol));
      if (count > options.max_columns)
        return std::unexpected(make_error_code(ClientError::resource_limit));

      output.format = reader.format;
      if (auto charged = charge(used, 2 * static_cast<std::size_t>(count) * sizeof(Column)); !charged)
        return std::unexpected(charged.error());
      reader.remaining_columns = static_cast<std::size_t>(count);
      output.columns.reserve(reader.remaining_columns);
      reader.stage = ResponseReader::Stage::columns;
      return {};
    }
    case ResponseReader::Stage::columns:
      if (auto charged = charge(used, 2 * bytes.size()); !charged)
        return std::unexpected(charged.error());
      if (auto inspected = inspect_error(bytes); !inspected)
        return std::unexpected(inspected.error());
      if (auto parsed = append_column(output.columns, bytes); !parsed)
        return std::unexpected(parsed.error());
      if (--reader.remaining_columns)
        return {};
      if (!modern_eof) {
        reader.stage = ResponseReader::Stage::metadata_eof;
        return {};
      }
      break;
    case ResponseReader::Stage::metadata_eof:
      if (auto charged = charge(used, bytes.size()); !charged)
        return std::unexpected(charged.error());
      if (auto inspected = inspect_error(bytes); !inspected)
        return std::unexpected(inspected.error());
      if (auto parsed = eof(bytes, output); !parsed)
        return std::unexpected(parsed.error());
      break;
    case ResponseReader::Stage::done:
      return std::unexpected(make_error_code(ClientError::protocol));
    }

    // Every retained Row owns this schema, independent of ResultSet/Connection lifetimes.
    const bool same_schema = cached_schema && cached_schema->size() == output.columns.size() &&
      std::equal(output.columns.begin(), output.columns.end(), cached_schema->begin(), same_column);
    if (!same_schema)
      cached_schema = std::make_shared<const db::detail::ResultList<Column>>(output.columns);
    response.schema = cached_schema;
    reader.stage = ResponseReader::Stage::done;
    return {};
  }

  Task<void> begin_response(Format format, std::size_t &used, Response &response)
  {
    const auto cancel = co_await weave::detail::GetCancellation{};
    auto packet = receive(cancel);
    ResponseReader reader{format};
    while (reader.stage != ResponseReader::Stage::done) {
      auto bytes = co_await packet;
      if (auto parsed = begin_response_packet(bytes, used, response, reader); !parsed)
        co_await fail(parsed.error());
    }
  }

  Result<bool> decode_row(std::span<const std::byte> bytes, Response &response, std::size_t &used, Row &row)
  {
    if (response.rows_done)
      return false;
    if (auto charged = charge(used, bytes.size()); !charged)
      return std::unexpected(charged.error());
    if (bytes.empty())
      return std::unexpected(make_error_code(ClientError::protocol));
    if (end_packet(std::to_integer<u8>(bytes[0]), bytes.size())) {
      auto &metadata = response.metadata;
      auto parsed = modern_eof ? ok(bytes, metadata, 0xfe) : eof(bytes, metadata);
      if (!parsed)
        return std::unexpected(parsed.error());
      if (metadata.affected_rows || metadata.last_insert_id)
        return std::unexpected(make_error_code(ClientError::protocol));
      status = response.metadata.status;
      at_boundary = !(status & more_results);
      response.rows_done = true;
      return false;
    }
    if (auto inspected = inspect_error(bytes); !inspected)
      return std::unexpected(inspected.error());
    if (response.row_count == options.max_rows)
      return std::unexpected(make_error_code(ClientError::resource_limit));

    const auto count = response.metadata.columns.size();
    const auto descriptors = row_prefix(response);
    const auto row_size = sizeof(Row) + descriptors;
    if (bytes.size() > std::numeric_limits<std::size_t>::max() - descriptors)
      return std::unexpected(make_error_code(ClientError::resource_limit));
    if (auto charged = charge(used, row_size); !charged)
      return std::unexpected(charged.error());
    row.columns_ = response.schema;
    row.format_ = response.metadata.format;
    const auto storage_size = descriptors + bytes.size();
    const bool assembled = row.storage_.size() == storage_size &&
      reinterpret_cast<const std::byte *>(row.storage_.data() + descriptors) == bytes.data();
    if (!assembled) {
      if (response.storage.identity() && storage_size > row.storage_.capacity())
        row.storage_ = db::detail::ResultText{db::detail::ResultAllocator<char>{response.storage}};
      row.storage_.resize_and_overwrite(storage_size, [&](char *destination, std::size_t) noexcept {
        if (descriptors)
          std::memset(destination, 0, descriptors);
        std::memcpy(destination + descriptors, bytes.data(), bytes.size());
        return storage_size;
      });
    }
    Reader reader{bytes};
    std::span<const std::byte> bitmap;
    if (row.format_ == Format::binary) {
      if (reader.integer(1) != 0)
        return std::unexpected(make_error_code(ClientError::protocol));
      bitmap = reader.take((count + 9) / 8);
      if (!reader.valid() || (std::to_integer<u8>(bitmap[0]) & 3))
        return std::unexpected(make_error_code(ClientError::protocol));
    }

    for (std::size_t index = 0; index < count; ++index) {
      bool null = false;
      if (row.format_ == Format::binary)
        null = (std::to_integer<u8>(bitmap[(index + 2) / 8]) & (1 << ((index + 2) % 8))) != 0;
      else if (reader.remaining() && bytes[bytes.size() - reader.remaining()] == std::byte{0xfb}) {
        null = true;
        reader.take(1);
      }
      if (null) {
        row.cell(index, {});
        continue;
      }

      std::string_view value;
      const auto type = response.metadata.columns[index].type;
      if (row.format_ == Format::text) {
        value = reader.text();
      } else if (type == 1 || type == 2 || type == 3 || type == 4 || type == 5 || type == 8 || type == 9 ||
        type == 13) {
        const std::size_t width = type == 1 ? 1 : (type == 2 || type == 13) ? 2 : (type == 8 || type == 5) ? 8 : 4;
        value = reader.text(width);
      } else if (type == 7 || type == 10 || type == 11 || type == 12) {
        const auto offset = bytes.size() - reader.remaining();
        const auto size = reader.integer(1);
        const bool time = type == 11;
        const bool valid_size = time ? (size == 0 || size == 8 || size == 12)
                                     : (size == 0 || size == 4 || (type != 10 && (size == 7 || size == 11)));
        if (!valid_size)
          return std::unexpected(make_error_code(ClientError::protocol));
        reader.take(static_cast<std::size_t>(size));
        if (reader.valid())
          value = {reinterpret_cast<const char *>(bytes.data() + offset), static_cast<std::size_t>(size) + 1};
      } else if (type == 0 || type == 15 || type == 16 || type == 245 || type >= 246) {
        value = reader.text();
      } else {
        return std::unexpected(make_error_code(ClientError::unsupported));
      }
      if (!reader.valid())
        return std::unexpected(make_error_code(ClientError::protocol));
      const auto offset = value.empty() ? 0 : reinterpret_cast<const std::byte *>(value.data()) - bytes.data();
      row.cell(index, {static_cast<u32>(offset), static_cast<u32>(value.size())});
    }
    if (!reader.done())
      return std::unexpected(make_error_code(ClientError::protocol));
    ++response.row_count;
    return true;
  }

  Result<void> append_row(std::span<const std::byte> bytes, Response &response, std::size_t &used, Row &storage)
  {
    auto decoded = decode_row(bytes, response, used, storage);
    if (!decoded)
      return std::unexpected(decoded.error());
    if (!*decoded)
      return {};

    auto &rows = response.metadata.rows;
    if (rows.size() == rows.capacity()) {
      const auto minimum_row = sizeof(Row) + (response.metadata.columns.size() - 1) * sizeof(Row::Cell) + 1;
      const auto budget = (options.max_result_bytes - used) / minimum_row;
      const auto remaining = options.max_rows - response.row_count;
      const auto hint = (std::min)({buffered_rows(), budget, remaining});
      const auto maximum = rows.size() + 1 + (std::min)(budget, remaining);
      const auto growth = rows.capacity() + (rows.capacity() + 1) / 2;
      const auto desired = (std::max)(rows.size() + 1 + hint, (std::min)(growth, maximum));
      rows.reserve(desired);
    }
    rows.push_back(std::move(storage));
    return {};
  }

  Result<void> buffered_packet(
    std::span<const std::byte> bytes,
    std::size_t &used,
    Response &response,
    ResponseReader &reader,
    Row *storage = nullptr)
  {
    if (reader.stage != ResponseReader::Stage::done) {
      if (auto parsed = begin_response_packet(bytes, used, response, reader); !parsed)
        return std::unexpected(parsed.error());
      if (reader.stage == ResponseReader::Stage::done && !response.rows_done) {
        // Buffered rows share one result pool. Streaming keeps a separate pool per delivered row.
        response.storage = response.metadata.columns.get_allocator().arena();
        response.metadata.rows = db::detail::ResultList<Row>{db::detail::ResultAllocator<Row>{response.storage}};
      }
      return {};
    }

    if (!storage)
      weave::detail::require(false);
    return append_row(bytes, response, used, *storage);
  }

  Result<bool> drain_cached_response(
    const CancelToken &cancel,
    std::size_t &used,
    Response &response,
    ResponseReader &reader)
  {
    while (!response.rows_done) {
      auto cached = receive_cached(cancel);
      // Match PacketAwaiter's second metadata check after an await_ready miss or error.
      if (reader.stage != ResponseReader::Stage::done && (!cached || !*cached))
        cached = receive_cached(cancel);
      if (!cached)
        return std::unexpected(cached.error());
      if (!*cached)
        return false;

      std::optional<Row> storage;
      if (reader.stage == ResponseReader::Stage::done)
        storage.emplace();
      if (auto parsed = buffered_packet(**cached, used, response, reader, storage ? &*storage : nullptr); !parsed)
        return std::unexpected(parsed.error());
    }
    return true;
  }

  Task<Results> results(Format format)
  {
    Results output;
    std::size_t used = 0;
    auto cancel = co_await weave::detail::GetCancellation{};
    do {
      if (output.size() == options.max_results)
        co_await fail(make_error_code(ClientError::resource_limit));
      if (auto charged = charge(used, sizeof(ResultSet)); !charged)
        co_await fail(charged.error());
      Response response;
      ResponseReader reader{format};
      while (!response.rows_done) {
        auto cached = drain_cached_response(cancel, used, response, reader);
        if (!cached)
          co_await fail(cached.error());
        if (*cached)
          break;

        std::optional<Row> storage;
        if (reader.stage == ResponseReader::Stage::done)
          storage.emplace();
        auto *destination = storage ? &*storage : nullptr;
        auto bytes = co_await receive_slow(
          0,
          destination,
          destination ? row_prefix(response) : 0,
          destination ? &response.storage : nullptr);
        if (auto parsed = buffered_packet(bytes, used, response, reader, destination); !parsed)
          co_await fail(parsed.error());
      }
      output.push_back(std::move(response.metadata));
    } while (!at_boundary);
    co_return output;
  }

  std::size_t buffered_rows() const noexcept
  {
    auto position = input_begin;
    std::size_t count = 0;
    while (input_end - position >= 4) {
      Reader header{std::span{input}.subspan(position, 4)};
      const auto size = static_cast<std::size_t>(header.integer(3));
      if (!size || size == packet_chunk || size > input_end - position - 4)
        break;
      const auto first = input[position + 4];
      if (first == std::byte{0xff} || end_packet(std::to_integer<u8>(first), size))
        break;
      ++count;
      position += 4 + size;
    }
    return count;
  }

  struct Prepared {
    u32 id = 0;
    db::detail::ResultList<Column> parameters;
    db::detail::ResultList<Column> columns;
  };

  Task<Prepared> prepare_command(std::string_view sql)
  {
    if (sql.size() >= options.max_packet_bytes || statements.size() == options.max_statements)
      co_await fail(make_error_code(ClientError::resource_limit));
    sequence = 0;
    at_boundary = false;
    diagnostic.reset();
    const auto cancel = co_await weave::detail::GetCancellation{};
    auto packet = receive(cancel);
    const std::array command{std::byte{22}};
    co_await send(command, std::as_bytes(std::span{sql}));
    auto bytes = co_await packet;
    if (auto inspected = inspect_error(bytes); !inspected)
      co_await fail(inspected.error());
    Reader reader{bytes};
    if (reader.integer(1) != 0)
      co_await fail(make_error_code(ClientError::protocol));
    Prepared prepared;
    prepared.id = static_cast<u32>(reader.integer(4));
    const auto columns = reader.integer(2);
    const auto parameters = reader.integer(2);
    const auto filler = reader.integer(1);
    reader.integer(2);
    if (!reader.done() || filler != 0 || !prepared.id || std::ranges::find(statements, prepared.id) != statements.end())
      co_await fail(make_error_code(ClientError::protocol));
    if (columns > options.max_columns || parameters > options.max_columns)
      co_await fail(make_error_code(ClientError::resource_limit));

    std::size_t used = 0;
    const std::array blocks{std::pair{&prepared.parameters, parameters}, std::pair{&prepared.columns, columns}};
    for (const auto &[destination, count] : blocks) {
      if (auto charged = charge(used, static_cast<std::size_t>(count) * sizeof(Column)); !charged)
        co_await fail(charged.error());
      destination->reserve(static_cast<std::size_t>(count));
      for (u64 index = 0; index < count; ++index) {
        bytes = co_await packet;
        if (auto charged = charge(used, bytes.size()); !charged)
          co_await fail(charged.error());
        // PREPARE_OK promises complete metadata; a later ERR is not a recoverable command boundary.
        if (auto parsed = append_column(*destination, bytes); !parsed)
          co_await fail(parsed.error());
      }
      if (count && !modern_eof) {
        bytes = co_await packet;
        ResultSet metadata;
        if (auto parsed = eof(bytes, metadata); !parsed)
          co_await fail(parsed.error());
        if (metadata.status & more_results)
          co_await fail(make_error_code(ClientError::protocol));
      }
    }
    statements.push_back(prepared.id);
    at_boundary = true;
    co_return prepared;
  }
};

struct Statement::State {
  Connection::Borrow borrow;
  Connection::Impl::Prepared prepared;
  std::vector<std::size_t> long_sizes;
  ExecutePacket packet;
  bool closed = false;

  State(Connection::Borrow owner, Connection::Impl::Prepared description)
      : borrow(std::move(owner)), prepared(std::move(description)), long_sizes(prepared.parameters.size(), 0)
  {
  }

  ~State()
  {
    if (!closed && borrow.impl && !borrow.impl->closed)
      borrow.impl->pending_close.push_back(prepared.id);
  }
};

struct RowStream::State {
  Connection::Borrow borrow;
  Connection::Impl::Response response;
  std::shared_ptr<Statement::State> statement_owner;
  std::size_t schema_bytes = 0;
  std::size_t result_count = 1;
  u64 generation = 0;
  bool reading = false;
  bool completed = false;
  std::optional<std::error_code> failure;

  explicit State(Connection::Borrow owner) : borrow(std::move(owner))
  {
  }

  void release() noexcept
  {
    if (!borrow.impl)
      return;
    if (!borrow.impl->at_boundary)
      static_cast<void>(borrow.impl->retire());
    borrow.impl->busy = false;
    borrow.release();
    statement_owner.reset();
    completed = true;
    ++generation;
  }

  ~State()
  {
    release();
  }

  struct Call {
    State &state;
    bool completed = false;

    ~Call()
    {
      state.reading = false;
      if (!completed)
        state.release();
    }
  };
};

Connection::Borrow::Borrow(Connection &connection) noexcept : impl(connection.impl_.get())
{
  if (impl)
    impl->borrowers.fetch_add(1, std::memory_order_relaxed);
}

Connection::Borrow::Borrow(Borrow &&other) noexcept : impl(std::exchange(other.impl, nullptr))
{
}

Connection::Borrow::~Borrow()
{
  release();
}

void Connection::Borrow::release() noexcept
{
  if (auto *owner = std::exchange(impl, nullptr))
    owner->borrowers.fetch_sub(1, std::memory_order_relaxed);
}

Connection::Connection(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

Connection::Connection(Connection &&other) noexcept
{
  weave::detail::require(
    !other.impl_ || (other.impl_->borrowers.load() == 0 && !other.impl_->busy && !other.impl_->pooled));
  impl_ = std::move(other.impl_);
}

Connection::~Connection()
{
  weave::detail::require(!impl_ || (impl_->borrowers.load() == 0 && !impl_->busy));
}

Task<Connection> Connection::establish(Options options)
{
  const auto password = options.password.view();
  const bool invalid_strings = options.host.empty() || options.user.empty() || contains_nul(options.host) ||
    contains_nul(options.user) || contains_nul(options.database) || contains_nul(password);
  const bool invalid_limits = options.max_packet_bytes < 1024 ||
    options.max_packet_bytes > std::numeric_limits<u32>::max() || options.max_result_bytes == 0 ||
    options.max_rows == 0 || options.max_columns == 0 || options.max_columns > 65535 || options.max_results == 0 ||
    options.max_statements == 0 || options.max_statements > 65535 || options.connect_timeout.count() <= 0;
  if (invalid_strings || invalid_limits || options.host.size() > 65536 || options.user.size() > 256 ||
    options.database.size() > 256 || password.size() > 65536 || (options.plaintext && options.tls))
    co_await fail(make_error_code(ClientError::invalid_options));

  if (!options.plaintext && !options.tls) {
    auto credentials = TlsContext::client();
    if (!credentials)
      co_await fail(credentials.error());
    options.tls = std::move(*credentials);
  }
  if (options.tls && options.tls->verification() != TlsVerification::hostname)
    co_await fail(make_error_code(ClientError::tls_required));

  auto impl = std::make_unique<Impl>(std::move(options));
  impl->context = weave::detail::current_context;
  weave::detail::require(impl->context != nullptr);
  impl->plain.emplace(co_await tcp::connect(*impl->context, impl->options.host, impl->options.port));
  if (auto configured = impl->plain->no_delay(); !configured)
    co_await fail(configured.error());

  auto bytes = co_await impl->receive(65536);
  if (!bytes.empty() && bytes[0] == std::byte{0xff}) {
    auto parsed = failure(bytes, false);
    if (!parsed)
      co_await fail(parsed.error());
    co_await fail(server_error(parsed->number));
  }
  auto server = greeting(bytes);
  if (!server)
    co_await fail(server.error());

  auto capabilities = server->capabilities &
    (long_password | long_flag | protocol_41 | transactions | secure_connection | plugin_auth | multi_resultsets |
      prepared_resultsets | deprecate_eof);
  impl->modern_eof = (capabilities & deprecate_eof) != 0;
  if (impl->options.multi_statements) {
    if (!(server->capabilities & multi_statements))
      co_await fail(make_error_code(ClientError::unsupported));
    capabilities |= multi_statements;
  }
  if (!impl->options.database.empty()) {
    if (!(server->capabilities & connect_with_db))
      co_await fail(make_error_code(ClientError::protocol));
    capabilities |= connect_with_db;
  }

  SecretBytes response;
  response.bytes.reserve(1024 + password.size());
  if (!impl->options.plaintext) {
    if (!(server->capabilities & ssl))
      co_await fail(make_error_code(ClientError::tls_required));
    capabilities |= ssl;
  }
  integer(response.bytes, capabilities, 4);
  integer(response.bytes, impl->options.max_packet_bytes, 4);
  integer(response.bytes, 45, 1); // utf8mb4_general_ci; no SQL escaping/rewriting relies on the collation.
  response.bytes.resize(32, std::byte{0});

  if (!impl->options.plaintext) {
    if (impl->input_begin != impl->input_end)
      co_await fail(make_error_code(ClientError::protocol));
    co_await impl->send(response.bytes);
    impl->secure.emplace(co_await tls::client(std::move(*impl->plain), *impl->options.tls, impl->options.host));
    impl->plain.reset();
    auto info = impl->secure->info();
    if (!info)
      co_await fail(info.error());
    if (!info->certificate_verified || !info->hostname_verified)
      co_await fail(make_error_code(ClientError::tls_required));
  }

  SecretBytes authentication;
  auto initial = proof(server->plugin, password, server->nonce);
  if (!initial)
    co_await fail(initial.error());
  authentication.bytes = std::move(*initial);
  terminated(response.bytes, impl->options.user);
  integer(response.bytes, authentication.bytes.size(), 1);
  response.bytes.insert(response.bytes.end(), authentication.bytes.begin(), authentication.bytes.end());
  if (capabilities & connect_with_db)
    terminated(response.bytes, impl->options.database);
  terminated(response.bytes, server->plugin);
  co_await impl->send(response.bytes);

  bool switched = false;
  bool continuing = false;
  for (unsigned round = 0; round < 5; ++round) {
    bytes = co_await impl->receive(65536);
    if (bytes.empty())
      co_await fail(make_error_code(ClientError::protocol));
    if (bytes[0] == std::byte{0}) {
      auto authenticated = ok(bytes);
      if (!authenticated)
        co_await fail(authenticated.error());
      if (authenticated->status & more_results)
        co_await fail(make_error_code(ClientError::protocol));
      impl->status = authenticated->status;
      impl->options.password = {};
      co_return Connection{std::move(impl)};
    }
    if (bytes[0] == std::byte{0xff}) {
      auto parsed = failure(bytes);
      if (!parsed)
        co_await fail(parsed.error());
      co_await fail(server_error(parsed->number));
    }
    if (bytes[0] == std::byte{0xfe} && !switched && !continuing) {
      Reader reader{bytes};
      reader.take(1);
      server->plugin = reader.terminated();
      auto nonce = reader.take(20);
      if (reader.remaining() == 1 && reader.integer(1) != 0)
        co_await fail(make_error_code(ClientError::protocol));
      if (!reader.done())
        co_await fail(make_error_code(ClientError::protocol));
      auto next = proof(server->plugin, password, nonce);
      if (!next)
        co_await fail(next.error());
      SecretBytes switched_proof;
      switched_proof.bytes = std::move(*next);
      co_await impl->send(switched_proof.bytes);
      switched = true;
      continue;
    }
    if (bytes.size() == 2 && bytes[0] == std::byte{1} && server->plugin == "caching_sha2_password" && !continuing) {
      continuing = true;
      if (bytes[1] == std::byte{3})
        continue;
      if (bytes[1] == std::byte{4}) {
        if (!impl->secure)
          co_await fail(make_error_code(ClientError::tls_required));
        SecretBytes clear;
        clear.bytes.reserve(password.size() + 1);
        terminated(clear.bytes, password);
        co_await impl->send(clear.bytes);
        continue;
      }
    }
    co_await fail(make_error_code(ClientError::protocol));
  }
  co_await fail(make_error_code(ClientError::protocol));
}

Task<Connection> connect(Options options)
{
  if (options.connect_timeout.count() <= 0)
    return Connection::establish(std::move(options));
  auto deadline = options.connect_timeout;
  return timeout(deadline, Connection::establish(std::move(options)));
}

Task<ResultSet> Connection::execute_owned(Borrow borrow, std::string sql, bool migration_owner)
{
  co_return co_await one_result(query_view(std::move(borrow), sql, migration_owner));
}

Task<ResultSet> Connection::execute_view(Borrow borrow, std::string_view sql)
{
  return one_result(query_view(std::move(borrow), sql));
}

Task<Results> Connection::query_owned(Borrow borrow, std::string sql)
{
  co_return co_await query_view(std::move(borrow), sql);
}

Task<Results> Connection::query_view(Borrow borrow, std::string_view sql, bool migration_owner)
{
  if (!borrow.impl || borrow.impl->closed)
    co_await fail(make_error_code(ClientError::closed));
  auto &impl = *borrow.impl;
  if (auto ready = impl.ready(migration_owner); !ready)
    co_await fail(ready.error());
  if (sql.size() >= impl.options.max_packet_bytes)
    co_await fail(make_error_code(ClientError::resource_limit));
  co_await cancellation_point();

  impl.busy = true;
  Impl::Guard guard{impl};
  if (!impl.pending_close.empty())
    co_await impl.flush_closes();
  impl.diagnostic.reset();
  impl.sequence = 0;
  impl.at_boundary = false;
  const std::array command{std::byte{3}};
  co_await impl.send(command, std::as_bytes(std::span{sql}));
  auto response = co_await as_result(impl.results(Format::text));
  if (!response) {
    if (impl.diagnostic)
      guard.completed = true;
    co_await fail(response.error());
  }

  guard.completed = true;
  co_return std::move(*response);
}

Task<ResultSet> Connection::execute(std::string sql)
{
  return execute_owned(Borrow{*this}, std::move(sql));
}

Task<ResultSet> Connection::execute_borrowed(std::string_view sql)
{
  return execute_view(Borrow{*this}, sql);
}

Task<Results> Connection::query(std::string sql)
{
  return query_owned(Borrow{*this}, std::move(sql));
}

Task<Results> Connection::query_borrowed(std::string_view sql)
{
  return query_view(Borrow{*this}, sql);
}

Task<Statement> Connection::prepare_owned(Borrow borrow, std::string sql)
{
  co_return co_await prepare_view(std::move(borrow), sql);
}

Task<Statement> Connection::prepare_view(Borrow borrow, std::string_view sql)
{
  if (!borrow.impl)
    co_await fail(make_error_code(ClientError::closed));
  auto &impl = *borrow.impl;
  if (auto ready = impl.ready(); !ready)
    co_await fail(ready.error());
  co_await cancellation_point();

  impl.busy = true;
  Impl::Guard guard{impl};
  if (!impl.pending_close.empty())
    co_await impl.flush_closes();
  auto description = co_await impl.prepare_command(sql);
  auto state = std::make_shared<Statement::State>(std::move(borrow), std::move(description));
  guard.completed = true;
  co_return Statement{std::move(state)};
}

Task<Statement> Connection::prepare(std::string sql)
{
  return prepare_owned(Borrow{*this}, std::move(sql));
}

Task<Statement> Connection::prepare_borrowed(std::string_view sql)
{
  return prepare_view(Borrow{*this}, sql);
}

Task<RowStream> Connection::stream_owned(Borrow borrow, std::string sql)
{
  co_return co_await stream_view(std::move(borrow), sql);
}

Task<RowStream> Connection::stream_view(Borrow borrow, std::string_view sql)
{
  if (!borrow.impl)
    co_await fail(make_error_code(ClientError::closed));
  auto &impl = *borrow.impl;
  if (auto ready = impl.ready(); !ready)
    co_await fail(ready.error());
  if (sql.size() >= impl.options.max_packet_bytes)
    co_await fail(make_error_code(ClientError::resource_limit));
  co_await cancellation_point();

  impl.busy = true;
  Impl::Guard guard{impl};
  if (!impl.pending_close.empty())
    co_await impl.flush_closes();
  impl.diagnostic.reset();
  impl.sequence = 0;
  impl.at_boundary = false;
  const std::array command{std::byte{3}};
  co_await impl.send(command, std::as_bytes(std::span{sql}));
  std::size_t used = 0;
  Impl::Response response;
  co_await impl.begin_response(Format::text, used, response);
  auto state = std::make_shared<RowStream::State>(std::move(borrow));
  state->response = std::move(response);
  state->schema_bytes = used;
  guard.completed = true;
  RowStream stream{state};
  if (impl.at_boundary)
    state->release();
  else
    guard.keep_busy = true;
  co_return stream;
}

Task<RowStream> Connection::stream(std::string sql)
{
  return stream_owned(Borrow{*this}, std::move(sql));
}

Task<RowStream> Connection::stream_borrowed(std::string_view sql)
{
  return stream_view(Borrow{*this}, sql);
}

Task<ResultSet> Connection::bound_owned(Borrow borrow, std::string sql, std::vector<Parameter> parameters)
{
  auto statement = co_await prepare_view(std::move(borrow), sql);
  auto result = co_await statement.execute(std::move(parameters));
  co_await statement.close();
  co_return result;
}

Task<ResultSet> Connection::bound_view(Borrow borrow, std::string_view sql, std::span<const ParameterView> parameters)
{
  auto statement = co_await prepare_view(std::move(borrow), sql);
  auto result = co_await statement.execute_borrowed(parameters);
  co_await statement.close();
  co_return result;
}

Task<ResultSet> Connection::execute(std::string sql, std::vector<Parameter> parameters)
{
  return bound_owned(Borrow{*this}, std::move(sql), std::move(parameters));
}

Task<ResultSet> Connection::execute_borrowed(std::string_view sql, std::span<const ParameterView> parameters)
{
  return bound_view(Borrow{*this}, sql, parameters);
}

Task<void> Connection::finish(Borrow borrow)
{
  if (!borrow.impl || borrow.impl->closed)
    co_return;
  auto &impl = *borrow.impl;
  if (impl.busy || impl.migrating)
    co_await fail(make_error_code(ClientError::busy));
  co_await cancellation_point();

  impl.busy = true;
  Impl::Guard guard{impl};
  impl.sequence = 0;
  impl.at_boundary = false;
  const std::array command{std::byte{1}};
  co_await impl.send(command);
  if (impl.secure)
    co_await impl.secure->shutdown_send();
  if (auto closed = impl.retire(); !closed)
    co_await fail(closed.error());
  guard.completed = true;
}

Task<void> Connection::disconnect()
{
  return finish(Borrow{*this});
}

Result<void> Connection::close() noexcept
{
  if (!impl_)
    return {};
  if (impl_->busy || impl_->migrating)
    return std::unexpected(make_error_code(ClientError::busy));
  return impl_->retire();
}

bool Connection::open() const noexcept
{
  return impl_ && !impl_->closed;
}

Result<std::optional<Failure>> Connection::last_failure() const
{
  if (!impl_)
    return std::optional<Failure>{};
  if (impl_->busy)
    return std::unexpected(make_error_code(ClientError::busy));
  return impl_->diagnostic;
}

Result<std::optional<TlsInfo>> Connection::tls_info() const
{
  if (!impl_)
    return std::optional<TlsInfo>{};
  if (impl_->busy)
    return std::unexpected(make_error_code(ClientError::busy));
  if (!impl_->secure)
    return std::optional<TlsInfo>{};
  auto info = impl_->secure->info();
  if (!info)
    return std::unexpected(info.error());
  return std::optional<TlsInfo>{std::move(*info)};
}

Statement::Statement(std::shared_ptr<State> state) noexcept : state_(std::move(state))
{
}

Statement::Statement(Statement &&) noexcept = default;
Statement::~Statement() = default;

std::span<const Column> Statement::parameters() const noexcept
{
  weave::detail::require(state_ != nullptr);
  return state_->prepared.parameters;
}

std::span<const Column> Statement::columns() const noexcept
{
  weave::detail::require(state_ != nullptr);
  return state_->prepared.columns;
}

Task<Results> Statement::run(std::shared_ptr<State> state, std::vector<Parameter> parameters)
{
  std::vector<ParameterView> views;
  views.reserve(parameters.size());
  for (const auto &parameter : parameters)
    views.push_back(parameter.view());
  co_return co_await run_view(std::move(state), views);
}

Task<Results> Statement::run_view(std::shared_ptr<State> state, std::span<const ParameterView> parameters)
{
  if (!state || state->closed || !state->borrow.impl)
    co_await fail(make_error_code(ClientError::closed));
  auto &impl = *state->borrow.impl;
  if (auto ready = impl.ready(); !ready)
    co_await fail(ready.error());
  ExecutePacket::Call command{state->packet};
  if (auto encoded = execute_packet(
        state->packet,
        state->prepared.id,
        parameters,
        state->long_sizes,
        impl.options.max_packet_bytes);
    !encoded)
    co_await fail(encoded.error());
  co_await cancellation_point();

  impl.busy = true;
  Connection::Impl::Guard guard{impl};
  if (!impl.pending_close.empty())
    co_await impl.flush_closes();
  impl.sequence = 0;
  impl.at_boundary = false;
  impl.diagnostic.reset();
  co_await impl.send(state->packet.header);
  std::ranges::fill(state->long_sizes, 0);
  auto results = co_await impl.results(Format::binary);
  guard.completed = true;
  co_return results;
}

Task<ResultSet> Statement::execute(std::vector<Parameter> parameters)
{
  return one_result(run(state_, std::move(parameters)));
}

Task<ResultSet> Statement::execute_borrowed(std::span<const ParameterView> parameters)
{
  return one_result(run_view(state_, parameters));
}

Task<Results> Statement::query(std::vector<Parameter> parameters)
{
  return run(state_, std::move(parameters));
}

Task<Results> Statement::query_borrowed(std::span<const ParameterView> parameters)
{
  return run_view(state_, parameters);
}

Task<RowStream> Statement::start(std::shared_ptr<State> state, std::vector<Parameter> parameters)
{
  std::vector<ParameterView> views;
  views.reserve(parameters.size());
  for (const auto &parameter : parameters)
    views.push_back(parameter.view());
  co_return co_await start_view(std::move(state), views);
}

Task<RowStream> Statement::start_view(std::shared_ptr<State> statement, std::span<const ParameterView> parameters)
{
  if (!statement || statement->closed || !statement->borrow.impl)
    co_await fail(make_error_code(ClientError::closed));
  auto &impl = *statement->borrow.impl;
  if (auto ready = impl.ready(); !ready)
    co_await fail(ready.error());
  ExecutePacket::Call command{statement->packet};
  if (auto encoded = execute_packet(
        statement->packet,
        statement->prepared.id,
        parameters,
        statement->long_sizes,
        impl.options.max_packet_bytes);
    !encoded)
    co_await fail(encoded.error());
  co_await cancellation_point();

  impl.busy = true;
  Connection::Impl::Guard guard{impl};
  if (!impl.pending_close.empty())
    co_await impl.flush_closes();
  impl.sequence = 0;
  impl.at_boundary = false;
  impl.diagnostic.reset();
  co_await impl.send(statement->packet.header);
  std::ranges::fill(statement->long_sizes, 0);
  std::size_t used = 0;
  Connection::Impl::Response response;
  co_await impl.begin_response(Format::binary, used, response);
  Connection::Borrow borrow;
  borrow.impl = &impl;
  impl.borrowers.fetch_add(1, std::memory_order_relaxed);
  auto state = std::make_shared<RowStream::State>(std::move(borrow));
  state->response = std::move(response);
  state->schema_bytes = used;
  state->statement_owner = std::move(statement);
  guard.completed = true;
  if (impl.at_boundary)
    state->release();
  else
    guard.keep_busy = true;
  co_return RowStream{std::move(state)};
}

Task<RowStream> Statement::stream(std::vector<Parameter> parameters)
{
  return start(state_, std::move(parameters));
}

Task<RowStream> Statement::stream_borrowed(std::span<const ParameterView> parameters)
{
  return start_view(state_, parameters);
}

Task<void> Statement::finish(std::shared_ptr<State> state)
{
  if (!state || state->closed)
    co_return;
  auto &impl = *state->borrow.impl;
  if (auto ready = impl.ready(); !ready) {
    if (impl.closed) {
      state->closed = true;
      state->borrow.release();
      co_return;
    }
    co_await fail(ready.error());
  }
  co_await cancellation_point();
  impl.busy = true;
  Connection::Impl::Guard guard{impl};
  if (!impl.pending_close.empty())
    co_await impl.flush_closes();
  co_await impl.close_statement(state->prepared.id);
  state->closed = true;
  state->borrow.release();
  guard.completed = true;
}

Task<void> Statement::close()
{
  return finish(state_);
}

Task<void> Statement::reset_impl(std::shared_ptr<State> state)
{
  if (!state || state->closed || !state->borrow.impl)
    co_await fail(make_error_code(ClientError::closed));
  auto &impl = *state->borrow.impl;
  if (auto ready = impl.ready(); !ready)
    co_await fail(ready.error());
  co_await cancellation_point();
  impl.busy = true;
  Connection::Impl::Guard guard{impl};
  if (!impl.pending_close.empty())
    co_await impl.flush_closes();
  impl.sequence = 0;
  impl.at_boundary = false;
  impl.diagnostic.reset();
  std::vector<std::byte> command;
  integer(command, 26, 1);
  integer(command, state->prepared.id, 4);
  co_await impl.send(command);
  std::ranges::fill(state->long_sizes, 0);
  auto results = co_await impl.results(Format::text);
  if (results.size() != 1 || !results[0].columns.empty())
    co_await fail(make_error_code(ClientError::protocol));
  guard.completed = true;
}

Task<void> Statement::reset()
{
  return reset_impl(state_);
}

Task<void> Statement::long_data(std::shared_ptr<State> state, u16 parameter, std::span<const std::byte> bytes)
{
  if (!state || state->closed || !state->borrow.impl)
    co_await fail(make_error_code(ClientError::closed));
  auto &impl = *state->borrow.impl;
  if (auto ready = impl.ready(); !ready)
    co_await fail(ready.error());
  if (parameter >= state->long_sizes.size())
    co_await fail(make_error_code(ClientError::invalid_options));
  const auto used = state->long_sizes[parameter] ? state->long_sizes[parameter] - 1 : 0;
  if (bytes.size() > impl.options.max_packet_bytes - 7 || bytes.size() > impl.options.max_packet_bytes - used)
    co_await fail(make_error_code(ClientError::resource_limit));
  co_await cancellation_point();
  impl.busy = true;
  Connection::Impl::Guard guard{impl};
  if (!impl.pending_close.empty())
    co_await impl.flush_closes();
  impl.sequence = 0;
  impl.at_boundary = false;
  std::vector<std::byte> command;
  integer(command, 24, 1);
  integer(command, state->prepared.id, 4);
  integer(command, parameter, 2);
  co_await impl.send(command, bytes);
  state->long_sizes[parameter] = used + bytes.size() + 1;
  impl.at_boundary = true;
  guard.completed = true;
}

Task<void> Statement::send_long_data(u16 parameter, std::span<const std::byte> bytes)
{
  return long_data(state_, parameter, bytes);
}

RowStream::RowStream(std::shared_ptr<State> state) noexcept : state_(std::move(state))
{
}

RowStream::RowStream(RowStream &&) noexcept = default;
RowStream::~RowStream() = default;

Task<std::optional<Row>> RowStream::read(std::shared_ptr<State> state, u64 generation)
{
  if (!state)
    co_await fail(make_error_code(ClientError::closed));
  if (state->failure)
    co_await fail(*state->failure);
  if (generation != state->generation || state->reading)
    co_await fail(make_error_code(ClientError::busy));
  if (state->completed || state->response.rows_done)
    co_return std::nullopt;
  co_await cancellation_point();
  state->reading = true;
  State::Call guard{*state};
  auto used = state->schema_bytes;
  auto &impl = *state->borrow.impl;
  Result<std::optional<std::span<const std::byte>>> cached;
  {
    const auto &cancel = co_await weave::detail::GetCancellationReference{};
    cached = impl.receive_cached(cancel);
  }
  if (!cached) {
    state->failure = cached.error();
    co_await fail(cached.error());
  }
  std::span<const std::byte> bytes;
  Row row_storage;
  if (*cached) {
    bytes = **cached;
  } else {
    const auto prefix = impl.row_prefix(state->response);
    auto received = co_await as_result(impl.receive_slow(0, &row_storage, prefix));
    if (!received) {
      state->failure = received.error();
      co_await fail(received.error());
    }
    bytes = *received;
  }
  auto decoded = impl.decode_row(bytes, state->response, used, row_storage);
  if (!decoded) {
    state->failure = decoded.error();
    co_await fail(decoded.error());
  }
  if (!*decoded && state->borrow.impl->at_boundary)
    state->release();
  guard.completed = true;
  if (!*decoded)
    co_return std::nullopt;
  co_return std::move(row_storage);
}

Task<std::optional<Row>> RowStream::next()
{
  return read(state_, state_ ? state_->generation : 0);
}

Task<bool> RowStream::advance(std::shared_ptr<State> state, u64 generation)
{
  if (!state)
    co_await fail(make_error_code(ClientError::closed));
  if (state->failure)
    co_await fail(*state->failure);
  if (generation != state->generation || state->reading)
    co_await fail(make_error_code(ClientError::busy));
  if (state->completed)
    co_return false;
  if (!state->response.rows_done)
    co_await fail(make_error_code(ClientError::busy));
  co_await cancellation_point();
  state->reading = true;
  State::Call guard{*state};
  auto &impl = *state->borrow.impl;
  if (state->result_count == impl.options.max_results) {
    state->failure = make_error_code(ClientError::resource_limit);
    co_await fail(*state->failure);
  }
  std::size_t used = 0;
  Connection::Impl::Response response;
  auto result = co_await as_result(impl.begin_response(state->response.metadata.format, used, response));
  if (!result) {
    state->failure = result.error();
    co_await fail(result.error());
  }
  state->response = std::move(response);
  state->schema_bytes = used;
  ++state->result_count;
  ++state->generation;
  if (impl.at_boundary)
    state->release();
  guard.completed = true;
  co_return true;
}

Task<bool> RowStream::next_result()
{
  return advance(state_, state_ ? state_->generation : 0);
}

Task<void> RowStream::drain(std::shared_ptr<State> state)
{
  if (!state)
    co_await fail(make_error_code(ClientError::closed));
  do {
    while (co_await read(state, state->generation)) {
    }
  } while (co_await advance(state, state->generation));
}

Task<void> RowStream::finish()
{
  return drain(state_);
}

Result<ResultSet> RowStream::summary() const
{
  if (!state_)
    return std::unexpected(make_error_code(ClientError::closed));
  if (state_->reading)
    return std::unexpected(make_error_code(ClientError::busy));
  if (state_->failure)
    return std::unexpected(*state_->failure);
  return state_->response.metadata;
}

bool RowStream::done() const noexcept
{
  return state_ && state_->completed;
}

Task<db::detail::ResultList<Outcome>> Connection::batch_impl(Borrow borrow, std::vector<Command> commands)
{
  if (!borrow.impl)
    co_await fail(make_error_code(ClientError::closed));
  auto &impl = *borrow.impl;
  if (auto ready = impl.ready(); !ready)
    co_await fail(ready.error());
  if (commands.size() > impl.options.max_results)
    co_await fail(make_error_code(ClientError::resource_limit));
  for (const auto &command : commands) {
    if (command.sql.size() >= impl.options.max_packet_bytes || command.parameters.size() > impl.options.max_columns)
      co_await fail(make_error_code(ClientError::resource_limit));
  }
  co_await cancellation_point();
  impl.busy = true;
  Impl::Guard guard{impl};
  if (!impl.pending_close.empty())
    co_await impl.flush_closes();
  db::detail::ResultList<Outcome> outcomes;
  std::size_t retained = 0;
  for (const auto &command : commands) {
    impl.diagnostic.reset();
    Outcome outcome;
    const bool prepared = command.prepared || !command.parameters.empty();
    std::optional<u32> id;
    if (prepared) {
      auto description = co_await as_result(impl.prepare_command(command.sql));
      if (!description) {
        if (!impl.diagnostic)
          co_await fail(description.error());
        outcome.failure = impl.diagnostic;
      } else {
        id = description->id;
        std::vector<ParameterView> parameters;
        for (const auto &parameter : command.parameters)
          parameters.push_back(parameter.view());
        std::vector<std::size_t> long_sizes(description->parameters.size(), 0);
        ExecutePacket packet;
        if (auto encoded = execute_packet(packet, *id, parameters, long_sizes, impl.options.max_packet_bytes); !encoded)
          co_await fail(encoded.error());
        impl.sequence = 0;
        impl.at_boundary = false;
        co_await impl.send(packet.header);
      }
    } else {
      impl.sequence = 0;
      impl.at_boundary = false;
      const std::array prefix{std::byte{3}};
      co_await impl.send(prefix, std::as_bytes(std::span{command.sql}));
    }
    if (!outcome.failure) {
      auto result = co_await as_result(impl.results(prepared ? Format::binary : Format::text));
      if (!result) {
        if (!impl.diagnostic)
          co_await fail(result.error());
        outcome.failure = impl.diagnostic;
      } else {
        outcome.results = std::move(*result);
      }
    }
    if (id)
      co_await impl.close_statement(*id);
    // Charge metadata, diagnostics and payloads across all commands, not just each response.
    if (auto charged = impl.charge(retained, sizeof(Outcome)); !charged)
      co_await fail(charged.error());
    if (outcome.failure) {
      const auto bytes = sizeof(Failure) + outcome.failure->sqlstate.size() + outcome.failure->message.size();
      if (auto charged = impl.charge(retained, bytes); !charged) {
        static_cast<void>(impl.retire());
        co_await fail(charged.error());
      }
    }
    for (const auto &result : outcome.results) {
      if (auto charged = impl.charge(retained, sizeof(ResultSet) + result.columns.size() * sizeof(Column)); !charged)
        co_await fail(charged.error());
      if (auto charged = impl.charge(retained, result.info.size()); !charged)
        co_await fail(charged.error());
      for (const auto &column : result.columns) {
        const auto bytes = column.schema.size() + column.table.size() + column.original_table.size() +
          column.name.size() + column.original_name.size();
        if (auto charged = impl.charge(retained, 2 * (sizeof(Column) + bytes)); !charged)
          co_await fail(charged.error());
      }
      for (const auto &row : result.rows) {
        if (auto charged = impl.charge(
              retained,
              sizeof(Row) + row.size() * sizeof(std::optional<db::detail::ResultText>));
          !charged)
          co_await fail(charged.error());
        for (std::size_t index = 0; index < row.size(); ++index) {
          auto data = row[index].bytes();
          if (data) {
            if (auto charged = impl.charge(retained, data->size()); !charged)
              co_await fail(charged.error());
          }
        }
      }
    }
    outcomes.push_back(std::move(outcome));
  }
  guard.completed = true;
  co_return outcomes;
}

Task<db::detail::ResultList<Outcome>> Connection::batch(std::vector<Command> commands)
{
  return batch_impl(Borrow{*this}, std::move(commands));
}

Task<void> Connection::reset_impl(Borrow borrow)
{
  if (!borrow.impl)
    co_await fail(make_error_code(ClientError::closed));
  auto &impl = *borrow.impl;
  if (auto ready = impl.ready(); !ready)
    co_await fail(ready.error());
  if (impl.borrowers.load(std::memory_order_relaxed) != 1)
    co_await fail(make_error_code(ClientError::busy));
  co_await cancellation_point();
  impl.busy = true;
  Impl::Guard guard{impl};
  impl.sequence = 0;
  impl.at_boundary = false;
  impl.diagnostic.reset();
  const std::array command{std::byte{31}};
  co_await impl.send(command);
  auto reset = co_await impl.results(Format::text);
  if (reset.size() != 1 || !reset[0].columns.empty())
    co_await fail(make_error_code(ClientError::protocol));
  impl.statements.clear();
  impl.pending_close.clear();
  impl.sequence = 0;
  impl.at_boundary = false;
  const std::array query{std::byte{3}};
  constexpr std::string_view charset = "SET NAMES utf8mb4 COLLATE utf8mb4_general_ci";
  co_await impl.send(query, std::as_bytes(std::span{charset}));
  co_await impl.results(Format::text);
  if (!impl.options.database.empty()) {
    impl.sequence = 0;
    impl.at_boundary = false;
    const std::array select{std::byte{2}};
    co_await impl.send(select, std::as_bytes(std::span{impl.options.database}));
    co_await impl.results(Format::text);
  }
  guard.completed = true;
}

Task<void> Connection::reset()
{
  return reset_impl(Borrow{*this});
}

void detail::PoolAccess::enroll(Connection &connection) noexcept
{
  weave::detail::require(connection.impl_ != nullptr);
  connection.impl_->pooled = true;
}

bool detail::PoolAccess::recycle(Connection &connection) noexcept
{
  weave::detail::require(connection.impl_ != nullptr);
  auto &impl = *connection.impl_;
  weave::detail::require(impl.borrowers.load(std::memory_order_relaxed) == 0 && !impl.busy && !impl.migrating);
  return !impl.closed && impl.at_boundary;
}

detail::MigrationAccess::Lease::Lease(Connection &session) noexcept : connection(&session), borrow(session)
{
}

detail::MigrationAccess::Lease::Lease(Lease &&other) noexcept
    : connection(other.connection), borrow(std::move(other.borrow)), acquired(std::exchange(other.acquired, false))
{
}

detail::MigrationAccess::Lease::~Lease()
{
  if (acquired) {
    weave::detail::require(borrow.impl != nullptr && !borrow.impl->busy);
    borrow.impl->migrating = false;
  }
}

Result<void> detail::MigrationAccess::Lease::acquire() noexcept
{
  if (!borrow.impl || borrow.impl->closed || borrow.impl->busy || borrow.impl->migrating ||
    borrow.impl->borrowers.load(std::memory_order_relaxed) != 1 || !(borrow.impl->status & 2) ||
    (borrow.impl->status & 1))
    return std::unexpected(db::make_error_code(db::MigrationError::invalid_session));
  borrow.impl->migrating = true;
  acquired = true;
  return {};
}

detail::MigrationAccess::Lease detail::MigrationAccess::borrow(Connection &connection) noexcept
{
  return Lease{connection};
}

Task<ResultSet> detail::MigrationAccess::execute(Lease &lease, std::string sql)
{
  weave::detail::require(lease.acquired);
  return Connection::execute_owned(Connection::Borrow{*lease.connection}, std::move(sql), true);
}

bool detail::MigrationAccess::idle(const Lease &lease) noexcept
{
  return lease.borrow.impl && !lease.borrow.impl->closed && (lease.borrow.impl->status & 2) &&
    !(lease.borrow.impl->status & 1);
}

void detail::MigrationAccess::close(Lease &lease) noexcept
{
  if (lease.borrow.impl)
    static_cast<void>(lease.borrow.impl->retire());
}

} // namespace weave::mysql
