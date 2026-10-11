#include <weave/database/record.hpp>
#include <weave/log.hpp>
#include <cstdio>
#include <type_traits>

struct Item {
  int id;
};

// A minimal in-memory adapter; actual drivers provide their own field codecs.
struct Field {
  int value;
  bool found;

  template <class T>
  weave::Result<T> as() const
  {
    static_assert(std::is_same_v<T, int>);
    if (!found)
      return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    return value;
  }
};

struct Row {
  int id;

  Field operator[](std::string_view name) const
  {
    return {id, name == "id"};
  }
};

int main()
{
  const Row row{42};
  auto item = weave::db::decode<Item>(row);
  if (!item)
    return weave::report_error(item.error());

  std::printf("Item %d\n", item->id);
}
