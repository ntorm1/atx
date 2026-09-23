#include "equity_allocation.hpp"

#include <cmath>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace atx_test_l8_e2e_equity_factor {
namespace impl = atx::impl;
constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr atx::usize kNames = 6;

// Six names; the preference is long high-beta sector-0 names and short low-beta
// sector-1 names, so an unconstrained book carries large beta and sector bets.
struct Fixture {
    std::vector<atx::i64> keys;
    std::vector<atx::f64> closes;
    std::vector<atx::f64> preference{0.20, 0.15, 0.10, -0.10, -0.15, -0.20};
    std::vector<atx::u8> eligibility = std::vector<atx::u8>(kNames, 1);
    std::vector<atx::f64> beta{1.6, 1.4, 1.2, 0.8, 0.6, 0.4};
    std::vector<atx::usize> sector{7, 7, 7, 3, 3, 9};
    std::vector<atx::f64> zeros = std::vector<atx::f64>(kNames, 0.0);
    std::vector<atx::f64> marks = std::vector<atx::f64>(kNames, 100.0);
    impl::EquityAllocationConfig config;

    Fixture() {
        config.name_limit = 0.3;
        config.gross_limit = 1.0;
        config.turnover_limit = 2.0;
        config.risk_penalty = 0.0;
        config.trade_bps = 0.0;
        for (atx::usize t = 0; t < 64; ++t) {
            keys.push_back(static_cast<atx::i64>(t) * kDay);
            for (atx::usize i = 0; i < kNames; ++i) {
                closes.push_back(100.0 + static_cast<atx::f64>((t * (i + 3)) % 7) +
                                 0.1 * static_cast<atx::f64>(i));
            }
        }
    }

    impl::EquityAllocationRiskWindow window() const {
        return {0, 0, 63, keys[63], keys, closes, {}};
    }

    impl::EquityAllocationExecution execution() const {
        return {0, 1, keys[63], keys[63] + kDay, 1000.0, 1000.0, zeros, marks, zeros};
    }

    atx::core::Result<impl::EquityAllocationResult> allocate(bool with_beta, bool with_sector) {
        auto cfg = config;
        if (with_beta) cfg.beta_tolerance = 0.01;
        if (with_sector) cfg.sector_net_cap = 0.02;
        auto decision = impl::freeze_equity_allocation_decision(window(), preference, eligibility, cfg);
        if (!decision) return atx::core::Err(decision.error().code(), decision.error().message());
        const auto attached = impl::attach_equity_exposures(
            *decision, with_beta ? std::span<const atx::f64>(beta) : std::span<const atx::f64>{},
            with_sector ? std::span<const atx::usize>(sector) : std::span<const atx::usize>{});
        if (!attached) return atx::core::Err(attached.error().code(), attached.error().message());
        return impl::allocate_equity_preference(*decision, execution());
    }
};

inline atx::f64 beta_exposure(const Fixture &f, const std::vector<atx::f64> &w) {
    atx::f64 sum = 0.0;
    for (atx::usize i = 0; i < kNames; ++i) sum += f.beta[i] * w[i];
    return sum;
}

inline atx::f64 sector_net(const Fixture &f, const std::vector<atx::f64> &w, atx::usize label) {
    atx::f64 sum = 0.0;
    for (atx::usize i = 0; i < kNames; ++i) if (f.sector[i] == label) sum += w[i];
    return sum;
}
} // namespace atx_test_l8_e2e_equity_factor

using namespace atx_test_l8_e2e_equity_factor;

TEST(EquityAllocationFactor, UnconstrainedPreferenceCarriesBetaAndSectorBets) {
    Fixture f;
    const auto result = f.allocate(false, false);
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_GT(beta_exposure(f, result->weights), 0.2);
    EXPECT_GT(std::abs(sector_net(f, result->weights, 7)), 0.4);
    EXPECT_EQ(result->certificate.postfee_beta_exposure, 0.0);
}

TEST(EquityAllocationFactor, BetaBoundIsEnforcedAndCertified) {
    Fixture f;
    const auto result = f.allocate(true, false);
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_LE(std::abs(beta_exposure(f, result->weights)), 0.01 + 1e-7);
    EXPECT_LE(std::abs(result->certificate.postfee_beta_exposure), 0.01 + 1e-8);
    EXPECT_NEAR(result->certificate.postfee_net, 0.0, 1e-7);
}

TEST(EquityAllocationFactor, SectorNetCapIsEnforcedForEverySector) {
    Fixture f;
    const auto result = f.allocate(false, true);
    ASSERT_TRUE(result) << result.error().message();
    for (const atx::usize label : {3U, 7U, 9U}) {
        EXPECT_LE(std::abs(sector_net(f, result->weights, label)), 0.02 + 1e-7) << label;
    }
    EXPECT_LE(result->certificate.postfee_max_sector_net, 0.02 + 1e-8);
}

TEST(EquityAllocationFactor, BetaAndSectorTogetherKeepPreferenceTracking) {
    Fixture f;
    const auto both = f.allocate(true, true);
    ASSERT_TRUE(both) << both.error().message();
    EXPECT_LE(std::abs(beta_exposure(f, both->weights)), 0.01 + 1e-7);
    for (const atx::usize label : {3U, 7U, 9U}) {
        EXPECT_LE(std::abs(sector_net(f, both->weights, label)), 0.02 + 1e-7);
    }
    // The book still leans with the preference after both projections.
    atx::f64 tracking = 0.0;
    for (atx::usize i = 0; i < kNames; ++i) tracking += both->weights[i] * f.preference[i];
    EXPECT_GT(tracking, 0.0);
}

TEST(EquityAllocationFactor, BoundWithoutExposureIsRejected) {
    Fixture f;
    auto cfg = f.config;
    cfg.beta_tolerance = 0.01;
    const auto decision =
        impl::freeze_equity_allocation_decision(f.window(), f.preference, f.eligibility, cfg);
    ASSERT_TRUE(decision) << decision.error().message();
    EXPECT_FALSE(impl::allocate_equity_preference(*decision, f.execution()));

    auto shaped = *decision;
    const std::vector<atx::f64> short_beta{1.0, 1.0};
    EXPECT_FALSE(impl::attach_equity_exposures(shaped, short_beta, {}));
}

TEST(EquityAllocationFactor, DefaultConfigIsUnchangedByTheOptInFields) {
    Fixture f;
    const auto decision =
        impl::freeze_equity_allocation_decision(f.window(), f.preference, f.eligibility, f.config);
    ASSERT_TRUE(decision);
    const auto plain = impl::allocate_equity_preference(*decision, f.execution());
    const auto via_helper = f.allocate(false, false);
    ASSERT_TRUE(plain && via_helper);
    for (atx::usize i = 0; i < kNames; ++i) EXPECT_EQ(plain->weights[i], via_helper->weights[i]);
}
