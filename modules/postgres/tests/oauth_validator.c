#include "postgres.h"
#include "fmgr.h"
#include "libpq/oauth.h"

PG_MODULE_MAGIC;

static bool validate(
  const ValidatorModuleState *state,
  const char *token,
  const char *role,
  ValidatorModuleResult *result)
{
  (void)state;
  result->authorized = strcmp(token, "abc") == 0 && strcmp(role, "weave") == 0;
  result->authn_id = pstrdup("weave");
  return true;
}

PGDLLEXPORT const OAuthValidatorCallbacks *_PG_oauth_validator_module_init(void)
{
  static const OAuthValidatorCallbacks callbacks = {PG_OAUTH_VALIDATOR_MAGIC, NULL, NULL, validate};
  return &callbacks;
}
