#include <gtest/gtest.h>

#include "atx/vol/detail/phase_profile.hpp"

namespace {

using atx::vol::phase_profile::Region;

#if !defined(ATX_VOL_PROFILE)
static_assert(!atx::vol::phase_profile::profile_enabled());
#endif

// The build-configuration ODR guard (plan 5.2), same contract as
// counters_test.cpp: ATX_VOL_PROFILE changes the definition of
// profile_enabled(), snapshot() and reset(), so the configuration is named in
// the namespace and a mismatched TU/library pair fails to LINK rather than
// silently keeping one of two definitions. The alias pins the tag; the
// function-address equalities pin that it is inline, so the unqualified
// spellings used everywhere else keep resolving.
#if defined(ATX_VOL_PROFILE)
namespace tagged_profile = atx::vol::phase_profile::profile_on;
static_assert(tagged_profile::profile_enabled());
#else
namespace tagged_profile = atx::vol::phase_profile::profile_off;
static_assert(!tagged_profile::profile_enabled());
#endif
static_assert(&atx::vol::phase_profile::reset == &tagged_profile::reset,
              "the unqualified spelling must resolve INTO the configuration-tagged namespace");
static_assert(&atx::vol::phase_profile::snapshot == &tagged_profile::snapshot,
              "the unqualified spelling must resolve INTO the configuration-tagged namespace");

TEST(PhaseProfile, ScopeAndSnapshotMatchBuildMode) {
  atx::vol::phase_profile::reset();
  { ATX_VOL_PROFILE_SCOPE(BookGreeks); }
  const auto snapshot = atx::vol::phase_profile::snapshot();
  if constexpr (atx::vol::phase_profile::profile_enabled()) {
    EXPECT_TRUE(snapshot.enabled);
    EXPECT_EQ(snapshot.calls[static_cast<unsigned>(Region::BookGreeks)], 1u);
  } else {
    EXPECT_FALSE(snapshot.enabled);
    EXPECT_EQ(snapshot.calls[static_cast<unsigned>(Region::BookGreeks)], 0u);
    EXPECT_EQ(snapshot.nanoseconds[static_cast<unsigned>(Region::BookGreeks)], 0u);
  }
}

} // namespace
