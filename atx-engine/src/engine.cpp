#include "atx/engine/engine.hpp"

#include "atx/core/core.hpp"

namespace atx::engine {

int step(int value) {
  // Demonstrates engine -> core dependency.
  return atx::core::add(value, 1);
}

} // namespace atx::engine
