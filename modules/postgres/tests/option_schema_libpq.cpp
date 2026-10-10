#include <libpq-fe.h>
#include <cstdio>

int main()
{
  auto version = PQlibVersion();
  if (version != WEAVE_POSTGRES_TEST_LIBPQ_VERSION)
    return 1;
  auto options = PQconndefaults();
  if (!options)
    return 1;

  std::printf("version\t%d\n", version);
  for (auto option = options; option->keyword; ++option) {
    std::printf(
      "option\t%s\t%s\t%s\t%s\n",
      option->keyword,
      option->envvar ? option->envvar : "",
      option->compiled ? option->compiled : "\\N",
      option->dispchar ? option->dispchar : "");
  }
  PQconninfoFree(options);
}
