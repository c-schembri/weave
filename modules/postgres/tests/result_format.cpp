#include <weave/postgres.hpp>
#include <doctest/doctest.h>
#include <array>

namespace pg = weave::pg;

static pg::ResultSet display_result()
{
  pg::ResultSet result;
  result.kind = pg::ResultKind::tuples;
  result.columns.push_back({.name = "name"});
  result.columns.push_back({.name = "value"});
  result.rows.push_back(pg::Row{{{"A<&\""}}, {{"42"}}});
  result.rows.push_back(pg::Row{{std::nullopt}, {{std::string{"\0\xff", 2}}, pg::Format::binary}});
  return result;
}

TEST_CASE("Result display owns bounded aligned, delimited, HTML and expanded output")
{
  const auto result = display_result();
  auto table = result.format({.null_text = "NULL"});
  REQUIRE(table);
  if (!table)
    return;

  CHECK(*table == "name | value \n-----+-------\nA<&\" | 42    \nNULL | \\x00ff\n(2 rows)\n");
  CHECK(result.format({.null_text = "NULL", .output_bytes = table->size()}) == *table);
  CHECK_FALSE(result.format({.null_text = "NULL", .output_bytes = table->size() - 1}));

  auto html = result.format({.layout = pg::ResultLayout::html, .caption = "<caption>"});
  REQUIRE(html);
  if (!html)
    return;
  CHECK(html->find("A&lt;&amp;&quot;") != std::string::npos);
  CHECK(html->find("<caption>&lt;caption&gt;</caption>") != std::string::npos);
  CHECK(html->find("<p>(2 rows)</p>") != std::string::npos);

  auto csv = result.format({.layout = pg::ResultLayout::delimited, .row_count = false, .separator = ","});
  REQUIRE(csv);
  if (!csv)
    return;
  CHECK(*csv == "name,value\n\"A<&\"\"\",42\n,\\x00ff\n");

  auto expanded = result.format({.expanded = true});
  REQUIRE(expanded);
  if (!expanded)
    return;
  CHECK(expanded->find("-[ RECORD 1 ]-\nname  | A<&\"\nvalue | 42\n") != std::string::npos);

  const std::array layouts{pg::ResultLayout::table, pg::ResultLayout::delimited, pg::ResultLayout::html};
  for (auto layout : layouts) {
    for (std::size_t limit = 0; limit < 150; ++limit) {
      auto output = result.format({.layout = layout, .output_bytes = limit});
      if (output)
        CHECK(output->size() <= limit);
      else
        CHECK(output.error() == pg::make_error_code(pg::Error::resource_limit));
    }
  }
  auto empty = pg::ResultSet{}.format({.headers = false, .row_count = false, .output_bytes = 0});
  CHECK(empty == std::string{});
}

TEST_CASE("Result display rejects malformed shapes and escapes terminal control bytes")
{
  auto result = display_result();
  const std::array<std::string_view, 1> wrong_headings{"wrong"};
  CHECK_FALSE(result.format({.headings = wrong_headings}));
  CHECK_FALSE(result.format({.layout = pg::ResultLayout::html, .expanded = true}));
  CHECK_FALSE(result.format({.separator = "\n"}));
  CHECK(result.format({.input_bytes = 1}).error() == pg::make_error_code(pg::Error::resource_limit));

  result.rows[0][0].data = std::string{"bad\n\x1btext"};
  auto escaped = result.format();
  REQUIRE(escaped);
  if (!escaped)
    return;
  CHECK(escaped->find("bad\\x0a\\x1btext") != std::string::npos);

  result.rows[0][0].data = std::string{"\xff"};
  CHECK_FALSE(result.format());
  result.rows[0].pop_back();
  CHECK(result.format().error() == std::errc::invalid_argument);
}

TEST_CASE("Application failure outcomes own diagnostics without fabricating successful results")
{
  pg::Diagnostic diagnostic{{{'M', "Application rejected the row"}, {'C', "22000"}}};
  auto failure = pg::Outcome::failure(diagnostic);
  REQUIRE(failure);
  if (!failure)
    return;

  CHECK_FALSE(failure->result);
  CHECK_FALSE(failure->aborted);
  CHECK(failure->error.message() == "Application rejected the row");
  CHECK(pg::status_name(*failure) == "sql_error");
  diagnostic.fields[0].second = "changed";
  CHECK(failure->error.message() == "Application rejected the row");
  CHECK_FALSE(pg::Outcome::failure({}));
  CHECK_FALSE(pg::Outcome::failure({{{'M', "text"}, {'M', "duplicate"}}}));
  CHECK_FALSE(pg::Outcome::failure({{{'M', "text"}, {'C', "00000"}}}));
  CHECK_FALSE(pg::Outcome::failure({{{'M', "text"}}}, 4));
  CHECK(pg::Outcome::failure({{{'M', "text"}}}, 5).has_value());
}
