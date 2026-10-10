#include <weave/postgres/connection.hpp>
#include <algorithm>
#include <limits>

namespace weave::pg {

namespace {

using DiagnosticFields = std::array<std::optional<std::string_view>, 256>;

class DiagnosticText {
  std::string text_;
  std::size_t limit_;
  bool full_ = false;

public:
  explicit DiagnosticText(std::size_t limit) : limit_(std::min(limit, text_.max_size()))
  {
  }

  void append(std::string_view text)
  {
    if (full_)
      return;
    if (text.size() > limit_ - text_.size()) {
      full_ = true;
      return;
    }
    text_.append(text);
  }

  void spaces(std::size_t count)
  {
    if (full_)
      return;
    if (count > limit_ - text_.size()) {
      full_ = true;
      return;
    }
    text_.append(count, ' ');
  }

  std::size_t number(std::size_t value)
  {
    std::array<char, std::numeric_limits<std::size_t>::digits10 + 1> digits;
    auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    auto length = static_cast<std::size_t>(converted.ptr - digits.data());
    append({digits.data(), length});
    return length;
  }

  void field(std::string_view label, const std::optional<std::string_view> &value)
  {
    if (value) {
      append(label);
      append(*value);
      append("\n");
    }
  }

  Result<std::string> finish()
  {
    if (full_)
      return std::unexpected(make_error_code(Error::resource_limit));
    return std::move(text_);
  }

  std::size_t remaining() const noexcept
  {
    return full_ ? 0 : limit_ - text_.size();
  }
};

void quoted(DiagnosticText &output, std::string_view value)
{
  constexpr std::string_view digits = "0123456789abcdef";
  output.append("\"");
  for (auto character : value) {
    const auto byte = static_cast<u8>(character);
    if (byte == '\\' || byte == '"') {
      output.append("\\");
      output.append({&character, 1});
    } else if (byte < 32 || byte >= 127) {
      const std::array escaped{'\\', 'x', digits[byte >> 4], digits[byte & 15]};
      output.append({escaped.data(), escaped.size()});
    } else {
      output.append({&character, 1});
    }
  }
  output.append("\"");
}

Result<std::string_view> stage_name(ConnectionStage stage)
{
  switch (stage) {
  case ConnectionStage::validation:
    return "validation";
  case ConnectionStage::resolution:
    return "resolution";
  case ConnectionStage::transport:
    return "transport";
  case ConnectionStage::socket_options:
    return "socket options";
  case ConnectionStage::peer_identity:
    return "peer identity";
  case ConnectionStage::gss:
    return "GSS negotiation";
  case ConnectionStage::tls:
    return "TLS negotiation";
  case ConnectionStage::startup:
    return "startup";
  case ConnectionStage::authentication:
    return "authentication";
  case ConnectionStage::target_session:
    return "target session";
  case ConnectionStage::oauth:
    return "OAuth acquisition";
  }
  return std::unexpected(std::make_error_code(std::errc::invalid_argument));
}

void error_text(DiagnosticText &output, std::error_code error)
{
  quoted(output, error.category().name());
  output.append(":");
  auto value = static_cast<i64>(error.value());
  if (value < 0) {
    output.append("-");
    value = -value;
  }
  output.number(static_cast<std::size_t>(value));
  output.append(" ");
  quoted(output, error.message());
  output.append("\n");
}

Result<void> validate_options(const DiagnosticFormat &options)
{
  if (options.verbosity < DiagnosticVerbosity::terse || options.verbosity > DiagnosticVerbosity::sqlstate ||
    options.context < DiagnosticContext::never || options.context > DiagnosticContext::always) {
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }
  auto info = encoding_info(options.encoding);
  if (!info)
    return std::unexpected(info.error());
  return {};
}

Result<std::size_t> position(const std::optional<std::string_view> &field)
{
  if (!field)
    return std::size_t{0};
  if (field->empty())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  std::size_t number = 0;
  auto parsed = std::from_chars(field->data(), field->data() + field->size(), number);
  if (parsed.ec != std::errc{} || parsed.ptr != field->data() + field->size() || !number)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  return number;
}

Result<DiagnosticFields> diagnostic_fields(const Diagnostic &diagnostic, const DiagnosticFormat &options)
{
  if (diagnostic.fields.size() > 255)
    return std::unexpected(make_error_code(Error::resource_limit));

  DiagnosticFields fields{};
  std::size_t bytes = 0;
  for (const auto &[code, value] : diagnostic.fields) {
    auto index = static_cast<u8>(code);
    if (!index || fields[index])
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    if (value.size() > options.input_bytes - bytes)
      return std::unexpected(make_error_code(Error::resource_limit));

    bytes += value.size();
    fields[index] = value;
  }
  if (options.query && options.query->size() > options.input_bytes - bytes)
    return std::unexpected(make_error_code(Error::resource_limit));

  for (const auto &[code, value] : diagnostic.fields) {
    if (auto valid = validate_text(value, options.encoding); !valid)
      return std::unexpected(valid.error());
  }
  if (options.query) {
    if (auto valid = validate_text(*options.query, options.encoding); !valid)
      return std::unexpected(valid.error());
  }
  if (fields['C']) {
    const auto state = *fields['C'];
    if (state.size() != 5 || !std::all_of(state.begin(), state.end(), [](char byte) {
          return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z');
        })) {
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
  }
  const std::array positions{'P', 'p'};
  for (auto code : positions) {
    if (auto parsed = position(fields[code]); !parsed)
      return std::unexpected(parsed.error());
  }
  return fields;
}

struct DisplayCharacter {
  std::size_t bytes;
  std::size_t width;
};

Result<DisplayCharacter> display_character(std::string_view text, Encoding encoding)
{
  auto bytes = character_size(text, encoding);
  if (!bytes)
    return std::unexpected(bytes.error());
  auto width = character_width(text, encoding);
  if (!width)
    return std::unexpected(width.error());
  return DisplayCharacter{*bytes, static_cast<std::size_t>(std::max(i32{1}, *width))};
}

Result<void> cursor(DiagnosticText &output, std::string_view query, std::size_t location, Encoding encoding)
{
  const auto target = location - 1;
  std::size_t offset = 0;
  std::size_t character = 0;
  std::size_t line = 1;
  std::size_t start = 0;
  std::size_t line_width = 0;
  std::size_t cursor_width = 0;
  bool previous_cr = false;

  // Locate the line and cursor without allocating an index for the whole query.
  while (offset < query.size()) {
    auto glyph = display_character(query.substr(offset), encoding);
    if (!glyph)
      return std::unexpected(glyph.error());
    const auto byte = query[offset];
    const bool newline = glyph->bytes == 1 && (byte == '\r' || byte == '\n');
    if (character == target)
      cursor_width = line_width;

    if (newline) {
      if (character >= target)
        break;
      if (byte == '\r' || !previous_cr)
        ++line;
      start = offset + glyph->bytes;
      line_width = 0;
    } else {
      if (glyph->width > std::numeric_limits<std::size_t>::max() - line_width)
        return std::unexpected(make_error_code(Error::resource_limit));
      line_width += glyph->width;
    }
    previous_cr = newline && byte == '\r';
    offset += glyph->bytes;
    ++character;
  }
  if (target > character)
    return {};
  if (target == character)
    cursor_width = line_width;

  const auto line_end = offset;
  const auto line_start = start;
  auto end = line_end;
  auto end_width = line_width;
  std::size_t start_width = 0;
  if (line_width > 60) {
    std::size_t width_limit = 60;
    if (cursor_width > 50)
      width_limit = line_width - cursor_width <= 10 ? line_width : cursor_width + 10;

    end = start;
    end_width = 0;
    while (end < line_end) {
      auto glyph = display_character(query.substr(end), encoding);
      if (!glyph)
        return std::unexpected(glyph.error());
      if (glyph->width > width_limit - end_width)
        break;
      end += glyph->bytes;
      end_width += glyph->width;
    }
    while (end_width - start_width > 60) {
      auto glyph = display_character(query.substr(start), encoding);
      if (!glyph)
        return std::unexpected(glyph.error());
      start += glyph->bytes;
      start_width += glyph->width;
    }
  }

  output.append("LINE ");
  auto digits = output.number(line);
  output.append(": ");
  const bool left_cut = start != line_start;
  if (left_cut)
    output.append("...");
  for (auto displayed = start; displayed < end;) {
    auto glyph = display_character(query.substr(displayed), encoding);
    if (!glyph)
      return std::unexpected(glyph.error());
    auto text = query.substr(displayed, glyph->bytes);
    output.append(text == "\t" ? std::string_view{" "} : text);
    displayed += glyph->bytes;
  }
  if (end != line_end)
    output.append("...");
  output.append("\n");
  auto prefix_width = 7 + digits + (left_cut ? 3 : 0);
  output.spaces(prefix_width + (cursor_width - start_width));
  output.append("^\n");
  return {};
}

bool error_severity(const DiagnosticFields &fields)
{
  const auto severity = fields['V'] ? fields['V'] : fields['S'];
  return severity && (*severity == "ERROR" || *severity == "FATAL" || *severity == "PANIC");
}

} // namespace

Result<std::string> Diagnostic::format(DiagnosticFormat options) const
{
  if (auto valid = validate_options(options); !valid)
    return std::unexpected(valid.error());
  auto parsed = diagnostic_fields(*this, options);
  if (!parsed)
    return std::unexpected(parsed.error());

  DiagnosticText output{options.output_bytes};
  if (fields.empty())
    return output.finish();
  const auto &values = *parsed;
  if (values['S']) {
    output.append(*values['S']);
    output.append(":  ");
  }
  if (options.verbosity == DiagnosticVerbosity::sqlstate) {
    if (values['C']) {
      output.append(*values['C']);
      output.append("\n");
      return output.finish();
    }
    options.verbosity = DiagnosticVerbosity::terse;
  }
  if (options.verbosity == DiagnosticVerbosity::verbose && values['C']) {
    output.append(*values['C']);
    output.append(": ");
  }
  if (values['M'])
    output.append(*values['M']);

  auto position_field = values['P'] ? values['P'] : values['p'];
  std::optional<std::string_view> query;
  if (values['P'])
    query = options.query;
  else if (values['p'])
    query = values['q'];
  if (position_field && (!query || options.verbosity == DiagnosticVerbosity::terse)) {
    output.append(" at character ");
    output.append(*position_field);
  }
  output.append("\n");

  if (options.verbosity != DiagnosticVerbosity::terse) {
    if (query) {
      auto location = position(position_field);
      if (!location)
        return std::unexpected(location.error());
      if (*location) {
        if (auto displayed = cursor(output, *query, *location, options.encoding); !displayed)
          return std::unexpected(displayed.error());
      }
    }
    output.field("DETAIL:  ", values['D']);
    output.field("HINT:  ", values['H']);
    output.field("QUERY:  ", values['q']);
    if (options.context == DiagnosticContext::always ||
      (options.context == DiagnosticContext::errors && error_severity(values))) {
      output.field("CONTEXT:  ", values['W']);
    }
  }
  if (options.verbosity == DiagnosticVerbosity::verbose) {
    const std::array labels{
      std::pair{'s', std::string_view{"SCHEMA NAME:  "}},
      std::pair{'t', std::string_view{"TABLE NAME:  "}},
      std::pair{'c', std::string_view{"COLUMN NAME:  "}},
      std::pair{'d', std::string_view{"DATATYPE NAME:  "}},
      std::pair{'n', std::string_view{"CONSTRAINT NAME:  "}}};
    for (const auto &[code, label] : labels)
      output.field(label, values[code]);

    if (values['R'] || values['F'] || values['L']) {
      output.append("LOCATION:  ");
      if (values['R']) {
        output.append(*values['R']);
        output.append(", ");
      }
      if (values['F'] && values['L']) {
        output.append(*values['F']);
        output.append(":");
        output.append(*values['L']);
      }
      output.append("\n");
    }
  }
  return output.finish();
}

Result<std::string> Failure::format(DiagnosticFormat options) const
{
  if (auto valid = validate_options(options); !valid)
    return std::unexpected(valid.error());
  if (!error && !diagnostic.fields.empty())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  DiagnosticText output{options.output_bytes};
  if (error)
    error_text(output, error);
  options.output_bytes = output.remaining();
  auto text = diagnostic.format(options);
  if (!text)
    return std::unexpected(text.error());
  output.append(*text);
  return output.finish();
}

Result<std::string> ConnectionReport::format(DiagnosticFormat options) const
{
  if (auto valid = validate_options(options); !valid)
    return std::unexpected(valid.error());
  if (attempts.size() > max_attempts)
    return std::unexpected(make_error_code(Error::resource_limit));

  std::size_t remaining = options.input_bytes;
  auto charge = [&](std::size_t bytes) {
    if (bytes > remaining)
      return false;
    remaining -= bytes;
    return true;
  };
  if (options.query && !charge(options.query->size()))
    return std::unexpected(make_error_code(Error::resource_limit));
  for (const auto &attempt : attempts) {
    if (!attempt.error || attempt.elapsed < std::chrono::microseconds::zero())
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    if (!charge(attempt.host.size()))
      return std::unexpected(make_error_code(Error::resource_limit));
    for (const auto &[code, value] : attempt.diagnostic.fields) {
      if (!charge(value.size()))
        return std::unexpected(make_error_code(Error::resource_limit));
    }
  }

  DiagnosticText output{options.output_bytes};
  for (const auto &attempt : attempts) {
    auto stage = stage_name(attempt.stage);
    if (!stage)
      return std::unexpected(stage.error());
    if (!attempt.host.empty()) {
      output.append("Host ");
      quoted(output, attempt.host);
      output.append(":");
      output.number(attempt.port);
      if (attempt.endpoint) {
        output.append(" (");
        output.append(attempt.endpoint->to_string());
        output.append(")");
      }
      output.append(", ");
    }
    output.append(*stage);
    output.append(" after ");
    output.number(attempt.elapsed.count());
    output.append(" us");
    output.append(": ");
    error_text(output, attempt.error);

    auto policy = options;
    policy.output_bytes = output.remaining();
    auto diagnostic = attempt.diagnostic.format(policy);
    if (!diagnostic)
      return std::unexpected(diagnostic.error());
    output.append(*diagnostic);
    if (attempt.diagnostic_truncated)
      output.append("Server diagnostic omitted by capture limit.\n");
  }
  if (completed || error) {
    if (error) {
      output.append("Connection failed: ");
      error_text(output, error);
    } else {
      output.append("Connection established.\n");
    }
  }
  if (truncated)
    output.append("Connection history truncated.\n");
  return output.finish();
}

} // namespace weave::pg
