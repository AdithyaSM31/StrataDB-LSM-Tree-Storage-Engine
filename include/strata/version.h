#ifndef STRATA_INCLUDE_VERSION_H_
#define STRATA_INCLUDE_VERSION_H_

namespace strata {

inline constexpr int kMajorVersion = 0;
inline constexpr int kMinorVersion = 1;

// Returns "major.minor", e.g. "0.1".
const char* VersionString();

}  // namespace strata

#endif  // STRATA_INCLUDE_VERSION_H_
