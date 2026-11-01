// Invariant checks.
//
//   STRATA_CHECK(cond)   always on; aborts with file:line if cond is false.
//   STRATA_DCHECK(cond)  only when built with STRATA_DEBUG_CHECKS=ON (debug,
//                        asan and tsan presets). Compiles to nothing in
//                        release, so it is the place for expensive checks
//                        such as "L1+ files do not overlap".
#ifndef STRATA_UTIL_CHECK_H_
#define STRATA_UTIL_CHECK_H_

#include <cstdio>
#include <cstdlib>

#define STRATA_CHECK(cond)                                                          \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
      std::abort();                                                                 \
    }                                                                               \
  } while (0)

#if STRATA_DEBUG_CHECKS
#define STRATA_DCHECK(cond) STRATA_CHECK(cond)
#else
// sizeof keeps the expression type-checked without evaluating it.
#define STRATA_DCHECK(cond)       \
  do {                            \
    (void)sizeof((cond) ? 1 : 0); \
  } while (0)
#endif

#endif  // STRATA_UTIL_CHECK_H_
