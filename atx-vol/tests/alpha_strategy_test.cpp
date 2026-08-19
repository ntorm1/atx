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

// vrp_panel_v4 carries the emitter's own bar-axis position. When it is there,
// adjacency is read rather than inferred -- which is not a refinement but a
// capability: the date-union heuristic is BLIND on a one-symbol panel, since
// the union collapses to that symbol's own rows and every step looks adjacent.
// A single name is exactly the case a per-name study runs.
const char *const kOneSymbolWithBarIndex =
    "symbol\tdate\tspot\tiv_fair_21d\tiv_fair_63d\trv_fwd_21d\tbar_index\n"
    "AAA\t2026-01-05\t100\t0.20\t0.22\t0.25\t0\n"
    "AAA\t2026-01-06\t101\t0.21\t0.23\t0.26\t1\n"
    "AAA\t2026-01-08\t102\t0.22\t0.24\t0.27\t3\n"  // bar 2 missing: a real hole
    "AAA\t2026-01-09\t103\t0.23\t0.25\t0.28\t4\n";

TEST(AlphaCompute, BarIndexDetectsAGapTheDateUnionAxisCannotSee) {
  const PanelFrame f = load(kOneSymbolWithBarIndex);
  auto series = group_by_symbol(f);
  ASSERT_TRUE(series) << series.error().to_string();
  ASSERT_EQ(series->size(), 1U);
  const SymbolSeries &s = (*series)[0];
  ASSERT_EQ(s.size(), 4U);
  EXPECT_EQ(s.contiguous[0], 1U);
  EXPECT_EQ(s.contiguous[1], 1U);
  EXPECT_EQ(s.contiguous[2], 0U) << "bar_index jumped 1 -> 3 and the gate missed it";
  EXPECT_EQ(s.contiguous[3], 1U);
}

// The control that makes the test above mean something: strip the column and
// the identical rows report NO gap, because on a one-symbol panel the global
// date axis IS that symbol's rows. This is the blindness v4 removes.
TEST(AlphaCompute, WithoutBarIndexTheSameOneSymbolPanelLooksGapFree) {
  std::string text = kOneSymbolWithBarIndex;
  // Drop the trailing bar_index field from the header and every row.
  std::string stripped;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t tab = line.rfind('\t');
    ASSERT_NE(tab, std::string::npos);
    stripped += line.substr(0, tab);
    stripped += '\n';
  }
  const PanelFrame f = load(stripped);
  auto series = group_by_symbol(f);
  ASSERT_TRUE(series) << series.error().to_string();
  ASSERT_EQ(series->size(), 1U);
  const SymbolSeries &s = (*series)[0];
  ASSERT_EQ(s.size(), 4U);
  for (std::size_t i = 0; i < s.size(); ++i) {
    EXPECT_EQ(s.contiguous[i], 1U) << i;
  }
}

// A v4 panel's whole promise: the emitted axis IS the bar axis, so a symbol
// present on every emitted session reports contiguous everywhere and windows
// stop being gated away. Dates here deliberately SKIP a weekend, which the
// date-union axis handles only because every symbol skips it too -- bar_index
// makes that independent of the calendar.
const char *const kV4Contiguous =
    "symbol\tdate\tspot\tiv_fair_21d\tiv_fair_63d\trv_fwd_21d\tbar_index\n"
    "AAA\t2026-01-09\t100\t0.20\t0.22\t0.25\t7\n"
    "AAA\t2026-01-12\t101\t0.21\t0.23\t0.26\t8\n"
    "AAA\t2026-01-13\t102\t0.22\t0.24\t0.27\t9\n"
    "BBB\t2026-01-09\t50\tnan\tnan\t0.35\t7\n" // strip-less v4 row: iv NaN, spot good
    "BBB\t2026-01-12\t52\t0.32\t0.33\t0.36\t8\n"
    "BBB\t2026-01-13\t53\t0.33\t0.34\t0.37\t9\n";

TEST(AlphaCompute, V4PanelIsContiguousEverywhereIncludingStriplessRows) {
  const PanelFrame f = load(kV4Contiguous);
  auto series = group_by_symbol(f);
  ASSERT_TRUE(series) << series.error().to_string();
  ASSERT_EQ(series->size(), 2U);
  for (const SymbolSeries &s : *series) {
    ASSERT_EQ(s.size(), 3U) << s.symbol;
    for (std::size_t i = 0; i < s.size(); ++i) {
      EXPECT_EQ(s.contiguous[i], 1U) << s.symbol << " " << i;
    }
  }
}

// A raw spot series steps across an unadjusted corporate action. That step is
// a share-count ratio, not a return, and every window over it is fiction --
// so the axis gate must treat it exactly like a missing session. Here bar 1
// -> 2 is a 10:1 split: adjacent on the bar axis, NOT a usable step.
const char *const kUnadjustedSplit =
    "symbol\tdate\tspot\tiv_fair_21d\tiv_fair_63d\trv_fwd_21d\tbar_index\n"
    "AAA\t2026-01-05\t2400\t0.20\t0.22\t0.25\t0\n"
    "AAA\t2026-01-06\t2416\t0.21\t0.23\t0.26\t1\n"
    "AAA\t2026-01-07\t252\t0.22\t0.24\t0.27\t2\n"
    "AAA\t2026-01-08\t257\t0.23\t0.25\t0.28\t3\n";

TEST(AlphaCompute, AnUnadjustedCorporateActionStepIsNotAnAdjacency) {
  const PanelFrame f = load(kUnadjustedSplit);
  auto series = group_by_symbol(f);
  ASSERT_TRUE(series) << series.error().to_string();
  ASSERT_EQ(series->size(), 1U);
  const SymbolSeries &s = (*series)[0];
  ASSERT_EQ(s.size(), 4U);
  EXPECT_EQ(s.contiguous[1], 1U) << "an ordinary +0.7% step must stay usable";
  EXPECT_EQ(s.contiguous[2], 0U) << "the split step passed the gate";
  EXPECT_EQ(s.contiguous[3], 1U) << "the step AFTER the split is a real return";
  // bar_index says adjacent everywhere — so the split is caught by the return
  // magnitude, not by the axis, which is the point of carrying both.
  for (std::size_t i = 0; i < s.size(); ++i) {
    EXPECT_EQ(s.bar_index[i], static_cast<double>(i));
  }
}

// The threshold is DERIVED from vrp_panel's rv plausibility gate, not chosen.
// Restating it in this layer (which links only atx::core) is only safe while
// the derivation still holds, so pin it.
TEST(AlphaCompute, StepThresholdMatchesTheEmitterDerivation) {
  // 3.0 = kVrpMaxPlausibleRvFwd; 20 = the return terms in a 21-session window.
  const double derived = std::sqrt(3.0 * 3.0 * 20.0 / 252.0);
  EXPECT_NEAR(kAlphaImplausibleStepReturn, derived, 1e-15);
}

// ── Round 11: the semivariance decomposition ────────────────────────────────
//
// A 26-session single-name panel with a deterministic path that genuinely
// changes sign, so RS+ and RS- are both non-trivial. bar_index makes the axis
// exact so no window is gated away.
[[nodiscard]] std::string make_semivar_panel(int n) {
  std::string out = "symbol\tdate\tspot\tiv_fair_21d\tiv_fair_63d\trv_fwd_21d\tbar_index\n";
  double s = 100.0;
  for (int i = 0; i < n; ++i) {
    // Alternating-magnitude drift: down moves are deliberately LARGER than up
    // moves, so RS- > RS+ and a test that mixed them up fails loudly.
    const double r = (i % 3 == 0) ? -0.03 : 0.01;
    s *= std::exp(r);
    char buf[256];
    std::snprintf(buf, sizeof buf, "AAA\t2026-%02d-%02d\t%.10f\t0.20\t0.22\t0.25\t%d\n",
                  1 + i / 28, 1 + i % 28, s, i);
    out += buf;
  }
  return out;
}

// THE IDENTITY THE DECOMPOSITION RESTS ON: RS+ + RS- == RV, exactly. If it
// does not hold, the signed jump variation is not a jump measure -- the
// continuous part fails to cancel in the difference and f24 is measuring the
// vol level instead.
TEST(AlphaCompute, UpsideAndDownsideSemivarianceSumToTotalVariance) {
  const PanelFrame f = load(make_semivar_panel(40));
  const FeatureRegistry reg = builtin_features();
  const std::vector<std::string> pats{"f2_log_rv21", "f22_semivar_dn_21d", "f23_semivar_up_21d",
                                      "f24_signed_jump_21d"};
  const auto sel = reg.select(pats);
  ASSERT_TRUE(sel);
  auto out = evaluate(f, *sel);
  ASSERT_TRUE(out) << out.error().to_string();
  const std::vector<double> &rv = out->values.at("f2_log_rv21");
  const std::vector<double> &dn = out->values.at("f22_semivar_dn_21d");
  const std::vector<double> &up = out->values.at("f23_semivar_up_21d");
  const std::vector<double> &sj = out->values.at("f24_signed_jump_21d");

  std::size_t checked = 0;
  for (std::size_t i = 21; i < f.rows(); ++i) {
    ASSERT_TRUE(std::isfinite(rv[i])) << i;
    ASSERT_TRUE(std::isfinite(dn[i]) && std::isfinite(up[i])) << i;
    // The features are logs; the identity lives in variance space.
    const double v_dn = std::exp(dn[i]);
    const double v_up = std::exp(up[i]);
    EXPECT_NEAR(v_dn + v_up, std::exp(rv[i]), 1e-12 * std::exp(rv[i])) << i;
    // ...and f24 is that same pair, scaled to a share.
    EXPECT_NEAR(sj[i], (v_up - v_dn) / (v_up + v_dn), 1e-12) << i;
    ++checked;
  }
  ASSERT_GT(checked, 0u);
}

TEST(AlphaCompute, DownsideSemivarianceIsTheLargerLegOnADownwardSkewedPath) {
  // Anti-vacuity for the test above: the identity would hold even if the two
  // legs were swapped, so pin which is which on a path built to be asymmetric.
  const PanelFrame f = load(make_semivar_panel(40));
  const FeatureRegistry reg = builtin_features();
  const std::vector<std::string> pats{"f22_semivar_dn_21d", "f23_semivar_up_21d"};
  const auto sel = reg.select(pats);
  ASSERT_TRUE(sel);
  auto out = evaluate(f, *sel);
  ASSERT_TRUE(out) << out.error().to_string();
  EXPECT_GT(out->values.at("f22_semivar_dn_21d")[30], out->values.at("f23_semivar_up_21d")[30]);
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

// A cross-sectional proxy's step return is the mean of the one-session log
// returns of exactly the symbols that SAW that session pair as adjacent bars.
// On kGappy: BBB is missing 2026-01-06, so its 01-05 -> 01-07 two-day return
// must contribute to NEITHER of the two steps it spans.
TEST(AlphaCompute, CrossSectionProxyAveragesOnlyTheReturnsThatSpanOneSession) {
  const PanelFrame f = load(kGappy);
  auto series = group_by_symbol(f);
  ASSERT_TRUE(series);
  auto m = market_from_cross_section(*series, /*global_sessions=*/4, /*min_names=*/1);
  ASSERT_TRUE(m) << m.error().to_string();
  EXPECT_EQ(m->symbol, "@xsec");
  // The proxy lives on the UNION calendar, so its coverage is 100% by
  // construction — the property the single-symbol proxy could not deliver.
  ASSERT_EQ(m->date.size(), 4U);
  EXPECT_NEAR(m->coverage_fraction(), 1.0, 1e-12);

  const double into_0106 = std::log(m->spot[1] / m->spot[0]);
  const double into_0107 = std::log(m->spot[2] / m->spot[1]);
  const double into_0108 = std::log(m->spot[3] / m->spot[2]);
  EXPECT_NEAR(into_0106, std::log(101.0 / 100.0), 1e-12);          // AAA alone
  EXPECT_NEAR(into_0107, std::log(102.0 / 101.0), 1e-12);          // AAA alone
  EXPECT_NEAR(into_0108,
              0.5 * (std::log(103.0 / 102.0) + std::log(53.0 / 52.0)), 1e-12);
}

// A step with fewer than `min_names` contributors is refused, not fabricated:
// the spot at that session is NaN, so any regression window that touches the
// step reads a NaN return and declines. The union axis itself is kept intact —
// dropping the DATE would silently glue the two neighbouring sessions into a
// fake one-session interval.
TEST(AlphaCompute, CrossSectionProxyRefusesAnUnderpopulatedStep) {
  const PanelFrame f = load(kGappy);
  auto series = group_by_symbol(f);
  ASSERT_TRUE(series);
  auto m = market_from_cross_section(*series, /*global_sessions=*/4, /*min_names=*/2);
  ASSERT_TRUE(m);
  ASSERT_EQ(m->date.size(), 4U); // the axis survives even where the data don't
  EXPECT_TRUE(std::isfinite(m->spot[0]));
  EXPECT_TRUE(std::isnan(m->spot[1])); // only AAA saw 01-05 -> 01-06
  EXPECT_TRUE(std::isnan(m->spot[2])); // only AAA saw 01-06 -> 01-07
  EXPECT_TRUE(std::isfinite(m->spot[3])); // both saw 01-07 -> 01-08
}

namespace {

// Two symbols, n shared sessions, deterministic but decorrelated return
// paths — enough structure for a 63-window regression to have positive
// variance on both legs.
[[nodiscard]] std::string make_two_symbol_panel(int n) {
  std::string out = "symbol\tdate\tspot\tiv_fair_21d\tiv_fair_63d\trv_fwd_21d\tbar_index\n";
  for (const char *sym : {"AAA", "BBB"}) {
    double s = 100.0;
    for (int i = 0; i < n; ++i) {
      const double r = (sym[0] == 'A') ? 0.01 * static_cast<double>(i % 5 - 2)
                                       : 0.008 * static_cast<double>(i % 7 - 3);
      s *= std::exp(r);
      char buf[256];
      std::snprintf(buf, sizeof buf, "%s\t2026-%02d-%02d\t%.10f\t0.20\t0.22\t0.25\t%d\n", sym,
                    1 + i / 28, 1 + i % 28, s, i);
      out += buf;
    }
  }
  return out;
}

} // namespace

// End to end: the cross-sectional proxy is what makes f15/f27 computable on a
// panel with no designated market symbol. This is the round-11 fix for the
// SPY proxy covering 207/249 sessions and starving both features.
TEST(AlphaCompute, IdioAndSysvolShareAreFiniteUnderTheCrossSectionProxy) {
  const PanelFrame f = load(make_two_symbol_panel(70));
  auto series = group_by_symbol(f);
  ASSERT_TRUE(series);
  auto m = market_from_cross_section(*series, /*global_sessions=*/70, /*min_names=*/2);
  ASSERT_TRUE(m);

  const FeatureRegistry reg = builtin_features();
  const std::vector<std::string> pats{"f15_idio_share", "f27_sysvol_share_63d"};
  const auto sel = reg.select(pats);
  ASSERT_TRUE(sel);
  auto out = evaluate(f, *sel, &*m);
  ASSERT_TRUE(out) << out.error().to_string();
  const std::vector<double> &idio = out->values.at("f15_idio_share");
  const std::vector<double> &sys = out->values.at("f27_sysvol_share_63d");

  std::size_t checked = 0;
  for (std::size_t r = 0; r < f.rows(); ++r) {
    if (!std::isfinite(idio[r])) {
      continue;
    }
    EXPECT_GE(idio[r], 0.0) << r;
    EXPECT_LE(idio[r], 1.0) << r;
    ASSERT_TRUE(std::isfinite(sys[r])) << r;
    EXPECT_NEAR(sys[r], 1.0 - idio[r], 1e-12) << r;
    ++checked;
  }
  // Rows 63.. of each 70-row symbol qualify: 7 per symbol.
  EXPECT_EQ(checked, 14U);
}

// ── Earnings calendar (f28..f30) ────────────────────────────────────────────

namespace {

// EEE: inverted term structure (0.30/0.25) — sigma_E extractable. FFF: upward
// (0.20/0.22) — sigma_E^2 would be negative and must be declined.
[[nodiscard]] std::string make_earner_panel(int n) {
  std::string out = "symbol\tdate\tspot\tiv_fair_21d\tiv_fair_63d\trv_fwd_21d\tbar_index\n";
  struct Row {
    const char *sym;
    double iv21;
    double iv63;
  };
  for (const Row rr : {Row{"EEE", 0.30, 0.25}, Row{"FFF", 0.20, 0.22}}) {
    double s = 100.0;
    for (int i = 0; i < n; ++i) {
      s *= std::exp(0.005 * static_cast<double>(i % 3 - 1));
      char buf[256];
      std::snprintf(buf, sizeof buf, "%s\t2026-%02d-%02d\t%.10f\t%.4f\t%.4f\t0.25\t%d\n", rr.sym,
                    1 + i / 28, 1 + i % 28, s, rr.iv21, rr.iv63, i);
      out += buf;
    }
  }
  return out;
}

const char *const kEarnCal =
    "ticker\tearn_date\tsession_hint\tannounce_ts_et\tsource\tfetched_utc\n"
    "EEE\t2026-01-11\tbmo\t2026-01-11T07:00:00-05:00\tsec_edgar_8k_item202\tx\n"
    "FFF\t2026-01-11\tamc\t2026-01-11T16:30:00-05:00\tsec_edgar_8k_item202\tx\n";

} // namespace

TEST(AlphaEarnings, TsvParserSortsPerSymbolAndFailsClosed) {
  // Out-of-order dates arrive sorted with their amc flags still glued on.
  auto cal = earnings_from_tsv("ticker\tearn_date\tsession_hint\n"
                               "AAA\t2026-04-20\tamc\n"
                               "AAA\t2026-01-15\tbmo\n"
                               "AAA\t2026-07-21\tintraday\n");
  ASSERT_TRUE(cal) << cal.error().to_string();
  const EarningsEvents *ev = cal->find("AAA");
  ASSERT_NE(ev, nullptr);
  ASSERT_EQ(ev->date.size(), 3U);
  EXPECT_EQ(ev->date[0], "2026-01-15");
  EXPECT_EQ(ev->date[1], "2026-04-20");
  EXPECT_EQ(ev->amc[0], 0U);
  EXPECT_EQ(ev->amc[1], 1U);
  EXPECT_EQ(ev->amc[2], 0U); // intraday buckets with bmo
  EXPECT_EQ(cal->n_events, 3U);
  EXPECT_EQ(cal->find("ZZZ"), nullptr);

  // A hint outside {bmo, amc, intraday} shifts the event window a full day if
  // guessed, so it is refused, as is an unparseable date.
  EXPECT_FALSE(earnings_from_tsv("ticker\tearn_date\tsession_hint\nAAA\t2026-01-15\tdunno\n"));
  EXPECT_FALSE(earnings_from_tsv("ticker\tearn_date\tsession_hint\nAAA\t01/15/2026\tbmo\n"));
  EXPECT_FALSE(earnings_from_tsv("symbol\tdate\thint\nAAA\t2026-01-15\tbmo\n"));
}

TEST(AlphaEarnings, DaysToEarnRespectsTheCloseBoundary) {
  const PanelFrame f = load(make_two_symbol_panel(30));
  auto series = group_by_symbol(f);
  ASSERT_TRUE(series);
  // AAA prints AFTER the close of 01-05; BBB prints BEFORE its open.
  auto cal = earnings_from_tsv("ticker\tearn_date\tsession_hint\n"
                               "AAA\t2026-01-05\tamc\n"
                               "BBB\t2026-01-05\tbmo\n");
  ASSERT_TRUE(cal);

  const FeatureRegistry reg = builtin_features();
  const auto sel = reg.select(std::vector<std::string>{"f28_days_to_earn"});
  ASSERT_TRUE(sel);
  auto out = evaluate(f, *sel, nullptr, &*cal);
  ASSERT_TRUE(out) << out.error().to_string();
  const std::vector<double> &d = out->values.at("f28_days_to_earn");

  // AAA rows are frame rows 0..29; row 4 is 2026-01-05.
  EXPECT_NEAR(d[3], 1.0, 1e-12);
  EXPECT_NEAR(d[4], 0.0, 1e-12); // amc today: still ahead at the close
  EXPECT_TRUE(std::isnan(d[5])); // calendar exhausted, not "no print coming"
  // BBB rows are frame rows 30..59; its bmo print on 01-05 already hit by the
  // close of 01-05.
  EXPECT_NEAR(d[30 + 3], 1.0, 1e-12);
  EXPECT_TRUE(std::isnan(d[30 + 4]));
}

TEST(AlphaEarnings, ForwardCountAnchorsBmoIntoTheDateAndAmcPastIt) {
  const PanelFrame f = load(make_earner_panel(70));
  auto cal = earnings_from_tsv(kEarnCal);
  ASSERT_TRUE(cal);
  const FeatureRegistry reg = builtin_features();
  const auto sel = reg.select(std::vector<std::string>{"f29_earn_n_21d"});
  ASSERT_TRUE(sel);
  auto out = evaluate(f, *sel, nullptr, &*cal);
  ASSERT_TRUE(out) << out.error().to_string();
  const std::vector<double> &n = out->values.at("f29_earn_n_21d");

  // The print is dated row 10's session (2026-01-11). bmo lands in the return
  // INTO row 10: inside (i, i+21] for i in [0, 9]. amc lands in the return
  // into row 11: inside for i in [0, 10]. Row 10 is where they differ.
  EXPECT_NEAR(n[0], 1.0, 1e-12);        // EEE, bmo
  EXPECT_NEAR(n[9], 1.0, 1e-12);
  EXPECT_NEAR(n[10], 0.0, 1e-12);       // bmo already realized by row 10's close
  EXPECT_NEAR(n[70 + 10], 1.0, 1e-12);  // FFF, amc: still inside the window
  EXPECT_NEAR(n[70 + 11], 0.0, 1e-12);
  // The forward window must exist: the last 21 rows decline.
  EXPECT_TRUE(std::isnan(n[49]));
  EXPECT_FALSE(std::isnan(n[48]));
}

TEST(AlphaEarnings, SigmaEMatchesTheTwoTenorDecompositionAndDeclinesNegative) {
  const PanelFrame f = load(make_earner_panel(70));
  auto cal = earnings_from_tsv(kEarnCal);
  ASSERT_TRUE(cal);
  const FeatureRegistry reg = builtin_features();
  const auto sel = reg.select(std::vector<std::string>{"f30_earn_sigma_e"});
  ASSERT_TRUE(sel);
  auto out = evaluate(f, *sel, nullptr, &*cal);
  ASSERT_TRUE(out) << out.error().to_string();
  const std::vector<double> &se = out->values.at("f30_earn_sigma_e");

  // EEE at i in [0, 6]: n1 = n2 = 1 (the one print sits in both windows), so
  //   sigma_E^2 = T1 (0.30^2 - 0.25^2) / (1 - 1/3).
  const double expect =
      std::sqrt((21.0 / 252.0) * (0.30 * 0.30 - 0.25 * 0.25) / (1.0 - 21.0 / 63.0));
  for (const std::size_t i : {0U, 6U}) {
    EXPECT_NEAR(se[i], expect, 1e-12) << i;
  }
  EXPECT_TRUE(std::isnan(se[7]));       // i + 63 runs off the series
  EXPECT_TRUE(std::isnan(se[70 + 0]));  // FFF: upward structure, sigma_E^2 < 0
}

TEST(AlphaEarnings, MoveRichnessIsSigmaEOverTheNamesOwnDeliveredHistory) {
  const PanelFrame f = load(make_earner_panel(70));
  // EEE's fixture returns are 0.005*(i%3 - 1), so the anchored moves at rows
  // 2 and 4 are +0.005 and exactly 0 -- a zero move is still a delivered
  // print, it counts in the RMS.
  auto cal = earnings_from_tsv("ticker\tearn_date\tsession_hint\n"
                               "EEE\t2026-01-03\tbmo\n"
                               "EEE\t2026-01-05\tbmo\n"
                               "EEE\t2026-01-16\tbmo\n");
  ASSERT_TRUE(cal);
  const FeatureRegistry reg = builtin_features();
  const auto sel = reg.select(std::vector<std::string>{"f31_earn_move_rich"});
  ASSERT_TRUE(sel);
  auto out = evaluate(f, *sel, nullptr, &*cal);
  ASSERT_TRUE(out) << out.error().to_string();
  const std::vector<double> &rich = out->values.at("f31_earn_move_rich");

  // i = 5: upcoming anchor is row 15 (inside both windows, n1 = n2 = 1), and
  // the history is the two past anchors.
  const double se =
      std::sqrt((21.0 / 252.0) * (0.30 * 0.30 - 0.25 * 0.25) / (1.0 - 21.0 / 63.0));
  const double hist = std::sqrt((0.005 * 0.005 + 0.0) / 2.0);
  // 1e-9, not 1e-12: the fixture's spots round-trip through %.10f TSV text,
  // so the anchored move is 0.005 only to the panel's own precision.
  EXPECT_NEAR(rich[5], std::log(se / hist), 1e-9);
  // i = 3: sigma_E is computable (anchors 4 and 15 are both ahead) but only
  // ONE print is history -- an anecdote, not a yardstick. Declined.
  EXPECT_TRUE(std::isnan(rich[3]));
  // FFF has no calendar rows at all.
  EXPECT_TRUE(std::isnan(rich[70 + 5]));
}

TEST(AlphaEarnings, WithoutACalendarTheFamilyDeclinesRatherThanGuessing) {
  const PanelFrame f = load(make_earner_panel(70));
  const FeatureRegistry reg = builtin_features();
  const auto sel = reg.select(std::vector<std::string>{"f28*", "f29*", "f30*", "f31*"});
  ASSERT_TRUE(sel);
  auto out = evaluate(f, *sel);
  ASSERT_TRUE(out) << out.error().to_string();
  for (const auto &[name, col] : out->values) {
    for (const double v : col) {
      ASSERT_TRUE(std::isnan(v)) << name;
    }
  }
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

TEST(AlphaStrategy, AVetoedNameLeavesBothBooksNotJustTheSelection) {
  const PanelFrame f = load(kBook);
  auto dates = group_by_date(f);
  ASSERT_TRUE(dates);
  auto pnl = dh_straddle_pnl_vol_points(f);
  ASSERT_TRUE(pnl);
  StrategyConfig cfg;
  cfg.max_names = 2;
  cfg.horizon_sessions = 1;
  // Veto AAA (the best name) on every date. NaN means "no information" and
  // must NOT veto -- an unknown earnings date is not an imminent one.
  std::vector<double> veto(f.rows(), 0.0);
  auto series = group_by_symbol(f);
  ASSERT_TRUE(series);
  for (const SymbolSeries &s : *series) {
    for (std::size_t i = 0; i < s.size(); ++i) {
      if (s.symbol == "AAA") {
        veto[s.row[i]] = 1.0;
      } else if (s.symbol == "DDD") {
        veto[s.row[i]] = std::nan("");
      }
    }
  }
  auto card = run(f, *dates, *pnl, *pnl, cfg, veto);
  ASSERT_TRUE(card) << card.error().to_string();
  // Admission drops to {BBB +8, CCC +2, DDD 0}: the oracle's top-2 is
  // {BBB, CCC}, mean 5; the floor is the same three names, mean 10/3. The
  // floor moving too is the point: a veto changes the UNIVERSE, and a floor
  // computed on a different set would not be a paired comparison.
  EXPECT_NEAR(card->per_date[0].selected_gross, 5.0, 1e-12);
  EXPECT_NEAR(card->per_date[0].floor_gross, 10.0 / 3.0, 1e-12);
  EXPECT_EQ(card->per_date[0].n_admitted, 3U);

  // A veto column of the wrong length is a programming error, refused.
  const std::vector<double> short_veto(2, 0.0);
  EXPECT_FALSE(run(f, *dates, *pnl, *pnl, cfg, short_veto));
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

// The THIRD axis. `rv` removes the implied leg but keeps the VOL LEVEL, and
// volatility is persistent, so a top-N book sorted on any trailing-variance
// feature beats an equal-weight floor on `rv` almost by construction. Measured
// on the 616-name panel, f22_semivar_dn_21d scored +32.998 vol points of
// forward-RV excess with 100% of phases positive while LOSING 1.974 on the
// money axis with 0% positive. `volchg` subtracts the trailing leg so what is
// left is the CHANGE.
TEST(AlphaStrategy, TheVolChangeAxisCarriesNeitherTheImpliedLegNorTheVolLevel) {
  // rv_fwd 0.30 against a trailing 21d vol of exactly 0.25: f2 = ln(0.25^2).
  const double f2 = std::log(0.25 * 0.25);
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "symbol\tdate\tspot\tiv_fair_21d\tiv_fair_63d\trv_fwd_21d\tf2_log_rv21\n"
                "AAA\t2026-01-05\t100\t0.20\t0.22\t0.30\t%.17g\n",
                f2);
  const PanelFrame f = load(buf);
  auto dh = dh_straddle_pnl_vol_points(f);
  auto rv = forward_rv_vol_points(f);
  auto vc = vol_change_vol_points(f);
  ASSERT_TRUE(dh);
  ASSERT_TRUE(rv);
  ASSERT_TRUE(vc) << vc.error().to_string();
  EXPECT_NEAR((*dh)[0], 10.0, 1e-9); // 100 * (0.30 - 0.20), carries the mark
  EXPECT_NEAR((*rv)[0], 30.0, 1e-9); // 100 * 0.30,          carries the level
  EXPECT_NEAR((*vc)[0], 5.0, 1e-9);  // 100 * (0.30 - 0.25), carries neither
  // The trailing leg it removes is exp(f2/2), read from the panel's own column
  // rather than recomputed -- so the axis is the emitter's trailing vol, not a
  // second opinion about it.
  EXPECT_NEAR((*rv)[0] - (*vc)[0], 100.0 * std::exp(0.5 * f2), 1e-9);
}

// A panel with no f2 column cannot form this axis, and must say so rather than
// emit an all-NaN column that reads as "no signal".
TEST(AlphaStrategy, TheVolChangeAxisRefusesAPanelWithoutTheTrailingColumn) {
  const PanelFrame f = load(kBook);
  ASSERT_FALSE(f.schema().has("f2_log_rv21"));
  const auto vc = vol_change_vol_points(f);
  EXPECT_FALSE(vc.has_value());
}

TEST(AlphaStrategy, TheBackMonthAxisComputesItsForwardLegFromSpotUnderTheGate) {
  const PanelFrame f = load(make_earner_panel(70));
  auto pnl = dh63_straddle_pnl_vol_points(f);
  ASSERT_TRUE(pnl) << pnl.error().to_string();

  // The fixture's return into row j is 0.005*(j%3 - 1); each residue class
  // appears 21 times in any 63-step window, so the forward leg is the same
  // for every valid entry row.
  double sum = 0.0;
  for (int j = 1; j <= 63; ++j) {
    const double r = 0.005 * static_cast<double>(j % 3 - 1);
    sum += r * r;
  }
  const double rv63 = std::sqrt(sum / 63.0 * 252.0);

  // EEE rows are frame rows 0..69 with iv_fair_63d = 0.25; FFF 70..139 at
  // 0.22. 1e-6, not 1e-12: spot round-trips through %.10f panel text.
  EXPECT_NEAR((*pnl)[0], 100.0 * (rv63 - 0.25), 1e-6);
  EXPECT_NEAR((*pnl)[6], 100.0 * (rv63 - 0.25), 1e-6);
  EXPECT_NEAR((*pnl)[70], 100.0 * (rv63 - 0.22), 1e-6);
  // Row 7's forward window runs off the series: no fiction, a NaN.
  EXPECT_TRUE(std::isnan((*pnl)[7]));
  EXPECT_TRUE(std::isnan((*pnl)[69]));
}

TEST(AlphaStrategy, TheBackMonthAxisRefusesAPanelWithoutTheBackStrip) {
  const PanelFrame f = load("symbol\tdate\tspot\tiv_fair_21d\trv_fwd_21d\n"
                            "AAA\t2026-01-05\t100\t0.20\t0.25\n");
  EXPECT_FALSE(dh63_straddle_pnl_vol_points(f));
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
