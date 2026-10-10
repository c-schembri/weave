#include <gssapi/gssapi.h>
#include <dlfcn.h>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <thread>
#include <chrono>

// Test-only interposition. Acceptor contexts are deliberately not tracked.
static std::mutex guard;
static std::unordered_set<gss_ctx_id_t> contexts;
static std::atomic<unsigned> created;
static std::atomic<unsigned> destroyed;
static std::atomic<unsigned> violations;
static thread_local bool io_thread;

extern "C" void weave_test_gss_io()
{
  io_thread = true;
}

extern "C" unsigned weave_test_gss_created()
{
  return created.load();
}

extern "C" unsigned weave_test_gss_destroyed()
{
  return destroyed.load();
}

extern "C" unsigned weave_test_gss_violations()
{
  return violations.load();
}

extern "C" OM_uint32 gss_init_sec_context(
  OM_uint32 *minor,
  gss_cred_id_t credentials,
  gss_ctx_id_t *context,
  gss_name_t target,
  gss_OID mechanism,
  OM_uint32 flags,
  OM_uint32 lifetime,
  gss_channel_bindings_t bindings,
  gss_buffer_t input,
  gss_OID *actual,
  gss_buffer_t output,
  OM_uint32 *returned,
  OM_uint32 *expiry)
{
  using Function = decltype(&gss_init_sec_context);
  static auto next = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "gss_init_sec_context"));
  auto status = next(
    minor,
    credentials,
    context,
    target,
    mechanism,
    flags,
    lifetime,
    bindings,
    input,
    actual,
    output,
    returned,
    expiry);
  if (*context != GSS_C_NO_CONTEXT) {
    std::lock_guard lock(guard);
    if (contexts.insert(*context).second)
      ++created;
  }
  if (io_thread)
    ++violations;
  return status;
}

extern "C" OM_uint32 gss_delete_sec_context(OM_uint32 *minor, gss_ctx_id_t *context, gss_buffer_t output)
{
  using Function = decltype(&gss_delete_sec_context);
  static auto next = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "gss_delete_sec_context"));
  bool tracked;
  {
    std::lock_guard lock(guard);
    tracked = contexts.erase(*context) != 0;
  }
  if (tracked) {
    if (io_thread)
      ++violations;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  auto status = next(minor, context, output);
  if (tracked)
    ++destroyed;
  return status;
}

extern "C" OM_uint32 gss_wrap(
  OM_uint32 *minor,
  gss_ctx_id_t context,
  int privacy,
  gss_qop_t quality,
  gss_buffer_t input,
  int *confidential,
  gss_buffer_t output)
{
  using Function = decltype(&gss_wrap);
  static auto next = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "gss_wrap"));
  {
    std::lock_guard lock(guard);
    if (io_thread && contexts.contains(context))
      ++violations;
  }
  return next(minor, context, privacy, quality, input, confidential, output);
}

extern "C" OM_uint32 gss_unwrap(
  OM_uint32 *minor,
  gss_ctx_id_t context,
  gss_buffer_t input,
  gss_buffer_t output,
  int *confidential,
  gss_qop_t *quality)
{
  using Function = decltype(&gss_unwrap);
  static auto next = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "gss_unwrap"));
  {
    std::lock_guard lock(guard);
    if (io_thread && contexts.contains(context))
      ++violations;
  }
  return next(minor, context, input, output, confidential, quality);
}
