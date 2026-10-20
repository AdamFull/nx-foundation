#include "core/foundation/diagnostics/diagnostic.h"

#if defined(_WINDOWS_)
#error "diagnostic.h must not pull in <windows.h>"
#endif

#include "framework/nxtest.h"

TEST_CASE("NX_ASSERT: every form passes a true condition") {
  int checked = 0;
  NX_ASSERT(++checked == 1);
  NX_ASSERT(++checked == 2, "a plain message");
  NX_ASSERT(++checked == 3, "a formatted message {} of {}", checked, 3);
#if NX_DEBUG
  CHECK(checked == 3);
#else
  CHECK(checked == 0);
#endif
}

TEST_CASE("dbg helpers: answer in debug builds and do nothing otherwise") {
  const bool present = nx::dbg_is_present();
  (void)present;
  nx::dbg_wait(0);
#if NX_DEBUG
  CHECK(nx::dbg_get_pid() > 0);
#else
  CHECK(nx::dbg_get_pid() == 0);
#endif
}
