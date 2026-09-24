#include "preference_source.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace atx_test_l8_e2e_preference_source {
namespace impl = atx::impl;
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

inline impl::PanelIdentity identity(atx::usize dates, atx::usize names) {
    impl::PanelIdentity id;
    id.instrument_namespace = "test.l8";
    for (atx::usize t = 0; t < dates; ++t) id.session_keys.push_back(100 + static_cast<atx::i64>(t));
    for (atx::usize i = 0; i < names; ++i) {
        id.instrument_ids.push_back(std::to_string(1000 + i));
        id.original_instrument_indices.push_back(i);
    }
    id.recipe = "stage=test";
    id.parents.push_back({"research", atx::core::sha256_hex("evaluation").value()});
    return id;
}

inline impl::PanelArtifact combo(std::vector<atx::f64> alpha, atx::usize dates, atx::usize names) {
    impl::PanelArtifact artifact{
        atx::engine::alpha::Panel::create(dates, names, {"alpha"}, {std::move(alpha)}, {}).value(),
        identity(dates, names), "", ""};
    return artifact;
}


TEST(PreferenceSource, RowIsDemeanedAndScaledToTargetGross) {
    const auto artifact = combo({1.0, 2.0, 3.0, 5.0, kNaN, 1.0}, 2, 3);
    const auto source = impl::PreferenceSource::from_combo(artifact, identity(2, 3), 101, 0.5);
    ASSERT_TRUE(source) << source.error().message();
    const auto first = source->preference(0);
    ASSERT_TRUE(first) << first.error().message();
    // {1,2,3} -> {-1,0,1} -> gross 2 -> scaled to 0.5.
    EXPECT_DOUBLE_EQ((*first)[0], -0.25);
    EXPECT_DOUBLE_EQ((*first)[1], 0.0);
    EXPECT_DOUBLE_EQ((*first)[2], 0.25);
    const auto second = source->preference(1);
    ASSERT_TRUE(second);
    // NaN is "no view": {5, NaN, 1} -> {2, 0, -2} -> scaled.
    EXPECT_DOUBLE_EQ((*second)[0], 0.25);
    EXPECT_DOUBLE_EQ((*second)[1], 0.0);
    EXPECT_DOUBLE_EQ((*second)[2], -0.25);
}

TEST(PreferenceSource, RowAfterInformationCutoffIsRefused) {
    const auto artifact = combo({1.0, 2.0, 3.0, 4.0}, 2, 2);
    const auto source = impl::PreferenceSource::from_combo(artifact, identity(2, 2), 100);
    ASSERT_TRUE(source);
    EXPECT_TRUE(source->preference(0));
    const auto late = source->preference(1);
    ASSERT_FALSE(late);
    EXPECT_NE(late.error().message().find("cutoff"), std::string::npos);
    EXPECT_FALSE(source->preference(2));
}

TEST(PreferenceSource, MisalignedAxesOrMissingFieldAreRejected) {
    const auto artifact = combo({1.0, 2.0, 3.0, 4.0}, 2, 2);
    auto shifted = identity(2, 2);
    shifted.session_keys[1] = 999;
    EXPECT_FALSE(impl::PreferenceSource::from_combo(artifact, shifted, 1000));
    EXPECT_FALSE(impl::PreferenceSource::from_combo(artifact, identity(2, 2), 1000, 1.0, "beta"));
    EXPECT_FALSE(impl::PreferenceSource::from_combo(artifact, identity(2, 2), 1000, 0.0));
}

TEST(PreferenceSource, FlatOrEmptyRowGivesNoPreference) {
    const auto artifact = combo({2.0, 2.0, kNaN, kNaN}, 2, 2);
    const auto source = impl::PreferenceSource::from_combo(artifact, identity(2, 2), 1000);
    ASSERT_TRUE(source);
    EXPECT_EQ(source->preference(0).value(), (std::vector<atx::f64>{0.0, 0.0}));
    EXPECT_EQ(source->preference(1).value(), (std::vector<atx::f64>{0.0, 0.0}));
}

} // namespace atx_test_l8_e2e_preference_source
