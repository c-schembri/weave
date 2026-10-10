/* Private qualification extension: real Keycloak introspection, not a product JWT validator. */
#include "postgres.h"
#include "fmgr.h"
#include "libpq/oauth.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <openssl/crypto.h>
#include <time.h>

PG_MODULE_MAGIC;

struct Response {
  char bytes[65536];
  size_t size;
};

static size_t receive(char *data, size_t size, size_t count, void *argument)
{
  struct Response *response = argument;
  if (size && count > (sizeof(response->bytes) - response->size) / size)
    return 0;
  size_t length = size * count;
  memcpy(response->bytes + response->size, data, length);
  response->size += length;
  return length;
}

static const char *json_text(struct json_object *object, const char *name)
{
  struct json_object *value = NULL;
  if (!object || !json_object_is_type(object, json_type_object) || !json_object_object_get_ex(object, name, &value) ||
    !json_object_is_type(value, json_type_string))
    return "";
  const char *text = json_object_get_string(value);
  return strlen(text) == (size_t)json_object_get_string_len(value) ? text : "";
}

static bool audience(struct json_object *object)
{
  struct json_object *value = NULL;
  if (!object || !json_object_is_type(object, json_type_object) || !json_object_object_get_ex(object, "aud", &value))
    return false;
  if (json_object_is_type(value, json_type_string))
    return json_object_get_string_len(value) == 14 && strcmp(json_object_get_string(value), "weave-postgres") == 0;
  if (!json_object_is_type(value, json_type_array))
    return false;
  for (size_t index = 0; index < json_object_array_length(value); ++index) {
    struct json_object *item = json_object_array_get_idx(value, index);
    if (json_object_is_type(item, json_type_string) && json_object_get_string_len(item) == 14 &&
      strcmp(json_object_get_string(item), "weave-postgres") == 0)
      return true;
  }
  return false;
}

static bool scope(const char *value, const char *expected)
{
  const size_t length = strlen(expected);
  while (*value) {
    const char *end = strchr(value, ' ');
    const size_t size = end ? (size_t)(end - value) : strlen(value);
    if (size == length && memcmp(value, expected, length) == 0)
      return true;
    if (!end)
      return false;
    value = end + 1;
  }
  return false;
}

static bool validate(
  const ValidatorModuleState *state,
  const char *token,
  const char *role,
  ValidatorModuleResult *result)
{
  (void)state;
  result->authorized = false;
  if (strcmp(role, "weave") != 0 || !*token || strlen(token) > 16384)
    return true;
  const char *endpoint = getenv("WEAVE_KEYCLOAK_INTROSPECTION");
  const char *issuer = getenv("WEAVE_KEYCLOAK_ISSUER");
  const char *secret = getenv("WEAVE_KEYCLOAK_SECRET");
  const char *trust = getenv("WEAVE_KEYCLOAK_CA");
  if (!endpoint || !issuer || !secret || !trust)
    return false;

  CURL *client = curl_easy_init();
  if (!client)
    return false;
  char *encoded = curl_easy_escape(client, token, 0);
  char *encoded_secret = curl_easy_escape(client, secret, 0);
  char *form = psprintf("token=%s", encoded ? encoded : "");
  struct Response response = {0};
  curl_easy_setopt(client, CURLOPT_URL, endpoint);
  curl_easy_setopt(client, CURLOPT_CAINFO, trust);
  curl_easy_setopt(client, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(client, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(client, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(client, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(client, CURLOPT_PROXY, "");
  curl_easy_setopt(client, CURLOPT_USERNAME, "weave-pg");
  curl_easy_setopt(client, CURLOPT_PASSWORD, encoded_secret);
  curl_easy_setopt(client, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
  curl_easy_setopt(client, CURLOPT_POSTFIELDS, form);
  curl_easy_setopt(client, CURLOPT_TIMEOUT_MS, 10000L);
  curl_easy_setopt(client, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt(client, CURLOPT_WRITEFUNCTION, receive);
  curl_easy_setopt(client, CURLOPT_WRITEDATA, &response);
  CURLcode status = encoded && encoded_secret ? curl_easy_perform(client) : CURLE_OUT_OF_MEMORY;
  long code = 0;
  curl_easy_getinfo(client, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(client);
  if (encoded)
    OPENSSL_cleanse(encoded, strlen(encoded));
  if (encoded_secret)
    OPENSSL_cleanse(encoded_secret, strlen(encoded_secret));
  curl_free(encoded);
  curl_free(encoded_secret);
  OPENSSL_cleanse(form, strlen(form));
  pfree(form);

  struct json_tokener *parser = json_tokener_new_ex(16);
  if (!parser) {
    OPENSSL_cleanse(&response, sizeof(response));
    return false;
  }
  json_tokener_set_flags(parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
  struct json_object *object = status == CURLE_OK && code == 200
    ? json_tokener_parse_ex(parser, response.bytes, response.size)
    : NULL;
  struct json_object *active = NULL;
  struct json_object *expires = NULL;
  bool valid = object && json_tokener_get_error(parser) == json_tokener_success &&
    json_tokener_get_parse_end(parser) == response.size && json_object_is_type(object, json_type_object) &&
    json_object_object_get_ex(object, "active", &active) && json_object_is_type(active, json_type_boolean) &&
    json_object_get_boolean(active) && strcmp(json_text(object, "iss"), issuer) == 0 &&
    strcmp(json_text(object, "client_id"), "weave-pg") == 0 &&
    strcmp(json_text(object, "preferred_username"), role) == 0 &&
    strcmp(json_text(object, "token_type"), "Bearer") == 0 && scope(json_text(object, "scope"), "openid") &&
    scope(json_text(object, "scope"), "profile") && audience(object) &&
    json_object_object_get_ex(object, "exp", &expires) && json_object_is_type(expires, json_type_int) &&
    json_object_get_int64(expires) > time(NULL);
  if (valid) {
    result->authorized = true;
    result->authn_id = pstrdup(json_text(object, "preferred_username"));
  } else {
    ereport(
      LOG,
      (errmsg(
        "Keycloak control checks: curl=%d http=%ld json=%d consumed=%d active=%d issuer=%d client=%d role=%d type=%d "
        "scope=%d audience=%d expiry=%d",
        (int)status,
        code,
        (int)json_tokener_get_error(parser),
        json_tokener_get_parse_end(parser) == response.size,
        active && json_object_get_boolean(active),
        object && strcmp(json_text(object, "iss"), issuer) == 0,
        object && strcmp(json_text(object, "client_id"), "weave-pg") == 0,
        object && strcmp(json_text(object, "preferred_username"), role) == 0,
        object && strcmp(json_text(object, "token_type"), "Bearer") == 0,
        object && scope(json_text(object, "scope"), "openid") && scope(json_text(object, "scope"), "profile"),
        object && audience(object),
        expires && json_object_get_int64(expires) > time(NULL))));
  }
  json_object_put(object);
  json_tokener_free(parser);
  OPENSSL_cleanse(&response, sizeof(response));
  return true;
}

PGDLLEXPORT const OAuthValidatorCallbacks *_PG_oauth_validator_module_init(void)
{
  static const OAuthValidatorCallbacks callbacks = {PG_OAUTH_VALIDATOR_MAGIC, NULL, NULL, validate};
  return &callbacks;
}
