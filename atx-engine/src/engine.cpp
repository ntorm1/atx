#include "atx/engine/engine.hpp"

#include "atx/core/core.hpp"

namespace atx::engine {

int step(int value) {
  // Demonstrates engine -> core dependency via core's checked arithmetic.
  return atx::core::checked_add(value, 1).value_or(value);
}

} // namespace atx::engine
