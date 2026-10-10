#include <gssapi/gssapi.h>
#include <dlfcn.h>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <atomic>

static std::atomic<unsigned> entered = 0;
static std::atomic<unsigned> wrapped = 0;
static std::atomic<unsigned> unwrapped = 0;

extern "C" unsigned weave_test_gss_wrapped()
{
  return wrapped.load(std::memory_order_acquire);
}

extern "C" unsigned weave_test_gss_unwrapped()
{
  return unwrapped.load(std::memory_order_acquire);
}

extern "C" OM_uint32 gss_wrap(
  OM_uint32 *minor,
  gss_ctx_id_t context,
  int encrypt,
  gss_qop_t qop,
  gss_buffer_t input,
  int *confidential,
  gss_buffer_t output)
{
  using Function = decltype(&gss_wrap);
  static auto original = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "gss_wrap"));
  if (std::getenv("WEAVE_PROBE_RECORD_DELAY")) {
    wrapped.fetch_add(1, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  return original(minor, context, encrypt, qop, input, confidential, output);
}

extern "C" OM_uint32 gss_unwrap(
  OM_uint32 *minor,
  gss_ctx_id_t context,
  gss_buffer_t input,
  gss_buffer_t output,
  int *confidential,
  gss_qop_t *qop)
{
  using Function = decltype(&gss_unwrap);
  static auto original = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "gss_unwrap"));
  if (std::getenv("WEAVE_PROBE_RECORD_DELAY")) {
    unwrapped.fetch_add(1, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  return original(minor, context, input, output, confidential, qop);
}

extern "C" unsigned weave_test_gss_entered()
{
  return entered.load(std::memory_order_acquire);
}

extern "C" OM_uint32 gss_init_sec_context(
  OM_uint32 *minor,
  gss_cred_id_t credential,
  gss_ctx_id_t *context,
  gss_name_t target,
  gss_OID mechanism,
  OM_uint32 flags,
  OM_uint32 lifetime,
  gss_channel_bindings_t binding,
  gss_buffer_t input,
  gss_OID *actual,
  gss_buffer_t output,
  OM_uint32 *returned,
  OM_uint32 *expires)
{
  using Function = decltype(&gss_init_sec_context);
  static auto original = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "gss_init_sec_context"));
  if (std::getenv("WEAVE_PROBE_DELAY")) {
    entered.fetch_add(1, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  return original(
    minor,
    credential,
    context,
    target,
    mechanism,
    flags,
    lifetime,
    binding,
    input,
    actual,
    output,
    returned,
    expires);
}

extern "C" OM_uint32 gss_delete_sec_context(OM_uint32 *minor, gss_ctx_id_t *context, gss_buffer_t token)
{
  using Function = decltype(&gss_delete_sec_context);
  static auto original = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "gss_delete_sec_context"));
  if (std::getenv("WEAVE_PROBE_DELAY"))
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  return original(minor, context, token);
}
