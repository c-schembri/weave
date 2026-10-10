#include "wire.hpp"
#include <array>

namespace weave::pg {

namespace {

class ProtocolCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "weave.postgres";
  }

  std::string message(int value) const override
  {
    switch (static_cast<Error>(value)) {
    case Error::protocol:
      return "Invalid PostgreSQL protocol message";
    case Error::authentication:
      return "PostgreSQL authentication failed";
    case Error::unsupported_authentication:
      return "Unsupported PostgreSQL authentication method";
    case Error::resource_limit:
      return "PostgreSQL resource limit exceeded";
    case Error::closed:
      return "PostgreSQL connection is closed";
    case Error::busy:
      return "PostgreSQL connection is busy";
    case Error::unexpected_copy:
      return "Use the PostgreSQL COPY streaming API for this command";
    case Error::target_session:
      return "No acceptable PostgreSQL target session";
    }

    return "Unknown PostgreSQL error";
  }
};

class SqlCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "postgres.sqlstate";
  }

  std::string message(int value) const override
  {
    return "PostgreSQL SQLSTATE " + sqlstate({value, *this});
  }
};

const std::error_category &protocol_category()
{
  static const ProtocolCategory category;
  return category;
}

const std::error_category &sql_category()
{
  static const SqlCategory category;
  return category;
}

Result<std::string> identifier_name(std::string_view identifier, Encoding encoding, std::size_t limit)
{
  if (identifier.size() > limit)
    return std::unexpected(make_error_code(Error::resource_limit));
  if (auto info = encoding_info(encoding); !info)
    return std::unexpected(info.error());
  if (identifier.empty())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  const bool quoted = identifier.front() == '"';
  if (quoted) {
    if (identifier.size() < 2 || identifier.back() != '"')
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    identifier = identifier.substr(1, identifier.size() - 2);
  }

  std::string name;
  name.reserve(identifier.size());
  while (!identifier.empty()) {
    auto length = character_size(identifier, encoding);
    if (!length)
      return std::unexpected(length.error());

    const auto byte = static_cast<u8>(identifier.front());
    if (quoted && *length == 1 && byte == '"') {
      if (identifier.size() < 2 || identifier[1] != '"')
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
      name.push_back('"');
      identifier.remove_prefix(2);
      continue;
    }

    if (!quoted && *length == 1 && byte < 0x80) {
      const bool letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || byte == '_';
      const bool suffix = !name.empty() && ((byte >= '0' && byte <= '9') || byte == '$');
      if (!letter && !suffix)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

      name.push_back(static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte));
    } else {
      name.append(identifier.substr(0, *length));
    }
    identifier.remove_prefix(*length);
  }

  return name;
}

} // namespace

std::error_code make_error_code(Error error) noexcept
{
  return {static_cast<int>(error), protocol_category()};
}

std::error_code sql_error(std::string_view state) noexcept
{
  if (state.size() != 5)
    return make_error_code(Error::protocol);

  int value = 0;
  for (char digit : state) {
    int decoded = digit >= '0' && digit <= '9' ? digit - '0' : digit - 'A' + 10;
    if (decoded < 0 || decoded >= 36 || (digit > '9' && digit < 'A'))
      return make_error_code(Error::protocol);

    value = value * 36 + decoded;
  }

  if (!value)
    return make_error_code(Error::protocol);

  return {value, sql_category()};
}

std::string sqlstate(std::error_code error)
{
  if (&error.category() != &sql_category() || error.value() <= 0 || error.value() >= 60466176)
    return {};

  std::string state(5, '0');
  auto value = error.value();
  for (auto index = state.size(); index > 0; --index) {
    auto digit = value % 36;
    state[index - 1] = static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10);
    value /= 36;
  }

  return state;
}

std::string_view Diagnostic::field(char code) const noexcept
{
  for (const auto &[key, value] : fields) {
    if (key == code)
      return value;
  }

  return {};
}

std::string_view Diagnostic::message() const noexcept
{
  return field('M');
}

std::string_view Diagnostic::sqlstate() const noexcept
{
  return field('C');
}

Result<Outcome> Outcome::failure(Diagnostic diagnostic, std::size_t limit)
{
  std::array<bool, 256> present{};
  std::size_t bytes = 0;
  for (const auto &[code, text] : diagnostic.fields) {
    const auto index = static_cast<u8>(code);
    if (!index || present[index] || text.find('\0') != std::string::npos)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    present[index] = true;
    if (bytes == limit || text.size() > limit - bytes - 1)
      return std::unexpected(make_error_code(Error::resource_limit));
    bytes += text.size() + 1;
  }
  if (diagnostic.message().empty())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if (present['C'] && sql_error(diagnostic.sqlstate()) == make_error_code(Error::protocol))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  Outcome outcome;
  outcome.error = std::move(diagnostic);
  return outcome;
}

std::string_view result_kind_name(ResultKind kind) noexcept
{
  switch (kind) {
  case ResultKind::uninitialized:
    return "uninitialized";
  case ResultKind::empty_query:
    return "empty_query";
  case ResultKind::command:
    return "command";
  case ResultKind::tuples:
    return "tuples";
  case ResultKind::description:
    return "description";
  case ResultKind::row_chunk:
    return "row_chunk";
  case ResultKind::acknowledgment:
    return "acknowledgment";
  }
  return "unknown";
}

std::string_view status_name(const ResultSet &result) noexcept
{
  return result_kind_name(result.kind);
}

std::string_view status_name(const Diagnostic &diagnostic) noexcept
{
  return diagnostic.fields.empty() ? "uninitialized" : "diagnostic";
}

std::string_view status_name(const Outcome &outcome) noexcept
{
  const bool diagnostic = !outcome.error.fields.empty();
  const unsigned states = static_cast<unsigned>(outcome.result.has_value()) + static_cast<unsigned>(diagnostic) +
    static_cast<unsigned>(outcome.aborted);
  if (states > 1)
    return "invalid";

  if (outcome.result)
    return status_name(*outcome.result);
  if (outcome.aborted)
    return "aborted";
  if (diagnostic)
    return "sql_error";

  return "uninitialized";
}

std::string_view status_name(const CopyFormat &format) noexcept
{
  switch (format.direction) {
  case CopyDirection::input:
    return "copy_input";
  case CopyDirection::output:
    return "copy_output";
  case CopyDirection::both:
    return "copy_both";
  }
  return "unknown";
}

std::string_view status_name(const ExchangeEvent &event) noexcept
{
  if (auto result = std::get_if<ResultSet>(&event))
    return status_name(*result);
  if (auto format = std::get_if<CopyFormat>(&event))
    return status_name(*format);
  if (std::holds_alternative<std::vector<std::byte>>(event))
    return "copy_data";
  if (std::holds_alternative<CopyDone>(event))
    return "copy_done";

  return "invalid";
}

std::string_view status_name(const PipelineResult &result) noexcept
{
  switch (result.kind) {
  case PipelineKind::execute:
  case PipelineKind::prepare:
  case PipelineKind::describe:
  case PipelineKind::close:
    break;
  case PipelineKind::sync:
    if (!result.complete || result.outcome.result || result.outcome.aborted || !result.outcome.error.fields.empty())
      return "invalid";
    if (!result.transaction)
      return "uninitialized";

    switch (*result.transaction) {
    case Transaction::idle:
    case Transaction::active:
    case Transaction::failed:
      return "pipeline_sync";
    }
    return "invalid";
  default:
    return "unknown";
  }

  if (result.transaction)
    return "invalid";
  if (!result.complete) {
    const bool chunk = result.kind == PipelineKind::execute && result.outcome.result &&
      result.outcome.result->kind == ResultKind::row_chunk;
    if (!chunk || result.outcome.aborted || !result.outcome.error.fields.empty())
      return "invalid";
  } else if (result.outcome.result) {
    switch (result.outcome.result->kind) {
    case ResultKind::uninitialized:
      break;
    case ResultKind::empty_query:
    case ResultKind::command:
    case ResultKind::tuples:
      if (result.kind != PipelineKind::execute)
        return "invalid";
      break;
    case ResultKind::description:
      if (result.kind != PipelineKind::describe)
        return "invalid";
      break;
    case ResultKind::acknowledgment:
      if (result.kind != PipelineKind::prepare && result.kind != PipelineKind::close)
        return "invalid";
      break;
    case ResultKind::row_chunk:
      return "invalid";
    default:
      return "unknown";
    }
  }

  return status_name(result.outcome);
}

std::string_view status_name(std::error_code error) noexcept
{
  if (!error)
    return "success";
  if (&error.category() == &sql_category())
    return error.value() > 0 && error.value() < 60466176 ? "sql_error" : "unknown";
  if (&error.category() == &protocol_category()) {
    switch (static_cast<Error>(error.value())) {
    case Error::protocol:
      return "protocol_error";
    case Error::authentication:
      return "authentication_error";
    case Error::unsupported_authentication:
      return "unsupported_authentication";
    case Error::resource_limit:
      return "resource_limit";
    case Error::closed:
      return "closed";
    case Error::busy:
      return "busy";
    case Error::unexpected_copy:
      return "unexpected_copy";
    case Error::target_session:
      return "target_session";
    }
    return "unknown";
  }
  const bool standard_category = &error.category() == &std::generic_category() ||
    &error.category() == &std::system_category();
  if (standard_category && error == std::errc::operation_canceled)
    return "canceled";

  return "error";
}

bool Value::is_null() const noexcept
{
  return !data;
}

std::string_view Value::bytes() const noexcept
{
  return data ? std::string_view{*data} : std::string_view{};
}

Result<std::optional<std::size_t>> ResultSet::column_index(
  std::string_view identifier,
  Encoding encoding,
  std::size_t limit) const
{
  auto name = identifier_name(identifier, encoding, limit);
  if (!name)
    return std::unexpected(name.error());

  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (columns[index].name == *name)
      return index;
  }

  return std::nullopt;
}

Result<std::string_view> ResultSet::inserted_oid_text() const noexcept
{
  const std::string_view tag{command};
  if (!tag.starts_with("INSERT "))
    return std::string_view{};

  auto end = tag.find(' ', 7);
  auto text = tag.substr(7, end == std::string_view::npos ? end : end - 7);
  u32 value = 0;
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  return text;
}

Result<u32> ResultSet::inserted_oid() const noexcept
{
  auto text = inserted_oid_text();
  if (!text)
    return std::unexpected(text.error());
  if (text->empty())
    return u32{0};

  u32 value = 0;
  std::from_chars(text->data(), text->data() + text->size(), value);
  return value;
}

Result<u64> ResultSet::affected_rows() const noexcept
{
  const std::array<std::string_view, 8>
    counted{"SELECT ", "INSERT ", "UPDATE ", "DELETE ", "MERGE ", "MOVE ", "FETCH ", "COPY "};
  bool has_count = std::any_of(counted.begin(), counted.end(), [this](auto prefix) {
    return command.starts_with(prefix);
  });
  if (!has_count)
    return u64{0};

  auto position = command.find_last_of(' ');
  if (position == std::string::npos)
    return u64{0};

  auto count = std::string_view{command}.substr(position + 1);
  u64 value = 0;
  auto parsed = std::from_chars(count.data(), count.data() + count.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  return value;
}

namespace detail {

Result<Diagnostic> diagnostic(std::span<const std::byte> body)
{
  Reader reader{body};
  Diagnostic result;
  while (reader.valid()) {
    auto code = static_cast<char>(reader.integer(1));
    if (!code) {
      if (!reader.empty())
        break;

      return result;
    }

    auto value = reader.string();
    bool repeated = std::any_of(result.fields.begin(), result.fields.end(), [code](const auto &field) {
      return field.first == code;
    });
    if (!reader.valid() || repeated)
      break;

    result.fields.emplace_back(code, std::move(value));
  }

  return std::unexpected(make_error_code(Error::protocol));
}

Result<std::vector<Column>> columns(std::span<const std::byte> body, ResultArena arena)
{
  Reader reader{body};
  auto count = reader.integer(2);
  std::vector<Column> result;
  for (u32 index = 0; index < count && reader.valid(); ++index) {
    Column column;
    auto name = reader.string();
    column.name = ResultText{name.data(), name.size(), ResultAllocator<char>{arena}};
    column.table = reader.integer();
    column.attribute = static_cast<i16>(reader.integer(2));
    column.type = reader.integer();
    column.type_size = static_cast<i16>(reader.integer(2));
    column.modifier = static_cast<i32>(reader.integer());
    column.format = static_cast<Format>(reader.integer(2));
    if (column.format != Format::text && column.format != Format::binary)
      return std::unexpected(make_error_code(Error::protocol));

    result.push_back(std::move(column));
  }

  if (!reader.empty())
    return std::unexpected(make_error_code(Error::protocol));

  return result;
}

Result<Row> row(std::span<const std::byte> body, std::size_t columns, ResultArena arena)
{
  Reader reader{body};
  if (reader.integer(2) != columns)
    return std::unexpected(make_error_code(Error::protocol));

  if (!arena.identity())
    arena = ResultArena::create();
  Row result{ResultAllocator<Value>{arena}};
  for (std::size_t index = 0; index < columns && reader.valid(); ++index) {
    auto size = reader.integer();
    if (size == std::numeric_limits<u32>::max()) {
      result.push_back({std::nullopt});
      continue;
    }

    auto bytes = reader.take(size);
    if (!reader.valid())
      break;

    auto *data = bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data());
    result.push_back({ResultText{data, bytes.size(), ResultAllocator<char>{arena}}});
  }

  if (!reader.empty())
    return std::unexpected(make_error_code(Error::protocol));

  return result;
}

Result<Notification> notification(std::span<const std::byte> body)
{
  Reader reader{body};
  Notification value;
  value.process = reader.integer();
  value.channel = reader.string();
  value.payload = reader.string();
  if (!reader.empty())
    return std::unexpected(make_error_code(Error::protocol));

  return value;
}

Result<Row> row(std::span<const std::byte> body, std::span<const Column> columns, ResultArena arena)
{
  auto values = row(body, columns.size(), std::move(arena));
  if (!values)
    return std::unexpected(values.error());

  for (std::size_t index = 0; index < values->size(); ++index)
    (*values)[index].format = columns[index].format;

  return values;
}

Result<void> bind(
  Writer &request,
  std::string_view name,
  const std::vector<Parameter> &parameters,
  Format format,
  std::size_t limit,
  std::string_view portal)
{
  if (parameters.size() > 65535 || !cstring_valid(name) || !cstring_valid(portal) ||
    (format != Format::text && format != Format::binary))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  std::size_t size = name.size() + portal.size() + 10 + parameters.size() * 6;
  if (size > limit)
    return std::unexpected(make_error_code(Error::resource_limit));

  for (const auto &parameter : parameters) {
    if (parameter.data) {
      if (parameter.format == Format::text && !cstring_valid(*parameter.data))
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

      if (parameter.data->size() > limit - size)
        return std::unexpected(make_error_code(Error::resource_limit));

      size += parameter.data->size();
    }
  }

  Writer body;
  body.string(portal);
  body.string(name);
  body.integer(static_cast<u32>(parameters.size()), 2);
  for (const auto &parameter : parameters) {
    if (parameter.format != Format::text && parameter.format != Format::binary)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    body.integer(static_cast<u32>(parameter.format), 2);
  }

  body.integer(static_cast<u32>(parameters.size()), 2);
  for (const auto &parameter : parameters) {
    if (!parameter.data) {
      body.integer(std::numeric_limits<u32>::max());
      continue;
    }

    if (parameter.data->size() > static_cast<std::size_t>(std::numeric_limits<i32>::max()))
      return std::unexpected(make_error_code(Error::resource_limit));

    body.integer(static_cast<u32>(parameter.data->size()));
    body.raw(*parameter.data);
  }

  body.integer(1, 2);
  body.integer(static_cast<u32>(format), 2);
  request.message('B', body);
  return {};
}

Result<Writer> function_call(u32 function, const std::vector<Parameter> &parameters, Format format, std::size_t limit)
{
  if (function == 0 || parameters.size() > 65535 || (format != Format::text && format != Format::binary))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  std::size_t size = 15;
  if (size > limit || parameters.size() > (limit - size) / 6)
    return std::unexpected(make_error_code(Error::resource_limit));
  size += parameters.size() * 6;

  for (const auto &parameter : parameters) {
    bool invalid_format = parameter.format != Format::text && parameter.format != Format::binary;
    bool invalid_text = parameter.data && parameter.format == Format::text && !cstring_valid(*parameter.data);
    if (parameter.type != 0 || invalid_format || invalid_text)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    if (!parameter.data)
      continue;
    if (parameter.data->size() > std::numeric_limits<i32>::max() || parameter.data->size() > limit - size)
      return std::unexpected(make_error_code(Error::resource_limit));
    size += parameter.data->size();
  }

  Writer body;
  body.integer(function);
  body.integer(static_cast<u32>(parameters.size()), 2);
  for (const auto &parameter : parameters)
    body.integer(static_cast<u16>(parameter.format), 2);
  body.integer(static_cast<u32>(parameters.size()), 2);
  for (const auto &parameter : parameters) {
    body.integer(parameter.data ? static_cast<u32>(parameter.data->size()) : 0xffffffff);
    if (parameter.data)
      body.raw(*parameter.data);
  }
  body.integer(static_cast<u16>(format), 2);

  Writer request;
  request.message('F', body);
  return request;
}

} // namespace detail
} // namespace weave::pg
