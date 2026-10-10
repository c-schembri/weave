#include <weave/postgres/password.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace pg = weave::pg;
static unsigned checks = 0;

static void check(bool condition, std::source_location location = std::source_location::current())
{
  if (!condition) {
    std::fprintf(stderr, "Password verifier check failed at line %u\n", location.line());
    std::exit(1);
  }
  ++checks;
}

int main()
{
  const std::array passwords{
    std::string{"pencil"},
    std::string{},
    std::string{"\xc2\xaa"},
    std::string{"I\xc2\xadX"},
    std::string{"\a"},
    std::string{"\xff"},
    std::string{"\xd8\xa7x"},
    std::string{"\xc8\xa1"},
    std::string{"O'Reilly\\secret\n"}};
  for (std::size_t index = 0; index < passwords.size(); ++index) {
    auto scram = pg::password_verifier("user", passwords[index]);
    auto md5 = pg::password_verifier("user", passwords[index], pg::PasswordAlgorithm::md5);
    check(scram && md5);
    std::printf("{\"case\":%zu,\"scram\":\"%s\",\"md5\":\"%s\"}\n", index, scram->c_str(), md5->c_str());
  }
  check(!pg::password_verifier(std::string_view{"x\0y", 3}, "pencil"));
  check(!pg::password_verifier("user", std::string_view{"x\0y", 3}));
  check(!pg::password_verifier(std::string(65537, 'x'), "pencil"));
  check(!pg::password_verifier("user", std::string(65537, 'x')));
  check(!pg::password_verifier("user", "pencil", static_cast<pg::PasswordAlgorithm>(100)));
  check(!pg::password_verifier("user", "pencil", pg::PasswordAlgorithm::scram_sha256, 0));
  check(!pg::password_verifier("user", "pencil", pg::PasswordAlgorithm::scram_sha256, 1000001));
  auto first = pg::password_verifier("user", "pencil");
  auto second = pg::password_verifier("user", "pencil");
  check(first && second && *first != *second);
  auto custom = pg::password_verifier("user", "pencil", pg::PasswordAlgorithm::scram_sha256, 8192);
  check(custom && custom->starts_with("SCRAM-SHA-256$8192:"));
  std::printf("{\"custom\":\"%s\",\"checks\":%u}\n", custom->c_str(), checks);
}
