// combine_marginal_rank_ic_test.cpp — platform v8 F-2: the per-date residualise-then-rank-IC
// kernel behind contract K6 (marginal_ic.json).
//
// Suite: CombineMarginalRankIc

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/combine/marginal_rank_ic.hpp"
#include "atx/engine/eval/hac.hpp"

namespace atx_test_v8_combine_marginal_rank_ic {

using atx::f64;
using atx::u8;
using atx::usize;
namespace cb = atx::engine::combine;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

struct Rng {
  std::uint64_t s;
  f64 uni() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<f64>(s >> 11U) + 0.5) / 9007199254740992.0;
  }
  f64 gauss() {
    const f64 u1 = uni();
    const f64 u2 = uni();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

std::vector<f64> draws(Rng &g, usize n) {
  std::vector<f64> v(n);
  for (f64 &x : v) {
    x = g.gauss();
  }
  return v;
}

// Synthetic T x N world: pool p, planted q (independent of p), label noise u, candidate noise v.
struct World {
  static constexpr usize kT = 200;
  static constexpr usize kN = 300;
  std::vector<f64> p, q, u, v;
  World() {
    Rng g{20260929U};
    p = draws(g, kT * kN);
    q = draws(g, kT * kN);
    u = draws(g, kT * kN);
    v = draws(g, kT * kN);
  }
};

struct Daily {
  std::vector<f64> raw, marginal;
  usize spanned = 0U;
};

// One kernel call per date with the single regressor `pool` (row-major T x N spans).
Daily run_days(const std::vector<f64> &candidate, const std::vector<f64> &pool,
               const std::vector<f64> &label) {
  Daily out;
  cb::MarginalRankIcScratch scratch;
  for (usize t = 0U; t < World::kT; ++t) {
    const std::span<const f64> c(candidate.data() + t * World::kN, World::kN);
    const std::span<const f64> y(label.data() + t * World::kN, World::kN);
    const std::vector<std::span<const f64>> regressors{
        std::span<const f64>(pool.data() + t * World::kN, World::kN)};
    const auto day = cb::marginal_rank_ic_day(c, regressors, y, 20U, scratch);
    EXPECT_TRUE(day.has_value());
    if (!day) {
      return out;
    }
    out.raw.push_back(day->raw_ic);
    out.marginal.push_back(day->marginal_ic);
    out.spanned += day->spanned;
  }
  return out;
}

TEST(CombineMarginalRankIc, CentredTiedRanksMatchCompositionArithmetic) {
  const std::vector<f64> values{3.0, 1.0, 1.0, kNaN, 2.0};
  const std::vector<u8> eligible{1U, 1U, 1U, 1U, 0U};
  std::vector<f64> out(values.size());
  std::vector<std::pair<f64, usize>> sorted;
  ASSERT_TRUE(cb::centred_tied_ranks(values, eligible, out, sorted).has_value());
  // Three ranked names, denominator 2 (n - 1) = 4: the tie at positions 0..1 gets 1/4 - .5.
  EXPECT_EQ(out[0], 4.0 / 4.0 - 0.5);
  EXPECT_EQ(out[1], 1.0 / 4.0 - 0.5);
  EXPECT_EQ(out[2], 1.0 / 4.0 - 0.5);
  EXPECT_TRUE(std::isnan(out[3])); // not finite
  EXPECT_TRUE(std::isnan(out[4])); // not eligible
  const std::vector<f64> single{1.0, kNaN};
  std::vector<f64> single_out(2U, 0.0);
  ASSERT_TRUE(cb::centred_tied_ranks(single, {}, single_out, sorted).has_value());
  EXPECT_TRUE(std::isnan(single_out[0]));
  EXPECT_FALSE(cb::centred_tied_ranks(values, eligible, single_out, sorted).has_value());
}

TEST(CombineMarginalRankIc, NoRegressorsGivesMarginalEqualToRaw) {
  const World w;
  std::vector<f64> label(w.p.size());
  for (usize c = 0U; c < label.size(); ++c) {
    label[c] = w.p[c] + w.u[c];
  }
  cb::MarginalRankIcScratch scratch;
  std::vector<std::pair<f64, usize>> sorted;
  std::vector<f64> ranks(World::kN);
  for (usize t = 0U; t < 5U; ++t) {
    // A centred-rank candidate on fully paired names: its residual on the intercept alone is
    // itself, and the pair ranks are an affine map of it, so both ICs coincide.
    const std::span<const f64> p(w.p.data() + t * World::kN, World::kN);
    ASSERT_TRUE(cb::centred_tied_ranks(p, {}, ranks, sorted).has_value());
    const std::span<const f64> y(label.data() + t * World::kN, World::kN);
    const auto day = cb::marginal_rank_ic_day(ranks, {}, y, 20U, scratch);
    ASSERT_TRUE(day.has_value());
    EXPECT_EQ(day->names, World::kN);
    EXPECT_NEAR(day->marginal_ic, day->raw_ic, 1e-12);
    EXPECT_GT(day->raw_ic, 0.4);
  }
}

TEST(CombineMarginalRankIc, CandidateInsideRegressorSpanIsSpannedZero) {
  const World w;
  std::vector<f64> candidate(World::kN), label(World::kN);
  for (usize i = 0U; i < World::kN; ++i) {
    candidate[i] = 2.0 * w.p[i] + 1.0;
    label[i] = w.p[i] + w.u[i];
  }
  cb::MarginalRankIcScratch scratch;
  const std::vector<std::span<const f64>> regressors{
      std::span<const f64>(w.p.data(), World::kN)};
  const auto day = cb::marginal_rank_ic_day(candidate, regressors, label, 20U, scratch);
  ASSERT_TRUE(day.has_value());
  EXPECT_EQ(day->spanned, u8{1});
  EXPECT_EQ(day->marginal_ic, 0.0);
  EXPECT_GT(day->raw_ic, 0.4);
}

TEST(CombineMarginalRankIc, PoolPlusNoiseHasZeroMarginalWithin2Se) {
  const World w;
  std::vector<f64> candidate(w.p.size()), label(w.p.size());
  for (usize c = 0U; c < w.p.size(); ++c) {
    candidate[c] = w.p[c] + 0.1 * w.v[c];
    label[c] = w.p[c] + w.u[c];
  }
  const Daily daily = run_days(candidate, w.p, label);
  std::vector<f64> compact;
  const auto raw = cb::summarize_rank_ic(daily.raw, 21U, compact);
  const auto marginal = cb::summarize_rank_ic(daily.marginal, 21U, compact);
  EXPECT_EQ(raw.dates, World::kT);
  EXPECT_EQ(marginal.dates, World::kT);
  EXPECT_EQ(daily.spanned, 0U);
  EXPECT_GT(raw.hac_t, 10.0);                  // the candidate predicts ...
  EXPECT_LT(std::abs(marginal.hac_t), 2.0);    // ... nothing the pool does not
  EXPECT_LT(std::abs(marginal.mean), 0.02);
}

TEST(CombineMarginalRankIc, PlantedOrthogonalRecovered) {
  const World w;
  std::vector<f64> label(w.p.size());
  for (usize c = 0U; c < w.p.size(); ++c) {
    label[c] = w.p[c] + w.q[c] + w.u[c];
  }
  const Daily daily = run_days(w.q, w.p, label);
  std::vector<f64> compact;
  const auto raw = cb::summarize_rank_ic(daily.raw, 21U, compact);
  const auto marginal = cb::summarize_rank_ic(daily.marginal, 21U, compact);
  EXPECT_GT(marginal.mean, 0.45);
  EXPECT_GT(marginal.hac_t, 10.0);
  EXPECT_LT(std::abs(marginal.mean - raw.mean), 0.05);
}

TEST(CombineMarginalRankIc, SummaryIsBartlettHacOfCompactedSeries) {
  const std::vector<f64> daily{0.10, kNaN, 0.05, -0.02, 0.08, kNaN, 0.03, 0.01};
  const std::vector<f64> finite{0.10, 0.05, -0.02, 0.08, 0.03, 0.01};
  std::vector<f64> compact;
  const auto summary = cb::summarize_rank_ic(daily, 21U, compact);
  const auto reference = atx::engine::eval::hac::mean_inference(
      finite, atx::engine::eval::hac::Kernel::BartlettV1, 21U, true);
  ASSERT_EQ(summary.dates, finite.size());
  EXPECT_EQ(summary.mean, reference.mean);
  ASSERT_EQ(reference.defined, u8{1});
  EXPECT_EQ(summary.hac_t, reference.t);
  const auto empty = cb::summarize_rank_ic(std::vector<f64>{kNaN, kNaN}, 21U, compact);
  EXPECT_EQ(empty.dates, 0U);
  EXPECT_TRUE(std::isnan(empty.mean));
  EXPECT_TRUE(std::isnan(empty.hac_t));
}

TEST(CombineMarginalRankIc, PairwiseCorrelationAveragesDefinedDates) {
  cb::PairwiseRowCorrelation rho(3U, 3U);
  const std::vector<f64> a{0.1, 0.2, 0.3, 0.4};
  const std::vector<f64> b_same{1.0, 2.0, 3.0, 4.0};
  const std::vector<f64> b_flip{4.0, 3.0, 2.0, 1.0};
  const std::vector<f64> sparse{kNaN, kNaN, 1.0, 2.0}; // two joint names: below min_names
  ASSERT_TRUE(rho.add_date(std::vector<std::span<const f64>>{a, b_same, sparse}).has_value());
  ASSERT_TRUE(rho.add_date(std::vector<std::span<const f64>>{a, b_flip, sparse}).has_value());
  EXPECT_NEAR(rho.mean(0U, 1U), 0.0, 1e-12);
  EXPECT_EQ(rho.dates(1U, 0U), 2U);
  EXPECT_TRUE(std::isnan(rho.mean(0U, 2U)));
  EXPECT_EQ(rho.dates(0U, 2U), 0U);
  EXPECT_TRUE(std::isnan(rho.mean(1U, 1U)));
  EXPECT_FALSE(rho.add_date(std::vector<std::span<const f64>>{a, b_same}).has_value());
}

TEST(CombineMarginalRankIc, RefusesBadShapesAndBounds) {
  const std::vector<f64> row(30U, 1.0), shorter(29U, 1.0);
  cb::MarginalRankIcScratch scratch;
  EXPECT_FALSE(cb::marginal_rank_ic_day(row, {}, shorter, 20U, scratch).has_value());
  EXPECT_FALSE(cb::marginal_rank_ic_day(row, {}, row, 2U, scratch).has_value());
  const std::vector<std::span<const f64>> twelve(12U, std::span<const f64>(row));
  EXPECT_FALSE(cb::marginal_rank_ic_day(row, twelve, row, 20U, scratch).has_value());
  const std::vector<std::span<const f64>> ragged{std::span<const f64>(shorter)};
  EXPECT_FALSE(cb::marginal_rank_ic_day(row, ragged, row, 20U, scratch).has_value());
  // Too few paired names: a defined call with both ICs NaN, never an error.
  std::vector<f64> label(30U, kNaN);
  for (usize i = 0U; i < 10U; ++i) {
    label[i] = static_cast<f64>(i);
  }
  const auto day = cb::marginal_rank_ic_day(row, {}, label, 20U, scratch);
  ASSERT_TRUE(day.has_value());
  EXPECT_EQ(day->names, 10U);
  EXPECT_TRUE(std::isnan(day->raw_ic));
  EXPECT_TRUE(std::isnan(day->marginal_ic));
}

} // namespace atx_test_v8_combine_marginal_rank_ic
