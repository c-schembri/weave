#include "result_storage_inspection.hpp"
#include <concepts>
#include <format>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#if defined(_WIN32)
#include <crtdbg.h>
#endif

using Text = weave::pg::detail::ResultText;
using memory_test::check;

template <class T>
concept MixedConcatenation = requires(const T &text, const std::string &string) {
  text + string;
  string + text;
};

template <class T>
concept OwnsSubstring = std::convertible_to<decltype(std::declval<const T &>().substr(0)), std::string>;

static_assert(MixedConcatenation<Text>);
static_assert(OwnsSubstring<Text>);
static_assert(std::same_as<decltype(std::declval<Text>().substr(0)), std::string>);
static_assert(std::same_as<decltype(std::declval<Text>() + std::declval<Text>()), std::string>);
static_assert(!std::convertible_to<const Text &, const std::string &>);
static_assert(!std::convertible_to<const Text &, std::string>);
using List = weave::pg::detail::ResultList<int>;
static_assert(!std::convertible_to<const List &, const std::vector<int> &>);
static_assert(!std::convertible_to<const List &, std::vector<int>>);

static void consume(std::string value)
{
  check(value == "payload");
}

int main()
{
#if defined(_WIN32)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  Text text{"payload"};
  check(std::format("{}", text) == "payload");
  check(std::format("{:>9.4}", text) == "     payl");
  check(std::hash<Text>{}(text) == std::hash<std::string_view>{}("payload"));
  consume(std::string{text});
  consume(text.substr(0));
  consume(std::string{text.substr(0)});
  check(std::string_view{text.substr(0, 3)} == "pay");
  check(std::string{text + "!"} == "payload!");
  check(std::string{"!" + text} == "!payload");
  check(text + std::string{"!"} == "payload!");
  check(std::string{"!"} + text == "!payload");
  check(text + std::string_view{"!"} == "payload!");
  check(std::string_view{"!"} + text == "!payload");
  check(text + Text{"!"} == "payload!");
  check(text + '!' == "payload!");
  check('!' + text == "!payload");
  const Text binary{std::string_view{"a\0b", 3}};
  check(binary.substr(1) == std::string("\0b", 2));
  check(binary + std::string_view{"\0c", 2} == std::string("a\0b\0c", 5));
  check(std::string_view{"\0c", 2} + binary == std::string("\0ca\0b", 5));
  check(binary + '\0' == std::string("a\0b\0", 4));
  check('\0' + binary == std::string("\0a\0b", 4));
  check(text.substr(text.size()).empty());
  std::string escaped;
  {
    Text source{std::string(100, 'x')};
    const auto before = source.get_allocator().arena().bytes();
    escaped = source.substr(1) + source;
    check(source.get_allocator().arena().bytes() == before);
  }
  check(escaped == std::string(199, 'x'));
  const std::string owned{text};
  check(owned == text);
  std::optional<std::string> optional{std::string{text}};
  check(optional && *optional == "payload");
  std::unordered_map<Text, unsigned> values;
  values.emplace(text, 1);
  check(values.at(Text{"payload"}) == 1);
  const List list{1, 2, 3};
  auto copied = static_cast<std::vector<int>>(list);
  check(copied.size() == 3 && copied.front() == 1);
  copied.front() = 9;
  check(list.front() == 1);
  check(std::span<const int>{list}.size() == 3);
  std::printf(
    "Text boundary: mixed_concat=%u implicit_substring=%u exact_string_reference=%u\n",
    static_cast<unsigned>(MixedConcatenation<Text>),
    static_cast<unsigned>(OwnsSubstring<Text>),
    static_cast<unsigned>(std::same_as<Text, std::string>));
  std::printf("Reduced result contract: %u checks\n", memory_test::checks.load());
}
