#include <gtest/gtest.h>

#include <string>

#include "atx/vol/version.hpp"

// The library version has exactly ONE source of truth: `project(atx VERSION ...)`
// in the root CMakeLists (plan 5.3). ATX_VOL_CMAKE_PROJECT_VERSION is that field
// baked in by atx-vol/tests/CMakeLists.txt straight from ${PROJECT_VERSION}, so
// these tests compare what the library SHIPS -- the compiled `version()` symbol a
// consumer links, and the header constants it compiles against -- back against the
// build system's own declaration. A literal that drifts from project(VERSION)
// fails here rather than at a consumer.
namespace {

TEST(Version, Query_MatchesTheCMakeProjectVersion) {
  EXPECT_EQ(atx::vol::version(), ATX_VOL_CMAKE_PROJECT_VERSION);
}

} // namespace
