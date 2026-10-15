#include <weave/postgres/record.hpp>

struct Record {
#if WEAVE_RECORD_CONTRACT_CASE == 4
  std::vector<int> id;
#elif WEAVE_RECORD_CONTRACT_CASE == 5
  const int id = 0;
#else
  int id = 0;
#endif

#if WEAVE_RECORD_CONTRACT_CASE == 6
  Record() noexcept
  {
  }
#endif
};

#if WEAVE_RECORD_CONTRACT_CASE == 1
template <>
struct weave::pg::RowTraits<Record> {
  static constexpr auto fields = std::tuple{weave::pg::field("id", static_cast<int Record::*>(nullptr))};
};
#elif WEAVE_RECORD_CONTRACT_CASE == 2
struct Other {
  int id;
};

template <>
struct weave::pg::RowTraits<Record> {
  static constexpr auto fields = std::tuple{weave::pg::field("id", &Other::id)};
};
#elif WEAVE_RECORD_CONTRACT_CASE == 3
template <>
struct weave::pg::RowTraits<Record> {
  static constexpr auto fields = std::tuple{42};
};
#endif

void check_record(const weave::pg::ResultSet &result, const weave::pg::Row &row)
{
#if WEAVE_RECORD_CONTRACT_CASE == 7
  auto record = weave::pg::Row{}.as<Record>();
#else
  auto decoder = result.decoder<Record>();
#if WEAVE_RECORD_CONTRACT_CASE == 8
  if (decoder)
    auto record = decoder->decode(weave::pg::Row{});
#elif WEAVE_RECORD_CONTRACT_CASE == 0
  auto record = row.as<Record>();
  if (decoder)
    auto cached = decoder->decode(row);
#endif
#endif
}
