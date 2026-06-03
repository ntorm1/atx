// Single translation unit that includes the umbrella header, verifying that
// every public module compiles together (no ODR/macro/namespace collisions)
// and giving the static library a non-header anchor object.
#include "atx/core/core.hpp"
