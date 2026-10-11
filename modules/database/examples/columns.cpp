#include <weave/database/selection.hpp>
#include <weave/log.hpp>
#include <array>
#include <cstdio>

struct Column {
  std::string_view name;
};

int main()
{
  const std::array columns{Column{"recipient"}, Column{"id"}, Column{"weight"}};
  const std::array<std::string_view, 2> names{"id", "recipient"};

  auto indices = weave::db::bind_columns(names, [&](std::string_view name) -> weave::Result<std::size_t> {
    auto column = weave::db::find_column(columns, name);
    if (column)
      return *column;

    const bool missing = column.error() == weave::db::ColumnError::missing;
    auto error = missing ? std::errc::no_such_file_or_directory : std::errc::invalid_argument;
    return std::unexpected(std::make_error_code(error));
  });
  if (!indices)
    return weave::report_error(indices.error());

  std::printf("id: column %zu, recipient: column %zu\n", (*indices)[0], (*indices)[1]);
}
