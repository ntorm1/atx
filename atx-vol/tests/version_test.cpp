#include <gtest/gtest.h>

#include <string>

#include "atx/vol/api/core/version.hpp"

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

// The header half of the same contract: the macros a consumer feature-gates on
// and the constants it reads must describe the SAME release as the compiled
// symbol above. Everything here is generated from project(VERSION), so a failure
// means the generation wiring broke, not that someone forgot to edit a literal.
TEST(Version, MacrosAndConstantsMatchTheCMakeProjectVersion) {
  EXPECT_EQ(std::string{ATX_VOL_VERSION_STRING}, ATX_VOL_CMAKE_PROJECT_VERSION);
  EXPECT_EQ(atx::vol::kVersionString, ATX_VOL_CMAKE_PROJECT_VERSION);

  const std::string from_components = std::to_string(ATX_VOL_VERSION_MAJOR) + "." +
                                      std::to_string(ATX_VOL_VERSION_MINOR) + "." +
                                      std::to_string(ATX_VOL_VERSION_PATCH);
  EXPECT_EQ(from_components, ATX_VOL_CMAKE_PROJECT_VERSION);

  EXPECT_EQ(atx::vol::kVersionMajor, ATX_VOL_VERSION_MAJOR);
  EXPECT_EQ(atx::vol::kVersionMinor, ATX_VOL_VERSION_MINOR);
  EXPECT_EQ(atx::vol::kVersionPatch, ATX_VOL_VERSION_PATCH);
}

// ATX_VOL_VERSION is the numeric-comparable form, and the whole point of it is
// that `#if` can order two releases. Pin the packing AND its monotonicity, so a
// consumer's `#if ATX_VOL_VERSION >= ATX_VOL_VERSION_NUM(...)` guard means what
// it reads like.
TEST(Version, NumericFormIsOrderedAndConsistent) {
  EXPECT_EQ(ATX_VOL_VERSION, ATX_VOL_VERSION_NUM(ATX_VOL_VERSION_MAJOR, ATX_VOL_VERSION_MINOR,
                                                 ATX_VOL_VERSION_PATCH));

  static_assert(ATX_VOL_VERSION_NUM(1, 0, 0) > ATX_VOL_VERSION_NUM(0, 999, 999));
  static_assert(ATX_VOL_VERSION_NUM(1, 1, 0) > ATX_VOL_VERSION_NUM(1, 0, 999));
  static_assert(ATX_VOL_VERSION_NUM(1, 0, 1) > ATX_VOL_VERSION_NUM(1, 0, 0));

  // This build is at least the release this file was written for; usable in the
  // preprocessor, which is the reason the macro exists at all.
#if ATX_VOL_VERSION < ATX_VOL_VERSION_NUM(1, 0, 0)
#error "atx-vol 1.0.0 is the floor for this test"
#endif
  EXPECT_GE(ATX_VOL_VERSION, ATX_VOL_VERSION_NUM(1, 0, 0));
}

} // namespace
