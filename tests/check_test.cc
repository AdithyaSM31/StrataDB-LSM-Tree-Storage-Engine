// Tests for STRATA_CHECK and STRATA_DCHECK (util/check.h): checks abort with a
// useful message, and DCHECK compiles to nothing when STRATA_DEBUG_CHECKS is off.
#include "util/check.h"

#include <gtest/gtest.h>

namespace strata {
namespace {

TEST(CheckTest, PassesOnTrue) {
  STRATA_CHECK(1 + 1 == 2);
  STRATA_DCHECK(1 + 1 == 2);
}

TEST(CheckTest, CheckAbortsOnFalse) {
  EXPECT_DEATH(STRATA_CHECK(1 + 1 == 3), "check failed: 1 \\+ 1 == 3");
}

#if STRATA_DEBUG_CHECKS
TEST(CheckTest, DcheckAbortsWhenEnabled) { EXPECT_DEATH(STRATA_DCHECK(false), "check failed"); }
#else
TEST(CheckTest, DcheckIsCompiledOutWhenDisabled) {
  int evaluated = 0;
  STRATA_DCHECK(++evaluated == 1);
  EXPECT_EQ(evaluated, 0);
}
#endif

}  // namespace
}  // namespace strata
