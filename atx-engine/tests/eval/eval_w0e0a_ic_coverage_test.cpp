// atx-engine/tests/eval/eval_w0e0a_ic_coverage_test.cpp
//
// W0-E0a acceptance "A simulated MA(20) IC series gives 95% CI coverage in [93%, 97%] over
// 2000 repetitions" (E-02, E-03).
//
// The IC of a 21-day forward return overlaps its neighbours by 20 days, so the per-date IC
// series is MA(20). Each repetition draws such a series with a known mean and asks whether
// the published 95% interval covers that mean:
//
//   * the HAC interval cross_section_ic publishes under the default IcHacRule
//     (Hansen-Hodrick, lag max(h - 1, NW rule of thumb)) — `ic_mean_hac`, whose values are
//     pinned to hac::mean_inference on the emitted series by
//     EvalHac.CrossSectionIc_HacFieldsAreHacOfTheEmittedIcSeries;
//   * the naive IID interval (what `naive_t` implies) for contrast;
//   * the production circular-block bootstrap (`bootstrap_mean_interval`) under the V1 and
//     V2 block rules, to measure the E-02 correction.
//
// Deterministic: fixed seeds, no clock.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

#include "atx/core/random.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/eval/cross_section_ic.hpp"
#include "atx/engine/eval/hac.hpp"

namespace atx_test_w0_e0a_ic_coverage {

using namespace atx::engine::eval;

constexpr std::size_t kHorizon = 21U; // MA(h - 1) = MA(20)
constexpr double kTrueMean = 0.02;

// One equal-weight MA(20) IC series of length n with mean kTrueMean: the overlap of a
// 21-day forward return (each date shares 20 daily shocks with its neighbour).
void draw_ma20(atx::core::Xoshiro256pp &rng, std::size_t n, std::vector<double> &eps,
               std::vector<double> &out) {
  eps.resize(n + kHorizon - 1U);
  for (double &e : eps) {
    e = rng.normal();
  }
  out.resize(n);
  double window = 0.0;
  for (std::size_t j = 0U; j < kHorizon; ++j) {
    window += eps[j];
  }
  for (std::size_t t = 0U; t < n; ++t) {
    if (t > 0U) {
      window += eps[t + kHorizon - 1U] - eps[t - 1U];
    }
    out[t] = kTrueMean + 0.01 * window;
  }
}

struct Coverage {
  std::size_t covered{};
  std::size_t reps{};
  [[nodiscard]] double rate() const {
    return (reps == 0U) ? 0.0 : static_cast<double>(covered) / static_cast<double>(reps);
  }
};

TEST(EvalIcCoverage, HacCi_Ma20Series_CoverageWithin93To97PercentOver2000Reps) {
  constexpr std::size_t kReps = 2000U;
  constexpr std::size_t kN = 1750U; // the 2013-2019 panel length
  atx::core::Xoshiro256pp rng{0xC0'7E'2A'6EULL};
  std::vector<double> eps;
  std::vector<double> s;
  Coverage hh{};
  Coverage nw{};
  Coverage iid{};
  std::size_t fallbacks = 0U;
  for (std::size_t rep = 0U; rep < kReps; ++rep) {
    draw_ma20(rng, kN, eps, s);
    // The default IcHacRule::HansenHodrickV1 lag: max(h - 1, floor(4 (n/100)^(2/9))).
    const std::size_t hh_lag = std::max(kHorizon - 1U, hac::newey_west_rule_of_thumb_lag(kN));
    const hac::MeanInference a = hac::mean_inference(s, hac::Kernel::UniformV1, hh_lag, false);
    ASSERT_EQ(a.defined, 1U);
    fallbacks += a.fell_back;
    ++hh.reps;
    hh.covered += (std::fabs(a.mean - kTrueMean) <= kHacZ975 * a.se) ? 1U : 0U;
    // IcHacRule::NeweyWestV1: Bartlett at max(h - 1, NW-1994 automatic lag).
    const std::size_t nw_lag = std::max(kHorizon - 1U, hac::newey_west_auto_lag(s));
    const hac::MeanInference b = hac::mean_inference(s, hac::Kernel::BartlettV1, nw_lag, false);
    ++nw.reps;
    nw.covered += (std::fabs(b.mean - kTrueMean) <= kHacZ975 * b.se) ? 1U : 0U;
    // The naive IID interval implied by `naive_t`.
    const hac::MeanInference c = hac::mean_tstat(s, hac::TStatRule::IidV1);
    ++iid.reps;
    iid.covered += (std::fabs(c.mean - kTrueMean) <= kHacZ975 * c.se) ? 1U : 0U;
  }
  std::printf("[EvalIcCoverage] MA(20), n=%zu, %zu reps: HansenHodrickV1 %.4f  "
              "NeweyWestV1 %.4f  naive-IID %.4f  (uniform->Bartlett fallbacks %zu)\n",
              kN, kReps, hh.rate(), nw.rate(), iid.rate(), fallbacks);
  // The acceptance band, on the published default interval.
  EXPECT_GE(hh.rate(), 0.93);
  EXPECT_LE(hh.rate(), 0.97);
  // E-03 in numbers: the IID interval is far too narrow on overlapping ICs.
  EXPECT_LT(iid.rate(), 0.60);
  EXPECT_GT(nw.rate(), iid.rate() + 0.25);
}

TEST(EvalIcCoverage, BootstrapBlockRule_V2MovesMa20CoverageTowardNominal) {
  constexpr std::size_t kReps = 400U;
  constexpr std::size_t kN = 1000U;
  constexpr std::size_t kDraws = 199U;
  const std::size_t len_v1 = detail::block_len_for_rule(BlockLenRule::HalfHorizonV1, kHorizon, 5U);
  const std::size_t len_v2 = detail::block_len_for_rule(BlockLenRule::TwoHorizonV2, kHorizon, 5U);
  ASSERT_EQ(len_v1, 11U);
  ASSERT_EQ(len_v2, 42U);
  atx::core::Xoshiro256pp rng{0xB00'75'7A9ULL};
  std::vector<double> eps;
  std::vector<double> s;
  std::vector<double> stats(kDraws);
  Coverage v1{};
  Coverage v2{};
  for (std::size_t rep = 0U; rep < kReps; ++rep) {
    draw_ma20(rng, kN, eps, s);
    const auto key = static_cast<atx::u64>(rep) * 0x9E3779B97F4A7C15ULL;
    auto a = bootstrap_mean_interval(s, len_v1, kDraws, key, stats);
    auto b = bootstrap_mean_interval(s, len_v2, kDraws, key, stats);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(a->reportable, 1U);
    ASSERT_EQ(b->reportable, 1U);
    ++v1.reps;
    ++v2.reps;
    v1.covered += (a->lo <= kTrueMean && kTrueMean <= a->hi) ? 1U : 0U;
    v2.covered += (b->lo <= kTrueMean && kTrueMean <= b->hi) ? 1U : 0U;
  }
  std::printf("[EvalIcCoverage] CBB on MA(20), n=%zu, B=%zu, %zu reps: HalfHorizonV1 (L=%zu) "
              "%.4f  TwoHorizonV2 (L=%zu) %.4f\n",
              kN, kDraws, kReps, len_v1, v1.rate(), len_v2, v2.rate());
  // E-02 in numbers: the V1 block of 11 on a 20-lag dependence is far too narrow; the
  // 2h block recovers most of the gap. (The block bootstrap's Bartlett-like taper keeps
  // it a few points short of nominal at L = 2h; the HAC interval above is the one held
  // to the [93%, 97%] band.)
  EXPECT_LT(v1.rate(), 0.88);
  EXPECT_GT(v2.rate(), v1.rate() + 0.05);
  // Review fix pass 1: pin the level of the default bootstrap's coverage, not only its
  // gain over V1, so a later block-rule change cannot regress it silently. Measured 0.9300
  // here; an independent numpy run (n=1750, B=2000, 2000 reps) measured 0.925.
  EXPECT_GE(v2.rate(), 0.90);
}

TEST(EvalIcCoverage, BootstrapMeanInterval_RejectsBadScratchAndReportsReasons) {
  std::vector<double> stats(10U);
  const std::vector<double> s(100U, 0.5);
  EXPECT_FALSE(bootstrap_mean_interval(s, 5U, 11U, 1U, stats).has_value());
  EXPECT_FALSE(bootstrap_mean_interval(s, 5U, kMaxBootstrapDraws + 1U, 1U, stats).has_value());
  const std::vector<double> short_s(19U, 0.5);
  auto r = bootstrap_mean_interval(short_s, 1U, 10U, 1U, stats);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->reportable, 0U);
  EXPECT_EQ(r->unreportable_reason, 1U);
  auto r3 = bootstrap_mean_interval(s, 42U, 10U, 1U, stats); // 100 / 42 = 2 < 10
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3->unreportable_reason, 3U);
}

} // namespace atx_test_w0_e0a_ic_coverage
