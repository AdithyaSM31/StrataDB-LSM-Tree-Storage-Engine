// Proves the toolchain works: the library links, GoogleTest runs, and the
// debug-check macros behave as configured. Delete this file once real unit
// tests exist.
#include <gtest/gtest.h>

#include <string_view>

#include "strata/version.h"
#include "util/check.h"

TEST(SmokeTest, LibraryLinks) { EXPECT_EQ(std::string_view(strata::VersionString()), "0.1"); }

TEST(SmokeTest, CheckPassesOnTrue) {
  STRATA_CHECK(1 + 1 == 2);
  STRATA_DCHECK(1 + 1 == 2);
}

TEST(SmokeTest, CheckAbortsOnFalse) {
  EXPECT_DEATH(STRATA_CHECK(1 + 1 == 3), "check failed: 1 \\+ 1 == 3");
}

#if STRATA_DEBUG_CHECKS
TEST(SmokeTest, DcheckAbortsWhenEnabled) { EXPECT_DEATH(STRATA_DCHECK(false), "check failed"); }
#else
TEST(SmokeTest, DcheckIsCompiledOutWhenDisabled) {
  int evaluated = 0;
  STRATA_DCHECK(++evaluated == 1);
  EXPECT_EQ(evaluated, 0);
}
#endif
