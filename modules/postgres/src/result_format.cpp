#include <weave/postgres/connection.hpp>
#include <algorithm>
#include <limits>

namespace weave::pg {

namespace {

class DisplayText {
  std::string text_;
  std::size_t limit_;
  bool full_ = false;

public:
  explicit DisplayText(std::size_t limit) : limit_(limit)
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

  void repeat(char character, std::size_t count)
  {
    if (full_)
      return;
    if (count > limit_ - text_.size()) {
      full_ = true;
      return;
    }
    text_.append(count, character);
  }

  void number(std::size_t value)
  {
    std::array<char, std::numeric_limits<std::size_t>::digits10 + 1> digits;
    auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    append({digits.data(), static_cast<std::size_t>(converted.ptr - digits.data())});
  }

  bool full() const noexcept
  {
    return full_;
  }

  Result<std::string> finish()
  {
    if (full_)
      return std::unexpected(make_error_code(Error::resource_limit));
    return std::move(text_);
  }
};

struct DisplayCell {
  std::string text;
  std::size_t width = 0;
};

Result<DisplayCell> cell(std::string_view input, Format format, const ResultFormat &options)
{
  DisplayCell value;
  constexpr std::string_view digits = "0123456789abcdef";
  const auto escaped_byte = [&](u8 byte) {
    value.text.append("\\x");
    value.text.push_back(digits[byte >> 4]);
    value.text.push_back(digits[byte & 15]);
    value.width += 4;
  };

  if (format == Format::binary) {
    if (options.output_bytes < 2 || input.size() > (options.output_bytes - 2) / 2)
      return std::unexpected(make_error_code(Error::resource_limit));
    value.text = "\\x";
    for (auto byte : input) {
      value.text.push_back(digits[static_cast<u8>(byte) >> 4]);
      value.text.push_back(digits[static_cast<u8>(byte) & 15]);
    }
    value.width = value.text.size();
    return value;
  }
  if (format != Format::text)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  while (!input.empty()) {
    const auto byte = static_cast<u8>(input.front());
    if (byte < 32 || byte == 127) {
      if (options.output_bytes - value.text.size() < 4)
        return std::unexpected(make_error_code(Error::resource_limit));
      escaped_byte(byte);
      input.remove_prefix(1);
      continue;
    }
    auto size = character_size(input, options.encoding);
    auto width = character_width(input, options.encoding);
    if (!size || !width)
      return std::unexpected(!size ? size.error() : width.error());
    if (*width < 0) {
      if (*size > (options.output_bytes - value.text.size()) / 4)
        return std::unexpected(make_error_code(Error::resource_limit));
      for (auto character : input.substr(0, *size))
        escaped_byte(static_cast<u8>(character));
    } else {
      if (*size > options.output_bytes - value.text.size())
        return std::unexpected(make_error_code(Error::resource_limit));
      value.text.append(input.substr(0, *size));
      value.width += static_cast<std::size_t>(*width);
    }
    input.remove_prefix(*size);
  }
  return value;
}

void html(DisplayText &output, std::string_view text)
{
  for (auto character : text) {
    switch (character) {
    case '&':
      output.append("&amp;");
      break;
    case '<':
      output.append("&lt;");
      break;
    case '>':
      output.append("&gt;");
      break;
    case '"':
      output.append("&quot;");
      break;
    case '\'':
      output.append("&#39;");
      break;
    default:
      output.append({&character, 1});
      break;
    }
  }
}

void delimited(DisplayText &output, std::string_view text, std::string_view separator)
{
  const bool quote = text.find(separator) != std::string_view::npos || text.find('"') != std::string_view::npos;
  if (quote)
    output.append("\"");
  for (auto character : text) {
    output.append({&character, 1});
    if (character == '"')
      output.append("\"");
  }
  if (quote)
    output.append("\"");
}

} // namespace

Result<std::string> ResultSet::format(ResultFormat options) const
{
  constexpr std::size_t maximum = 64 * 1024 * 1024;
  if (options.layout < ResultLayout::table || options.layout > ResultLayout::html || options.input_bytes > maximum ||
    options.output_bytes > maximum || (!options.headings.empty() && options.headings.size() != columns.size()) ||
    (options.expanded && options.layout != ResultLayout::table) || options.separator.empty())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if (auto encoding = encoding_info(options.encoding); !encoding)
    return std::unexpected(encoding.error());
  if (columns.size() > options.output_bytes / sizeof(std::size_t))
    return std::unexpected(make_error_code(Error::resource_limit));

  std::size_t input_bytes = 0;
  const auto charge = [&](std::string_view text) {
    if (input_bytes == options.input_bytes || text.size() > options.input_bytes - input_bytes - 1)
      return false;
    input_bytes += text.size() + 1;
    return true;
  };
  const auto heading = [&](std::size_t index) -> std::string_view {
    return options.headings.empty() ? std::string_view{columns[index].name} : options.headings[index];
  };
  if (!charge(options.separator) || !charge(options.null_text) || !charge(options.caption))
    return std::unexpected(make_error_code(Error::resource_limit));
  if (options.separator.find_first_of("\r\n\0", 0, 3) != std::string_view::npos)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto caption = cell(options.caption, Format::text, options);
  auto null = cell(options.null_text, Format::text, options);
  if (!caption || !null)
    return std::unexpected(!caption ? caption.error() : null.error());

  // Two bounded passes avoid retaining a second copy of the entire result payload.
  std::vector<std::size_t> widths(columns.size());
  std::size_t label_width = 0;
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (!charge(heading(index)))
      return std::unexpected(make_error_code(Error::resource_limit));
    auto label = cell(heading(index), Format::text, options);
    if (!label)
      return std::unexpected(label.error());
    label_width = std::max(label_width, label->width);
    if (options.headers)
      widths[index] = label->width;
  }
  for (const auto &row : rows) {
    if (row.size() != columns.size())
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    if (!charge({}))
      return std::unexpected(make_error_code(Error::resource_limit));
    for (std::size_t index = 0; index < row.size(); ++index) {
      if (!charge(row[index].bytes()))
        return std::unexpected(make_error_code(Error::resource_limit));
      if (row[index].is_null()) {
        widths[index] = std::max(widths[index], null->width);
      } else {
        auto value = cell(row[index].bytes(), row[index].format, options);
        if (!value)
          return std::unexpected(value.error());
        widths[index] = std::max(widths[index], value->width);
      }
    }
  }

  DisplayText output{options.output_bytes};
  if (options.layout == ResultLayout::html) {
    output.append("<table>\n");
    if (!caption->text.empty()) {
      output.append("<caption>");
      html(output, caption->text);
      output.append("</caption>\n");
    }
  } else if (!caption->text.empty()) {
    output.append(caption->text);
    output.append("\n");
  }

  const auto emit = [&](const DisplayCell &value, std::size_t index, bool header) {
    if (options.layout == ResultLayout::html) {
      output.append(header ? "<th>" : "<td>");
      html(output, value.text);
      output.append(header ? "</th>" : "</td>");
    } else if (options.layout == ResultLayout::delimited) {
      if (index)
        output.append(options.separator);
      delimited(output, value.text, options.separator);
    } else {
      if (index)
        output.append(" | ");
      output.append(value.text);
      output.repeat(' ', widths[index] - value.width);
    }
  };

  if (options.headers && !options.expanded && !columns.empty()) {
    if (options.layout == ResultLayout::html)
      output.append("<thead><tr>");
    for (std::size_t index = 0; index < columns.size(); ++index) {
      if (output.full())
        return output.finish();
      auto label = cell(heading(index), Format::text, options);
      if (!label)
        return std::unexpected(label.error());
      emit(*label, index, true);
    }
    output.append(options.layout == ResultLayout::html ? "</tr></thead>\n" : "\n");
    if (options.layout == ResultLayout::table) {
      for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index)
          output.append("-+-");
        output.repeat('-', widths[index]);
      }
      output.append("\n");
    }
  }
  if (options.layout == ResultLayout::html)
    output.append("<tbody>\n");

  for (std::size_t record = 0; record < rows.size(); ++record) {
    if (output.full())
      return output.finish();
    if (options.expanded) {
      output.append("-[ RECORD ");
      output.number(record + 1);
      output.append(" ]-\n");
    } else if (options.layout == ResultLayout::html) {
      output.append("<tr>");
    }
    for (std::size_t index = 0; index < columns.size(); ++index) {
      if (output.full())
        return output.finish();
      const auto &item = rows[record][index];
      std::optional<DisplayCell> value;
      if (!item.is_null()) {
        auto rendered = cell(item.bytes(), item.format, options);
        if (!rendered)
          return std::unexpected(rendered.error());
        value = std::move(*rendered);
      }
      const auto &display = value ? *value : *null;
      if (options.expanded) {
        if (options.headers) {
          auto label = cell(heading(index), Format::text, options);
          if (!label)
            return std::unexpected(label.error());
          output.append(label->text);
          output.repeat(' ', label_width - label->width);
          output.append(" | ");
        }
        output.append(display.text);
        output.append("\n");
      } else {
        emit(display, index, false);
      }
    }
    if (!options.expanded)
      output.append(options.layout == ResultLayout::html ? "</tr>\n" : "\n");
  }
  if (options.layout == ResultLayout::html)
    output.append("</tbody>\n</table>\n");
  if (options.row_count) {
    if (options.layout == ResultLayout::html)
      output.append("<p>");
    output.append("(");
    output.number(rows.size());
    output.append(rows.size() == 1 ? " row)" : " rows)");
    output.append(options.layout == ResultLayout::html ? "</p>\n" : "\n");
  }
  return output.finish();
}

} // namespace weave::pg
