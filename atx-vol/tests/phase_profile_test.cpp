#include <gtest/gtest.h>

#include "atx/vol/detail/phase_profile.hpp"

namespace {

using atx::vol::phase_profile::Region;

#if !defined(ATX_VOL_PROFILE)
static_assert(!atx::vol::phase_profile::profile_enabled());
#endif

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
