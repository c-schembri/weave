#pragma once

#include <weave/mysql/row.hpp>
#include <weave/database/record.hpp>

namespace weave::mysql {

template <class T>
Result<T> Row::as() const &
{
  return db::decode<T>(*this);
}

} // namespace weave::mysql
