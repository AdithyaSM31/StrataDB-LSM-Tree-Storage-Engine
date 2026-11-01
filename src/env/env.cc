#include "strata/env.h"

#include "env/env_posix.h"

namespace strata {

Env* Env::Default() {
  static Env* env = new PosixEnv();
  return env;
}

}  // namespace strata
