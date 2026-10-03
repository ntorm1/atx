// combine_marginal_rank_ic_test.cpp — platform v8 F-2: the per-date residualise-then-rank-IC
// kernel behind contract K6 (marginal_ic.json).
//
// Suite: CombineMarginalRankIc

#include <bit>
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
  const std::vector<std::span<const f64>> over(cb::kMaxMarginalRegressors + 1U,
                                               std::span<const f64>(row));
  EXPECT_FALSE(cb::marginal_rank_ic_day(row, over, row, 20U, scratch).has_value());
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

// ---- P9 S1 (marginal verb speed) ----------------------------------------------------------------

std::uint64_t bits(f64 x) { return std::bit_cast<std::uint64_t>(x); }

// CM-6: the book composite plus 33 theme composites are accepted; one more is refused.
TEST(CombineMarginalRankIc, AcceptsThirtyFourRegressors) {
  static_assert(cb::kMaxMarginalRegressors == 34U);
  Rng g{20261003U};
  const usize n = 120U;
  const std::vector<f64> candidate = draws(g, n);
  const std::vector<f64> label = draws(g, n);
  std::vector<std::vector<f64>> columns;
  for (usize j = 0U; j < cb::kMaxMarginalRegressors; ++j) {
    columns.push_back(draws(g, n));
  }
  std::vector<std::span<const f64>> regressors;
  for (const auto &column : columns) {
    regressors.emplace_back(column);
  }
  cb::MarginalRankIcScratch scratch;
  const auto day = cb::marginal_rank_ic_day(candidate, regressors, label, 20U, scratch);
  ASSERT_TRUE(day.has_value());
  EXPECT_EQ(day->names, n);
  EXPECT_TRUE(std::isfinite(day->raw_ic));
  EXPECT_TRUE(std::isfinite(day->marginal_ic));
  const std::span<const f64> first = regressors.front();
  regressors.push_back(first);
  EXPECT_FALSE(cb::marginal_rank_ic_day(candidate, regressors, label, 20U, scratch).has_value());
}

// Dropping the names that are NaN in every candidate input of a date (the marginal verb keeps only
// the date's member names) gives the same bits from every row kernel: ranks, the marginal read-out
// and the pair correlation. The regressor and the label stay finite on the dropped names.
TEST(CombineMarginalRankIc, CompactedNamesKeepBits) {
  Rng g{20261004U};
  const usize n = 240U;
  std::vector<u8> member(n, u8{0});
  std::vector<usize> kept;
  for (usize i = 0U; i < n; ++i) {
    member[i] = (g.uni() < 0.7) ? u8{1} : u8{0};
    if (member[i] != u8{0}) {
      kept.push_back(i);
    }
  }
  std::vector<f64> values = draws(g, n);
  for (f64 &x : values) {
    x = std::round(x * 4.0) / 4.0; // ties
  }
  const std::vector<f64> other = draws(g, n);
  const std::vector<f64> book = draws(g, n);
  const std::vector<f64> label = draws(g, n);
  const usize m = kept.size();
  const auto gather = [&](const std::vector<f64> &full) {
    std::vector<f64> out(m);
    for (usize j = 0U; j < m; ++j) {
      out[j] = full[kept[j]];
    }
    return out;
  };
  std::vector<std::pair<f64, usize>> sorted;
  std::vector<f64> ranks(n), other_ranks(n), ranks_m(m), other_ranks_m(m);
  const std::vector<u8> member_m(m, u8{1});
  ASSERT_TRUE(cb::centred_tied_ranks(values, member, ranks, sorted).has_value());
  ASSERT_TRUE(cb::centred_tied_ranks(other, member, other_ranks, sorted).has_value());
  const auto values_m = gather(values);
  const auto other_m = gather(other);
  ASSERT_TRUE(cb::centred_tied_ranks(values_m, member_m, ranks_m, sorted).has_value());
  ASSERT_TRUE(cb::centred_tied_ranks(other_m, member_m, other_ranks_m, sorted).has_value());
  for (usize j = 0U; j < m; ++j) {
    EXPECT_EQ(bits(ranks_m[j]), bits(ranks[kept[j]])) << j;
  }
  const auto book_m = gather(book);
  const auto label_m = gather(label);
  const std::vector<std::span<const f64>> full_regressors{std::span<const f64>(book)};
  const std::vector<std::span<const f64>> kept_regressors{std::span<const f64>(book_m)};
  cb::MarginalRankIcScratch scratch;
  const auto wide = cb::marginal_rank_ic_day(ranks, full_regressors, label, 20U, scratch);
  const auto narrow = cb::marginal_rank_ic_day(ranks_m, kept_regressors, label_m, 20U, scratch);
  ASSERT_TRUE(wide.has_value());
  ASSERT_TRUE(narrow.has_value());
  EXPECT_EQ(narrow->names, m);
  EXPECT_EQ(wide->names, narrow->names);
  EXPECT_EQ(bits(wide->raw_ic), bits(narrow->raw_ic));
  EXPECT_EQ(bits(wide->marginal_ic), bits(narrow->marginal_ic));
  EXPECT_EQ(wide->spanned, narrow->spanned);
  cb::PairwiseRowCorrelation rho_wide(2U, 20U);
  cb::PairwiseRowCorrelation rho_narrow(2U, 20U);
  ASSERT_TRUE(rho_wide.add_date(std::vector<std::span<const f64>>{ranks, other_ranks}).has_value());
  ASSERT_TRUE(
      rho_narrow.add_date(std::vector<std::span<const f64>>{ranks_m, other_ranks_m}).has_value());
  EXPECT_EQ(rho_wide.dates(0U, 1U), 1U);
  EXPECT_EQ(bits(rho_wide.sum(0U, 1U)), bits(rho_narrow.sum(1U, 0U)));
}

// A small K-row pair world: rows with about 15% NaN names over a number of dates.
struct PairWorld {
  static constexpr usize kRows = 5U;
  static constexpr usize kNames = 60U;
  static constexpr usize kDates = 25U;
  std::vector<std::vector<f64>> data; // date d, row r: data[d * kRows + r]
  PairWorld() {
    Rng g{20261005U};
    data.resize(kDates * kRows);
    for (auto &row : data) {
      row = draws(g, kNames);
      for (f64 &x : row) {
        if (g.uni() < 0.15) {
          x = kNaN;
        }
      }
    }
  }
  [[nodiscard]] std::vector<std::span<const f64>> date(usize d) const {
    std::vector<std::span<const f64>> rows;
    for (usize r = 0U; r < kRows; ++r) {
      rows.emplace_back(data[d * kRows + r]);
    }
    return rows;
  }
  // Every date through add_date.
  [[nodiscard]] bool run(cb::PairwiseRowCorrelation &rho) const {
    for (usize d = 0U; d < kDates; ++d) {
      if (!rho.add_date(date(d)).has_value()) {
        return false;
      }
    }
    return true;
  }
};

void expect_same_pair(const cb::PairwiseRowCorrelation &x, const cb::PairwiseRowCorrelation &y,
                      usize a, usize b) {
  EXPECT_EQ(bits(x.sum(a, b)), bits(y.sum(a, b))) << a << "," << b;
  EXPECT_EQ(x.dates(a, b), y.dates(a, b)) << a << "," << b;
  EXPECT_EQ(bits(x.mean(a, b)), bits(y.mean(a, b))) << a << "," << b;
}

// day_values then accumulate is add_date, bit for bit (the banded verb's date-ordered sum).
TEST(CombineMarginalRankIc, PairDayValuesThenAccumulateIsAddDate) {
  const PairWorld w;
  cb::PairwiseRowCorrelation serial(PairWorld::kRows, 20U);
  cb::PairwiseRowCorrelation split(PairWorld::kRows, 20U);
  ASSERT_EQ(split.computed_pairs().size(), PairWorld::kRows * (PairWorld::kRows - 1U) / 2U);
  std::vector<f64> values(split.computed_pairs().size());
  ASSERT_TRUE(w.run(serial));
  for (usize d = 0U; d < PairWorld::kDates; ++d) {
    ASSERT_TRUE(split.day_values(w.date(d), values).has_value());
    ASSERT_TRUE(split.accumulate(values).has_value());
  }
  for (usize a = 0U; a < PairWorld::kRows; ++a) {
    for (usize b = a + 1U; b < PairWorld::kRows; ++b) {
      expect_same_pair(serial, split, a, b);
    }
  }
  EXPECT_GT(serial.dates(0U, 1U), 0U);
  std::vector<f64> short_values(values.size() - 1U);
  EXPECT_FALSE(split.day_values(w.date(0U), short_values).has_value());
  EXPECT_FALSE(split.accumulate(short_values).has_value());
}

// --candidates: only pairs with a listed row are computed; those keep their bits.
TEST(CombineMarginalRankIc, RestrictToKeepsListedPairs) {
  const PairWorld w;
  cb::PairwiseRowCorrelation full(PairWorld::kRows, 20U);
  cb::PairwiseRowCorrelation listed(PairWorld::kRows, 20U);
  const std::vector<u8> flags{u8{0}, u8{1}, u8{0}, u8{0}, u8{1}};
  ASSERT_TRUE(listed.restrict_to(flags).has_value());
  const std::vector<std::pair<usize, usize>> expected{{0U, 1U}, {0U, 4U}, {1U, 2U}, {1U, 3U},
                                                      {1U, 4U}, {2U, 4U}, {3U, 4U}};
  const auto computed = listed.computed_pairs();
  const std::vector<std::pair<usize, usize>> actual(computed.begin(), computed.end());
  EXPECT_EQ(actual, expected);
  ASSERT_TRUE(w.run(full));
  ASSERT_TRUE(w.run(listed));
  for (usize a = 0U; a < PairWorld::kRows; ++a) {
    for (usize b = a + 1U; b < PairWorld::kRows; ++b) {
      if (flags[a] != u8{0} || flags[b] != u8{0}) {
        expect_same_pair(full, listed, a, b);
      } else {
        EXPECT_EQ(listed.dates(a, b), 0U);
        EXPECT_TRUE(std::isnan(listed.mean(a, b)));
      }
    }
  }
  EXPECT_FALSE(listed.restrict_to(flags).has_value()); // after the first date
  cb::PairwiseRowCorrelation fresh(PairWorld::kRows, 20U);
  EXPECT_FALSE(fresh.restrict_to(std::vector<u8>(PairWorld::kRows - 1U, u8{1})).has_value());
}

// --pair-cache: a seeded pair takes the cached sum and count and is not computed again; every
// pair ends with the bits of the run that computed all of them. Bad seeds change nothing.
TEST(CombineMarginalRankIc, SeededPairsEqualComputedPairs) {
  const PairWorld w;
  cb::PairwiseRowCorrelation full(PairWorld::kRows, 20U);
  ASSERT_TRUE(w.run(full));
  cb::PairwiseRowCorrelation seeded(PairWorld::kRows, 20U);
  const std::vector<cb::PairSeed> seeds{{3U, 0U, full.sum(0U, 3U), full.dates(0U, 3U)},
                                        {1U, 2U, full.sum(1U, 2U), full.dates(1U, 2U)}};
  ASSERT_TRUE(seeded.seed(seeds).has_value());
  EXPECT_EQ(seeded.computed_pairs().size(), PairWorld::kRows * (PairWorld::kRows - 1U) / 2U - 2U);
  ASSERT_TRUE(w.run(seeded));
  for (usize a = 0U; a < PairWorld::kRows; ++a) {
    for (usize b = a + 1U; b < PairWorld::kRows; ++b) {
      expect_same_pair(full, seeded, a, b);
    }
  }
  EXPECT_FALSE(seeded.seed(std::vector<cb::PairSeed>{{0U, 4U, 0.0, 0U}}).has_value()); // started
  cb::PairwiseRowCorrelation bad(PairWorld::kRows, 20U);
  const std::vector<std::vector<cb::PairSeed>> refused{
      {{2U, 2U, 0.0, 0U}},                       // not a pair
      {{0U, PairWorld::kRows, 0.0, 0U}},         // out of range
      {{0U, 1U, 3.0, 2U}},                       // |sum| > dates
      {{0U, 1U, kNaN, 2U}},                      // not finite
      {{0U, 1U, 0.5, 2U}, {1U, 0U, 0.5, 2U}},    // one pair twice
      {{0U, 2U, 0.5, 2U}, {0U, 0U, 0.0, 0U}}};   // a valid seed before a bad one
  for (const auto &batch : refused) {
    EXPECT_FALSE(bad.seed(batch).has_value()) << batch.size();
    EXPECT_EQ(bad.computed_pairs().size(), PairWorld::kRows * (PairWorld::kRows - 1U) / 2U);
  }
  ASSERT_TRUE(bad.seed(std::vector<cb::PairSeed>{{0U, 1U, 0.5, 2U}}).has_value());
  EXPECT_FALSE(bad.seed(std::vector<cb::PairSeed>{{1U, 0U, 0.5, 2U}}).has_value()); // seeded
  EXPECT_EQ(bad.dates(0U, 1U), 2U);
  EXPECT_EQ(bits(bad.sum(1U, 0U)), bits(0.5));
}

} // namespace atx_test_v8_combine_marginal_rank_ic
