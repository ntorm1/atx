// Gate for the alpha layer's compute and strategy stages.
//
// The load-bearing cases here are the ones that caught real defects when this
// layer was first run against a shipped panel:
//
//   * THE AXIS GATE. `vrp_panel.hpp` drops a session whose 21d strip is
//     unavailable but keeps that session's spot in its neighbours' windows, so
//     the emitted rows are a SUBSET of the bar axis the features were computed
//     on. Recomputing a trailing window off the emitted rows silently produces
//     a different number. `SymbolSeries::contiguous` closes it, and
//     `AGapNaNsEveryWindowSpanningIt` is the pin.
//   * THE BLEND CLAMP. Requiring two finite features per row out of a
//     one-feature set scored no row at all and reported an empty book as a
//     successful run.
//   * COST CANCELLATION. Under a uniform cost tier the charge is a within-date
//     constant and cancels EXACTLY in the paired selection excess. That is why
//     the transaction-cost assumption cannot move the alpha measurement, and
//     it is asserted rather than argued.

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/alpha/compute.hpp"
#include "atx/vol/alpha/frame.hpp"
#include "atx/vol/alpha/registry.hpp"
#include "atx/vol/alpha/strategy.hpp"

namespace {

using namespace atx::vol::alpha;

PanelFrame load(const std::string &text) {
  std::istringstream in(text);
  auto got = PanelFrame::read_tsv(in);
  EXPECT_TRUE(got) << (got ? "" : got.error().to_string());
  return got ? std::move(*got) : PanelFrame{};
}

// Two symbols on a shared four-session axis. AAA is present on every session;
// BBB is MISSING 2026-01-06, exactly the emitter's row-drop case.
const char *const kGappy = "symbol\tdate\tspot\tiv_fair_21d\tiv_fair_63d\trv_fwd_21d\n"
                           "AAA\t2026-01-05\t100\t0.20\t0.22\t0.25\n"
                           "AAA\t2026-01-06\t101\t0.21\t0.23\t0.26\n"
                           "AAA\t2026-01-07\t102\t0.22\t0.24\t0.27\n"
                           "AAA\t2026-01-08\t103\t0.23\t0.25\t0.28\n"
                           "BBB\t2026-01-05\t50\t0.30\t0.31\t0.35\n"
                           "BBB\t2026-01-07\t52\t0.32\t0.33\t0.36\n"
                           "BBB\t2026-01-08\t53\t0.33\t0.34\t0.37\n";

// ── The axis gate ───────────────────────────────────────────────────────────

TEST(AlphaCompute, GlobalAxisAdjacencyNotOwnRowAdjacency) {
  const PanelFrame f = load(kGappy);
  auto series = group_by_symbol(f);
  ASSERT_TRUE(series) << series.error().to_string();
  ASSERT_EQ(series->size(), 2U);

  const SymbolSeries *bbb = nullptr;
  for (const SymbolSeries &s : *series) {
    if (s.symbol == "BBB") {
      bbb = &s;
    }
  }
  ASSERT_NE(bbb, nullptr);
  ASSERT_EQ(bbb->size(), 3U);
  // Row 0 has nothing before it. Row 1 is 2026-01-07, whose predecessor on the
  // GLOBAL axis is 2026-01-06 — a session BBB does not have — so the step is
  // not an adjacency even though the two rows are adjacent in BBB's own array.
  EXPECT_EQ(bbb->contiguous[0], 1U);
  EXPECT_EQ(bbb->contiguous[1], 0U);
  EXPECT_EQ(bbb->contiguous[2], 1U);
}

TEST(AlphaCompute, AGapNaNsEveryWindowSpanningIt) {
  const PanelFrame f = load(kGappy);
  const FeatureRegistry reg = builtin_features();
  const std::vector<std::string> pats{"f0_log_rv1"};
  const auto sel = reg.select(pats);
  ASSERT_TRUE(sel);
  auto out = evaluate(f, *sel);
  ASSERT_TRUE(out) << out.error().to_string();
  const std::vector<double> &f0 = out->values.at("f0_log_rv1");

  // AAA rows 0..3 are frame rows 0..3; row 0 has no prior bar, rows 1..3 do.
  EXPECT_TRUE(std::isnan(f0[0]));
  EXPECT_TRUE(std::isfinite(f0[1]));
  EXPECT_TRUE(std::isfinite(f0[3]));
  // BBB rows are frame rows 4..6. Row 5 (2026-01-07) steps over the missing
  // 2026-01-06, so its one-step return is NOT a one-session return and must be
  // declined rather than computed off the wrong pair.
  EXPECT_TRUE(std::isnan(f0[4]));
  EXPECT_TRUE(std::isnan(f0[5]));
  EXPECT_TRUE(std::isfinite(f0[6]));
}

TEST(AlphaCompute, ANonAscendingSymbolGroupIsAnError) {
  // The panel's (symbol, session) sort is a gate-tested contract. Re-sorting
  // here would mask a violation of it, so it is refused instead.
  const PanelFrame f = load("symbol\tdate\tspot\tiv_fair_21d\tiv_fair_63d\n"
                            "AAA\t2026-01-07\t100\t0.2\t0.22\n"
                            "AAA\t2026-01-05\t101\t0.2\t0.22\n");
  const auto series = group_by_symbol(f);
  ASSERT_FALSE(series);
  EXPECT_EQ(series.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(AlphaCompute, SurfaceOnlyFeaturesAreNamedNotSilentlyNaN) {
  const PanelFrame f = load(kGappy);
  const FeatureRegistry reg = builtin_features();
  const std::vector<std::string> pats{"f17_slope_126d", "f11_rr25_21d", "f4_term_slope"};
  const auto sel = reg.select(pats);
  ASSERT_TRUE(sel);
  auto out = evaluate(f, *sel);
  ASSERT_TRUE(out);
  EXPECT_EQ(out->values.count("f4_term_slope"), 1U);
  ASSERT_EQ(out->needs_surface.size(), 2U);
  EXPECT_EQ(out->values.count("f17_slope_126d"), 0U);
}

TEST(AlphaCompute, MarketCoverageIsReportedAgainstTheGlobalAxis) {
  const PanelFrame f = load(kGappy);
  auto series = group_by_symbol(f);
  ASSERT_TRUE(series);
  auto m = market_from(*series, "BBB", /*global_sessions=*/4);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->symbol, "BBB");
  EXPECT_EQ(m->date.size(), 3U);
  EXPECT_NEAR(m->coverage_fraction(), 0.75, 1e-12);
  EXPECT_FALSE(market_from(*series, "ZZZ", 4));
}

// ── Ranking and blending ────────────────────────────────────────────────────

TEST(AlphaStrategy, RankWithinIsMidRankAndSkipsNonFinite) {
  const std::vector<double> v{1.0, 3.0, 3.0, std::nan(""), 0.0};
  const std::vector<std::size_t> rows{0, 1, 2, 3, 4};
  std::vector<double> out(5, std::nan(""));
  rank_within(v, rows, out);
  // Four live values: 0.0 -> 0/4 + 0.5*1/4 = 0.125; 1.0 -> 1/4 + 0.125 = 0.375;
  // the two 3.0s tie -> 2/4 + 0.5*2/4 = 0.75.
  EXPECT_NEAR(out[4], 0.125, 1e-12);
  EXPECT_NEAR(out[0], 0.375, 1e-12);
  EXPECT_NEAR(out[1], 0.75, 1e-12);
  EXPECT_NEAR(out[2], 0.75, 1e-12);
  EXPECT_TRUE(std::isnan(out[3]));
}

TEST(AlphaStrategy, BuyLowIsFlippedSoHighAlwaysMeansAttractive) {
  const PanelFrame f = load(kGappy);
  auto dates = group_by_date(f);
  ASSERT_TRUE(dates);
  const FeatureRegistry reg = builtin_features();

  // Two rows on 2026-01-05: AAA (frame row 0) and BBB (frame row 4).
  std::unordered_map<std::string, std::vector<double>> vals;
  std::vector<double> col(f.rows(), std::nan(""));
  col[0] = 0.10; // AAA low
  col[4] = 0.90; // BBB high
  vals.emplace("f16_iv_vov_21d", col); // prior BuyLow
  const std::vector<std::string> pats{"f16_iv_vov_21d"};
  const auto sel = reg.select(pats);
  ASSERT_TRUE(sel);

  BlendConfig cfg;
  auto res = blend(f, *dates, *sel, vals, cfg);
  ASSERT_TRUE(res) << res.error().to_string();
  // BuyLow: the LOW raw value must end up with the HIGH score.
  EXPECT_GT(res->score[0], res->score[4]);
  EXPECT_EQ(res->required_per_row, 1U);
}

TEST(AlphaStrategy, AFeatureWithNoPublishedPriorIsRefusedNotFitted) {
  const PanelFrame f = load(kGappy);
  auto dates = group_by_date(f);
  ASSERT_TRUE(dates);
  const FeatureRegistry reg = builtin_features();
  const std::vector<std::string> pats{"f11_rr25_21d", "f16_iv_vov_21d"};
  const auto sel = reg.select(pats);
  ASSERT_TRUE(sel);

  std::unordered_map<std::string, std::vector<double>> vals;
  std::vector<double> col(f.rows(), 0.5);
  vals.emplace("f16_iv_vov_21d", col);
  vals.emplace("f11_rr25_21d", col);

  BlendConfig cfg;
  auto res = blend(f, *dates, *sel, vals, cfg);
  ASSERT_TRUE(res);
  ASSERT_EQ(res->refused.size(), 1U);
  EXPECT_EQ(res->refused[0], "f11_rr25_21d");
  ASSERT_EQ(res->used.size(), 1U);
  EXPECT_EQ(res->used[0], "f16_iv_vov_21d");
}

TEST(AlphaStrategy, ABlendOfOnlyUnpricedFeaturesIsAnError) {
  const PanelFrame f = load(kGappy);
  auto dates = group_by_date(f);
  ASSERT_TRUE(dates);
  const FeatureRegistry reg = builtin_features();
  const std::vector<std::string> pats{"f11_rr25_21d"};
  const auto sel = reg.select(pats);
  ASSERT_TRUE(sel);
  std::unordered_map<std::string, std::vector<double>> vals;
  vals.emplace("f11_rr25_21d", std::vector<double>(f.rows(), 0.5));
  BlendConfig cfg;
  const auto res = blend(f, *dates, *sel, vals, cfg);
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ── Significance ────────────────────────────────────────────────────────────

TEST(AlphaStrategy, NeweyWestAtLagZeroIsThePlainT) {
  const std::vector<double> x{1.0, 2.0, 3.0, 4.0, 5.0};
  // mean 3, population variance 2, se = sqrt(2/5).
  const double expect = 3.0 / std::sqrt(2.0 / 5.0);
  EXPECT_NEAR(newey_west_t(x, 0), expect, 1e-9);
}

TEST(AlphaStrategy, NeweyWestShrinksTOnAPositivelyAutocorrelatedSeries) {
  // A slow ramp is strongly positively autocorrelated; the HAC correction must
  // reduce the t, which is the whole reason an overlapping hold needs one.
  std::vector<double> x;
  for (int i = 0; i < 60; ++i) {
    x.push_back(1.0 + 0.01 * static_cast<double>(i));
  }
  const double t0 = newey_west_t(x, 0);
  const double t20 = newey_west_t(x, 20);
  EXPECT_GT(t0, 0.0);
  EXPECT_GT(t20, 0.0);
  EXPECT_LT(t20, t0);
}

TEST(AlphaStrategy, NeweyWestIsNaNOnADegenerateSeries) {
  const std::vector<double> flat{2.0, 2.0, 2.0, 2.0};
  EXPECT_TRUE(std::isnan(newey_west_t(flat, 0)));
  EXPECT_TRUE(std::isnan(newey_west_t(std::vector<double>{1.0}, 0)));
}

// ── The book ────────────────────────────────────────────────────────────────

namespace {
// Four names on each of two sessions, so a top-2 selection is a real choice.
const char *const kBook = "symbol\tdate\tspot\tiv_fair_21d\tiv_fair_63d\trv_fwd_21d\n"
                          "AAA\t2026-01-05\t100\t0.20\t0.22\t0.30\n"
                          "AAA\t2026-01-06\t100\t0.20\t0.22\t0.30\n"
                          "BBB\t2026-01-05\t100\t0.20\t0.22\t0.28\n"
                          "BBB\t2026-01-06\t100\t0.20\t0.22\t0.28\n"
                          "CCC\t2026-01-05\t100\t0.20\t0.22\t0.22\n"
                          "CCC\t2026-01-06\t100\t0.20\t0.22\t0.22\n"
                          "DDD\t2026-01-05\t100\t0.20\t0.22\t0.20\n"
                          "DDD\t2026-01-06\t100\t0.20\t0.22\t0.20\n";
} // namespace

TEST(AlphaStrategy, PerfectForesightSelectsTheTopAndBeatsTheFloor) {
  const PanelFrame f = load(kBook);
  auto dates = group_by_date(f);
  ASSERT_TRUE(dates);
  auto pnl = dh_straddle_pnl_vol_points(f);
  ASSERT_TRUE(pnl);
  // Scores equal to the P&L: an oracle. Selection must pick AAA and BBB.
  StrategyConfig cfg;
  cfg.max_names = 2;
  cfg.horizon_sessions = 1;
  auto card = run(f, *dates, *pnl, *pnl, cfg);
  ASSERT_TRUE(card) << card.error().to_string();
  ASSERT_EQ(card->n_dates, 2U);
  // rv - iv in vol points: AAA +10, BBB +8, CCC +2, DDD 0. Top-2 mean 9, floor
  // mean 5, so the gross excess is exactly 4.
  EXPECT_NEAR(card->per_date[0].selected_gross, 9.0, 1e-12);
  EXPECT_NEAR(card->per_date[0].floor_gross, 5.0, 1e-12);
  EXPECT_NEAR(card->mean_excess_gross, 4.0, 1e-12);
}

TEST(AlphaStrategy, AConstantScoreEarnsExactlyTheFloor) {
  // No selection information at all: the top-N of a tied ranking is still a
  // subset, but with n = N the two books coincide and the excess must be 0.
  const PanelFrame f = load(kBook);
  auto dates = group_by_date(f);
  ASSERT_TRUE(dates);
  auto pnl = dh_straddle_pnl_vol_points(f);
  ASSERT_TRUE(pnl);
  const std::vector<double> flat(f.rows(), 0.5);
  StrategyConfig cfg;
  cfg.max_names = 4;
  cfg.horizon_sessions = 1;
  auto card = run(f, *dates, flat, *pnl, cfg);
  ASSERT_TRUE(card);
  EXPECT_NEAR(card->mean_excess_gross, 0.0, 1e-12);
  EXPECT_NEAR(card->mean_excess_net, 0.0, 1e-12);
}

TEST(AlphaStrategy, AUniformCostTierCancelsExactlyInTheExcess) {
  // THE REASON THE TRANSACTION-COST ASSUMPTION CANNOT MOVE THE ALPHA. With no
  // measured liquidity column every name lands in the same tier, so the charge
  // is a within-date constant and drops out of the paired difference. The
  // absolute numbers move; the excess does not.
  const PanelFrame f = load(kBook);
  auto dates = group_by_date(f);
  ASSERT_TRUE(dates);
  auto pnl = dh_straddle_pnl_vol_points(f);
  ASSERT_TRUE(pnl);
  StrategyConfig cheap;
  cheap.max_names = 2;
  cheap.horizon_sessions = 1;
  cheap.cost_vp_illiquid = 0.25;
  StrategyConfig dear = cheap;
  dear.cost_vp_illiquid = 5.0;

  auto a = run(f, *dates, *pnl, *pnl, cheap);
  auto b = run(f, *dates, *pnl, *pnl, dear);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_NEAR(a->mean_excess_net, b->mean_excess_net, 1e-12);
  EXPECT_GT(a->mean_selected_net, b->mean_selected_net);
  EXPECT_NEAR(a->mean_selected_net - b->mean_selected_net, 4.75, 1e-12);
}

TEST(AlphaStrategy, ThePhaseSweepHasOneSubSeriesPerHorizonSession) {
  const PanelFrame f = load(kBook);
  auto dates = group_by_date(f);
  ASSERT_TRUE(dates);
  auto pnl = dh_straddle_pnl_vol_points(f);
  ASSERT_TRUE(pnl);
  StrategyConfig cfg;
  cfg.max_names = 2;
  cfg.horizon_sessions = 21;
  auto card = run(f, *dates, *pnl, *pnl, cfg);
  ASSERT_TRUE(card);
  EXPECT_EQ(card->phase_mean_excess_net.size(), 21U);
  // Only two dates exist, so 19 phases are empty and report NaN rather than 0.
  EXPECT_TRUE(std::isnan(card->phase_mean_excess_net[2]));
  EXPECT_NEAR(card->phase_positive_fraction, 1.0, 1e-12);
}

TEST(AlphaStrategy, TheDecontaminatedAxisCarriesNoImpliedLeg) {
  const PanelFrame f = load(kBook);
  auto dh = dh_straddle_pnl_vol_points(f);
  auto rv = forward_rv_vol_points(f);
  ASSERT_TRUE(dh);
  ASSERT_TRUE(rv);
  // AAA: rv_fwd 0.30, iv 0.20 -> dh = +10 vol pts, rv = 30 vol pts.
  EXPECT_NEAR((*dh)[0], 10.0, 1e-9);
  EXPECT_NEAR((*rv)[0], 30.0, 1e-9);
  // The difference is exactly the entry mark, which is the shared leg the
  // adjudicator reports and the cross-read removes.
  EXPECT_NEAR((*rv)[0] - (*dh)[0], 20.0, 1e-9);
}

TEST(AlphaStrategy, ADateThatCannotFormBothBooksFormsNeither) {
  const PanelFrame f = load("symbol\tdate\tspot\tiv_fair_21d\tiv_fair_63d\trv_fwd_21d\n"
                            "AAA\t2026-01-05\t100\t0.20\t0.22\t0.30\n"
                            "AAA\t2026-01-06\t100\t0.20\t0.22\t0.30\n"
                            "BBB\t2026-01-06\t100\t0.20\t0.22\t0.28\n");
  auto dates = group_by_date(f);
  ASSERT_TRUE(dates);
  auto pnl = dh_straddle_pnl_vol_points(f);
  ASSERT_TRUE(pnl);
  const std::vector<double> flat(f.rows(), 0.5);
  StrategyConfig cfg;
  cfg.max_names = 2;
  cfg.horizon_sessions = 1;
  auto card = run(f, *dates, flat, *pnl, cfg);
  ASSERT_TRUE(card);
  // 2026-01-05 has a single admitted name: a top-1-of-1 "selection" against a
  // one-name floor is a comparison of a set with itself.
  ASSERT_EQ(card->n_dates, 1U);
  EXPECT_EQ(card->per_date[0].date, "2026-01-06");
}

TEST(AlphaStrategy, RequireMeasuredLiquidityRefusesAPanelWithoutTheColumn) {
  const PanelFrame f = load(kBook);
  auto dates = group_by_date(f);
  ASSERT_TRUE(dates);
  auto pnl = dh_straddle_pnl_vol_points(f);
  ASSERT_TRUE(pnl);
  const std::vector<double> flat(f.rows(), 0.5);
  StrategyConfig cfg;
  cfg.horizon_sessions = 1;
  cfg.require_measured_liquidity = true;
  const auto card = run(f, *dates, flat, *pnl, cfg);
  ASSERT_FALSE(card);
  EXPECT_EQ(card.error().code(), atx::core::ErrorCode::NotFound);
}

} // namespace
