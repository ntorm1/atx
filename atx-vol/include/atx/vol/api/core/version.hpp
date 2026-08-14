#pragma once

// Library identity for atx-vol.
//
// SINGLE-SOURCED (plan 5.3). Every value below derives from
// `project(atx VERSION ...)` in the root CMakeLists through the generated
// atx/vol/detail/version_generated.hpp. This header restates nothing, and
// neither does src/version.cpp — it used to carry its own "0.1.0" literal that
// nothing compared against the constants here. `Version.*` in
// atx-vol/tests/version_test.cpp compares all of it back against
// ${PROJECT_VERSION}, so a drift fails the test gate rather than a consumer.
//
// ── Macros this header leaves defined (its deliberate macro surface) ─────────
//
//   ATX_VOL_VERSION_MAJOR / _MINOR / _PATCH  integer components
//   ATX_VOL_VERSION_STRING                   "MAJOR.MINOR.PATCH"
//   ATX_VOL_VERSION_NUM(major, minor, patch) pack a version for comparison
//   ATX_VOL_VERSION                          this build, packed
//
// These are macros, and not only the constexpr constants below, because the
// point of a version is PREPROCESSOR feature-gating — a consumer supporting
// several atx-vol releases writes
//
//   #if ATX_VOL_VERSION >= ATX_VOL_VERSION_NUM(1, 1, 0)
//     // ... use the 1.1 API ...
//   #endif
//
// which a `constexpr int` cannot express: the header that would declare it may
// not exist in the older release at all.

#include <string_view>

#include "atx/vol/detail/version_generated.hpp" // configure_file'd from project(VERSION)

// Numeric-comparable packing: three decimal digits per component, so integer
// ordering IS semantic-version ordering for any component below 1000.
#define ATX_VOL_VERSION_NUM(major, minor, patch) ((major) * 1000000 + (minor) * 1000 + (patch))

#define ATX_VOL_VERSION                                                                            \
  ATX_VOL_VERSION_NUM(ATX_VOL_VERSION_MAJOR, ATX_VOL_VERSION_MINOR, ATX_VOL_VERSION_PATCH)

namespace atx::vol {

inline constexpr int kVersionMajor = ATX_VOL_VERSION_MAJOR;
inline constexpr int kVersionMinor = ATX_VOL_VERSION_MINOR;
inline constexpr int kVersionPatch = ATX_VOL_VERSION_PATCH;

// Semantic version string, e.g. "1.0.0", as the HEADERS declare it.
inline constexpr std::string_view kVersionString = ATX_VOL_VERSION_STRING;

// The same string through a COMPILED symbol. Prefer this over kVersionString
// when the answer must describe the library that was actually linked: a header/
// binary skew shows up as `version() != kVersionString` instead of going unseen.
[[nodiscard]] std::string_view version() noexcept;

} // namespace atx::vol
