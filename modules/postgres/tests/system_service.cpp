#include <weave/postgres.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace pg = weave::pg;
static int checks = 0;

static void check(bool condition)
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "System service check failed: %d\n", checks);
    std::exit(1);
  }
}

static std::string text(const std::filesystem::path &path)
{
  auto bytes = path.u8string();
  return {bytes.begin(), bytes.end()};
}

static void environment(const char *key, const std::string &value)
{
#ifdef _WIN32
  std::string_view name{key};
  std::wstring wide_name{name.begin(), name.end()};
  std::u8string bytes{value.begin(), value.end()};
  auto wide_value = std::filesystem::path{bytes}.native();
  check(_wputenv_s(wide_name.c_str(), wide_value.c_str()) == 0);
#else
  check(setenv(key, value.c_str(), 1) == 0);
#endif
}

static void write(const std::filesystem::path &path, std::string_view contents)
{
  std::ofstream output(path, std::ios::binary);
  output << contents;
  output.close();
  check(static_cast<bool>(output));
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
#else
int main(int argc, char **argv)
#endif
{
  check(argc == 2);
#ifdef _WIN32
  auto root = std::filesystem::path{argv[1]};
#else
  auto root = std::filesystem::path{argv[1]};
#endif
  auto system = root / "pg_service.conf";
  auto user = root / "user.conf";
  auto override_file = root / "override.conf";
  auto other_directory = root / "override";
  std::error_code error;
  std::filesystem::create_directory(other_directory, error);
  check(!error);
  write(system, "[sample]\nuser=system\ndbname=system_database\n");
  write(override_file, "[sample]\nuser=override\n");
  write(other_directory / "pg_service.conf", "[sample]\nuser=environment\n");
  pg::ConfigSources sources{.environment = false, .user_files = false};

  auto loaded = pg::Options::load("service=sample", sources);
  check(loaded && loaded->user == "system" && loaded->database == "system_database");
  loaded = pg::Options::load("service=sample user=explicit", sources);
  check(loaded && loaded->user == "explicit");
  loaded = pg::Options::load("user=explicit", sources);
  check(loaded && loaded->user == "explicit");
  auto parsed = pg::Options::parse("service=sample");
  check(!parsed && parsed.error() == std::errc::operation_not_supported);

  sources.system_files = false;
  loaded = pg::Options::load("service=sample", sources);
  check(!loaded && loaded.error() == std::errc::no_such_file_or_directory);
  sources.system_service_file = text(override_file);
  loaded = pg::Options::load("service=sample", sources);
  check(loaded && loaded->user == "override");
  environment("PGSYSCONFDIR", text(other_directory));
  sources.environment = true;
  loaded = pg::Options::load("service=sample", sources);
  check(loaded && loaded->user == "override");
  sources.system_service_file.clear();
  loaded = pg::Options::load("service=sample", sources);
  check(loaded && loaded->user == "environment");
  sources.environment = false;
  sources.system_files = true;
  loaded = pg::Options::load("service=sample", sources);
  check(loaded && loaded->user == "system");

  sources.service_file = text(user);
  write(user, "[sample]\nuser=user\n");
  loaded = pg::Options::load("service=sample", sources);
  check(loaded && loaded->user == "user" && loaded->database != "system_database");
  write(user, "[other]\nuser=other\n");
  loaded = pg::Options::load("service=sample", sources);
  check(loaded && loaded->user == "system");
  write(user, "[sample]\nunsupported=value\n");
  check(!pg::Options::load("service=sample", sources));
  sources.service_file.clear();

  write(system, "[sample]\nunsupported=value\n");
  check(!pg::Options::load("service=sample", sources));
  loaded = pg::Options::load("user=explicit", sources);
  check(loaded && loaded->user == "explicit");
  std::filesystem::remove(system, error);
  check(!error);
  loaded = pg::Options::load("service=sample", sources);
  check(!loaded && loaded.error() == std::errc::no_such_file_or_directory);
  sources.service_file = text(user);
  write(user, "[sample]\nuser=user\n");
  loaded = pg::Options::load("service=sample", sources);
  check(loaded && loaded->user == "user");
  sources.service_file.clear();
  std::filesystem::create_directory(system, error);
  check(!error);
  check(!pg::Options::load("service=sample", sources));
  std::filesystem::remove(system, error);
  check(!error);
  write(system, "[other]\nuser=other\n");
  check(!pg::Options::load("service=sample", sources));
  write(system, "[sample]\nuser=system\n");
  loaded = pg::Options::load("service=sample", sources);
  check(loaded && loaded->user == "system");
  std::printf("System service controls passed: %d checks\n", checks);
}
