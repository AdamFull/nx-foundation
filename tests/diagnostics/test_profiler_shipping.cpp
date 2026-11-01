#include "framework/nxtest.h"

// Only profiler.h, included last, sees this build pretend to be Shipping.
#if defined(NX_PROFILE_HOOKS)
#error "this test must be the first to include diagnostics/profiler.h"
#endif
#undef NX_PROFILE_ENABLED
#define NX_PROFILE_ENABLED 0
#if !defined(NX_BUILD_SHIPPING)
#define NX_BUILD_SHIPPING 1
#endif

#include "core/foundation/diagnostics/profiler.h"

static_assert(NX_PROFILE_HOOKS == 0);

namespace {

// A zone with a site or a scope could not appear in a constant expression, so
// this only compiles when Shipping's macros leave no code behind.
constexpr int zones_compile_out() {
  NX_PROFILE_ZONE("gone");
  NX_PROFILE_ZONE_COLOR("gone", 1);
  NX_PROFILE_FUNCTION();
  NX_PROFILE_ZONE_TEXT("gone", 4);
  NX_PROFILE_FRAME_MARK();
  NX_PROFILE_FRAME_MARK_NAMED("gone");
  NX_PROFILE_SET_THREAD_NAME("gone");
  NX_PROFILE_SET_THREAD_NAME_ONCE("gone");
  NX_PROFILE_PLOT("gone", 1);
  NX_PROFILE_MESSAGE("gone");
  return 1;
}
static_assert(zones_compile_out() == 1);

} // namespace

TEST_CASE("profile macros: Shipping leaves no code") {
  CHECK(zones_compile_out() == 1);
}
