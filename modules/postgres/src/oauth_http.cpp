#include "oauth_http.hpp"
#include "oauth_llhttp_prefix.h"
#include "vendor/llhttp/include/llhttp.h"
#include <weave/postgres/connection.hpp>
#include <weave/tcp.hpp>
#include <weave/tls.hpp>
#include <weave/timer.hpp>
#include <algorithm>
#include <charconv>

namespace weave::pg::detail {

namespace {

constexpr std::size_t maximum_headers = 32768;
constexpr std::size_t maximum_body = 65536;
constexpr std::size_t maximum_wire = 256 * 1024;
constexpr std::size_t maximum_request = 512 * 1024;

unsigned char lowercase(unsigned char byte) noexcept
{
  return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
}

bool equal_ascii(std::string_view left, std::string_view right) noexcept
{
  return left.size() == right.size() && std::ranges::equal(left, right, {}, lowercase, lowercase);
}

std::string_view view(const SecretText &text) noexcept
{
  return {text.data(), text.size()};
}

std::string_view trim(std::string_view text) noexcept
{
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    text.remove_suffix(1);
  return text;
}

struct ResponseParser {
  llhttp_t parser{};
  llhttp_settings_t callbacks{};
  OAuthHttpResponse response;
  SecretText field;
  SecretText value;
  std::error_code error;
  std::size_t header_bytes = 0;
  std::size_t header_count = 0;
  unsigned interim = 0;
  bool content_type = false;
  bool content_encoding = false;
  bool transfer_encoding = false;
  bool complete = false;

  ResponseParser()
  {
    llhttp_settings_init(&callbacks);
    callbacks.on_header_field = on_field;
    callbacks.on_header_value = on_value;
    callbacks.on_header_value_complete = on_value_complete;
    callbacks.on_headers_complete = on_headers;
    callbacks.on_body = on_body;
    callbacks.on_message_complete = on_complete;
    llhttp_init(&parser, HTTP_RESPONSE, &callbacks);
    parser.data = this;
  }

  static ResponseParser &self(llhttp_t *parser) noexcept
  {
    return *static_cast<ResponseParser *>(parser->data);
  }

  int reject(Error reason) noexcept
  {
    error = make_error_code(reason);
    return HPE_USER;
  }

  int header_part(SecretText &destination, const char *data, std::size_t size)
  {
    if (size > maximum_headers - header_bytes || size > 8192 - destination.size())
      return reject(Error::resource_limit);
    header_bytes += size;
    destination.insert(destination.end(), data, data + size);
    return HPE_OK;
  }

  static int on_field(llhttp_t *parser, const char *data, std::size_t size)
  {
    auto &state = self(parser);
    if (parser->flags & F_TRAILING)
      return state.reject(Error::protocol);
    return state.header_part(state.field, data, size);
  }

  static int on_value(llhttp_t *parser, const char *data, std::size_t size)
  {
    auto &state = self(parser);
    return state.header_part(state.value, data, size);
  }

  static int on_value_complete(llhttp_t *parser)
  {
    auto &state = self(parser);
    if (++state.header_count > 64)
      return state.reject(Error::resource_limit);
    auto name = view(state.field);
    auto value = trim(view(state.value));
    if (equal_ascii(name, "content-type")) {
      if (state.content_type)
        return state.reject(Error::protocol);
      state.content_type = true;
      auto delimiter = value.find(';');
      if (!equal_ascii(trim(value.substr(0, delimiter)), "application/json"))
        return state.reject(Error::protocol);
      if (delimiter != std::string_view::npos) {
        auto parameter = trim(value.substr(delimiter + 1));
        if (!equal_ascii(parameter, "charset=utf-8") && !equal_ascii(parameter, "charset=\"utf-8\""))
          return state.reject(Error::protocol);
      }
    } else if (equal_ascii(name, "content-encoding")) {
      if (state.content_encoding || !equal_ascii(value, "identity"))
        return state.reject(Error::protocol);
      state.content_encoding = true;
    } else if (equal_ascii(name, "transfer-encoding")) {
      if (state.transfer_encoding || !equal_ascii(value, "chunked"))
        return state.reject(Error::protocol);
      state.transfer_encoding = true;
    }
    state.field.clear();
    state.value.clear();
    return HPE_OK;
  }

  static int on_headers(llhttp_t *parser)
  {
    auto &state = self(parser);
    if (parser->http_major != 1 || parser->http_minor > 1 || parser->upgrade || parser->status_code < 100 ||
      parser->status_code > 599 || parser->status_code == 101)
      return state.reject(Error::protocol);
    if (parser->status_code < 200) {
      if (++state.interim > 4)
        return state.reject(Error::resource_limit);
      return HPE_OK;
    }
    if (!state.content_type)
      return state.reject(Error::protocol);
    if ((parser->flags & F_CONTENT_LENGTH) && parser->content_length > maximum_body)
      return state.reject(Error::resource_limit);
    state.response.status = parser->status_code;
    return HPE_OK;
  }

  static int on_body(llhttp_t *parser, const char *data, std::size_t size)
  {
    auto &state = self(parser);
    if (parser->status_code < 200 || size > maximum_body - state.response.body.size())
      return state.reject(Error::resource_limit);
    state.response.body.insert(state.response.body.end(), data, data + size);
    return HPE_OK;
  }

  static int on_complete(llhttp_t *parser)
  {
    auto &state = self(parser);
    if (parser->status_code < 200) {
      state.content_type = false;
      state.content_encoding = false;
      state.transfer_encoding = false;
      return HPE_OK;
    }
    state.complete = true;
    return HPE_PAUSED;
  }

  Result<void> feed(std::span<const unsigned char> bytes)
  {
    auto *first = reinterpret_cast<const char *>(bytes.data());
    auto result = llhttp_execute(&parser, first, bytes.size());
    if (result == HPE_OK)
      return {};
    if (result == HPE_PAUSED && complete && llhttp_get_error_pos(&parser) == first + bytes.size())
      return {};
    return std::unexpected(error ? error : make_error_code(Error::protocol));
  }

  Result<void> finish()
  {
    auto result = llhttp_finish(&parser);
    if ((result == HPE_OK || result == HPE_PAUSED) && complete)
      return {};
    return std::unexpected(error ? error : make_error_code(Error::protocol));
  }
};

void append(SecretText &destination, std::string_view text)
{
  destination.insert(destination.end(), text.begin(), text.end());
}

Task<OAuthHttpResponse> exchange(OAuthHttpRequest request, TlsContext credentials, std::chrono::milliseconds deadline)
{
  co_await cancellation_point();
  auto url = oauth_https_url(request.url);
  if (!url)
    co_await fail(url.error());
  if (deadline <= std::chrono::milliseconds::zero() || deadline > std::chrono::hours{24} ||
    request.body.size() > maximum_request || (!request.post && !request.body.empty()))
    co_await fail(std::errc::invalid_argument);
  if (!request.authorization.empty()) {
    auto authorization = view(request.authorization);
    bool valid = authorization.starts_with("Basic ") && authorization.size() > 6 && authorization.size() <= 131072 &&
      std::ranges::all_of(authorization.substr(6), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
          byte == '+' || byte == '/' || byte == '=';
      });
    if (!valid)
      co_await fail(std::errc::invalid_argument);
  }

  SecretText outgoing;
  append(outgoing, request.post ? "POST " : "GET ");
  append(outgoing, url->target);
  append(outgoing, " HTTP/1.1\r\nHost: ");
  append(outgoing, url->authority);
  append(outgoing, "\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n");
  if (!request.authorization.empty()) {
    append(outgoing, "Authorization: ");
    append(outgoing, view(request.authorization));
    append(outgoing, "\r\n");
  }
  if (request.post) {
    append(outgoing, "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: ");
    std::array<char, 32> length;
    auto converted = std::to_chars(length.data(), length.data() + length.size(), request.body.size());
    append(outgoing, {length.data(), converted.ptr});
    append(outgoing, "\r\n");
  }
  append(outgoing, "\r\n");
  outgoing.insert(outgoing.end(), request.body.begin(), request.body.end());
  if (outgoing.size() > maximum_request)
    co_await fail(Error::resource_limit);

  auto socket = co_await tcp::connect(url->host, url->port);
  auto stream = co_await tls::client(std::move(socket), credentials, url->host, TlsHandshakeOptions{deadline});
  auto protocol = stream.negotiated_protocol();
  if (!protocol.empty() && protocol != "http/1.1")
    co_await fail(Error::protocol);
  co_await stream.write_all(std::as_bytes(std::span{outgoing}));
  clear_secret(outgoing.data(), outgoing.size());
  outgoing.clear();
  clear_secret(request.body.data(), request.body.size());
  request.body.clear();
  clear_secret(request.authorization.data(), request.authorization.size());
  request.authorization.clear();

  ResponseParser response;
  SecretArray<4096> buffer;
  std::size_t received = 0;
  while (!response.complete) {
    auto size = co_await stream.read(std::as_writable_bytes(std::span{buffer.bytes}));
    if (!size) {
      if (auto finished = response.finish(); !finished)
        co_await fail(finished.error());
      break;
    }
    if (size > maximum_wire - received)
      co_await fail(Error::resource_limit);
    received += size;
    if (auto parsed = response.feed(std::span{buffer.bytes}.first(size)); !parsed)
      co_await fail(parsed.error());
  }
  // A complete framed reply does not depend on the peer accepting our close_notify.
  static_cast<void>(co_await as_result(stream.shutdown_send()));
  co_await cancellation_point();
  co_return std::move(response.response);
}

} // namespace

Task<OAuthHttpResponse> oauth_https(
  OAuthHttpRequest request,
  TlsContext credentials,
  std::chrono::milliseconds deadline)
{
  return timeout(deadline, exchange(std::move(request), std::move(credentials), deadline));
}

} // namespace weave::pg::detail
