// combine_combined_source_test.cpp — P4-5: CombinedSignalSource (the mega-alpha).
//
// CombinedSignalSource wraps the pool's constituent ISignalSource* + a frozen
// Combination (fit OOS-safely by P4-4) so the mega-alpha runs in the EXISTING
// BacktestLoop with ZERO loop changes — the seam Phase-2 anticipated. It blends
// each constituent's current-date cross-section into ONE signal:
//   linear methods:  out[k] = (Σ_{i∈V} w_i·s_i[k]) / (Σ_{i∈V}|w_i|)   (V = non-NaN)
//   RankAverage:     out[k] = mean_{i∈V} rank_cs(s_i)[k]              (rank space)
// where V is the set of constituents that are non-NaN at instrument k. The
// denominator Σ|w_i| over the SURVIVING set is the GROSS-PRESERVING renorm: it
// keeps the blend scale stable when some constituents are NaN AND is finite for a
// dollar-neutral combo (Σw=0 but Σ|w|>0). All-NaN cell ⇒ NaN.
//
// Coverage (plan §8 P4-5):
//   * two constituents + known weights -> hand-computed linear blend
//   * RankAverage -> mean of cross-sectional ordinal-percentile ranks (hand-computed)
//   * a NaN constituent at a cell is skipped + renormalized (hand-computed)
//   * all-NaN cell -> NaN
//   * max_lookback = max over constituents
//   * boundary: single constituent (= that alpha sign-normalized by w_0/|w_0|)
//   * weights summing to 0 (dollar-neutral): gross renorm -> finite, no div-by-zero
//   * the REAL BacktestLoop integration test: a mega-alpha over a synthetic panel
//     produces deterministic fills, exactly as a single alpha did in Phase 2.

#include <array>
#include <cmath>  // std::isnan
#include <limits> // std::numeric_limits
#include <memory> // std::make_unique
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/combine/combined_source.hpp"
#include "atx/engine/combine/combiner.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/backtest_loop.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/rolling_panel.hpp"
#include "atx/engine/loop/signal_source.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/portfolio/portfolio.hpp"

namespace {

using atx::f64;
using atx::i64;
using atx::usize;
using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::engine::BacktestLoop;
using atx::engine::BacktestResult;
using atx::engine::EventBus;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::ISignalSource;
using atx::engine::Market;
using atx::engine::Portfolio;
using atx::engine::RollingPanel;
using atx::engine::Schedule;
using atx::engine::ScriptedSignalSource;
using atx::engine::SignalView;
using atx::engine::SimClock;
using atx::engine::Universe;
using atx::engine::WeightPolicy;
using atx::engine::combine::Combination;
using atx::engine::combine::CombinedSignalSource;
using atx::engine::combine::CombineMethod;
using atx::engine::data::BarRow;
using atx::engine::data::InMemoryBarFeed;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using Timestamp = atx::core::time::Timestamp;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// A throwaway PanelView the unit-level evaluate() tests can hand in: the blend is
// PURE in the constituents' outputs, and the ScriptedSignalSource constituents
// IGNORE the panel by design (canned replay), so a minimal view exercises the
// blend math without a populated panel. The panel is built over a single dummy
// instrument (a zero-instrument RollingPanel allocates a null buffer and aborts);
// no row is ever appended, so view() reports 0 valid rows — harmless, the
// constituents never read it. (The loop-integration test below drives a REAL
// populated panel through the real loop.)
using EmptyPanel = RollingPanel<2>;

// Build a Combination value with the given weights (fit window is inert for the
// apply path the source exercises — P4-5 only APPLIES the frozen weights).
[[nodiscard]] Combination combo(std::vector<f64> weights) {
  return Combination{std::move(weights), /*fit_begin=*/0U, /*fit_end=*/0U};
}

// One-shot constituent: a ScriptedSignalSource whose single scheduled vector is
// `v` (universe_size == v.size(), max_lookback == lookback). One evaluate() replays
// `v` verbatim (NaNs preserved), which is exactly the current-date cross-section a
// real constituent would emit.
[[nodiscard]] std::unique_ptr<ScriptedSignalSource> scripted(std::vector<f64> v,
                                                             usize lookback = 0U) {
  const usize n = v.size();
  std::vector<std::vector<f64>> schedule{std::move(v)};
  return std::make_unique<ScriptedSignalSource>(schedule, n, lookback);
}

// Evaluate `src` over a fresh empty panel view and copy the borrowed SignalView
// into an owned vector (the borrow is invalidated by the next evaluate()).
[[nodiscard]] std::vector<f64> eval_copy(ISignalSource &src) {
  static const std::vector<InstrumentId> dummy_universe{Symbol{1}};
  EmptyPanel panel{std::span<const InstrumentId>{dummy_universe}, /*max_lookback=*/1};
  auto r = src.evaluate(panel.view());
  EXPECT_TRUE(r.has_value());
  const SignalView sv = r.value();
  return std::vector<f64>{sv.values.begin(), sv.values.end()};
}

// ============================================================================
//  Linear blend, two constituents, known gross-1 weights, no NaN. With
//  Σ|w| = 1 the gross-preserving denominator is 1, so this is the plain
//  weighted blend out[k] = Σ_i w_i s_i[k].
//    w = [0.75, 0.25]; s0 = [4,2,1]; s1 = [1,3,5]
//    out = [0.75*4+0.25*1, 0.75*2+0.25*3, 0.75*1+0.25*5] = [3.25, 2.25, 2.0]
// ============================================================================
TEST(CombinedSignalSource, LinearBlend_KnownWeights_HandComputed) {
  auto s0 = scripted({4.0, 2.0, 1.0});
  auto s1 = scripted({1.0, 3.0, 5.0});
  std::vector<ISignalSource *> sources{s0.get(), s1.get()};
  CombinedSignalSource src{std::move(sources), combo({0.75, 0.25}), CombineMethod::EqualWeight};

  const std::vector<f64> out = eval_copy(src);
  ASSERT_EQ(out.size(), 3U);
  EXPECT_NEAR(out[0], 3.25, 1e-12);
  EXPECT_NEAR(out[1], 2.25, 1e-12);
  EXPECT_NEAR(out[2], 2.0, 1e-12);
}

// ============================================================================
//  RankAverage: combine in RANK space. Each constituent's cross-section is
//  mapped to ordinal-percentile ranks r/(n-1) (ascending value, NaN-last, stable
//  index tie-break — the Phase-3 CsRank convention), then averaged per cell.
//    s0 = [10,30,20] -> ranks [0.0,1.0,0.5]   (10<20<30)
//    s1 = [ 5, 1, 9] -> ranks [0.5,0.0,1.0]   (1<5<9)
//    out = mean = [0.25, 0.5, 0.75]
// ============================================================================
TEST(CombinedSignalSource, RankAverage_MeanOfCrossSectionalRanks) {
  auto s0 = scripted({10.0, 30.0, 20.0});
  auto s1 = scripted({5.0, 1.0, 9.0});
  std::vector<ISignalSource *> sources{s0.get(), s1.get()};
  // Weights are IGNORED by RankAverage (pure rank-space mean); pass uniform.
  CombinedSignalSource src{std::move(sources), combo({0.5, 0.5}), CombineMethod::RankAverage};

  const std::vector<f64> out = eval_copy(src);
  ASSERT_EQ(out.size(), 3U);
  EXPECT_NEAR(out[0], 0.25, 1e-12);
  EXPECT_NEAR(out[1], 0.5, 1e-12);
  EXPECT_NEAR(out[2], 0.75, 1e-12);
}

// ============================================================================
//  RankAverage with a NaN constituent cell: the NaN cell gets NO rank for that
//  constituent (NaN-last == out-of-set), so the per-cell mean is over the valid
//  constituents only.
//    s0 = [10, NaN, 20] -> ranks [0.0, NaN, 1.0]  (2 valid: 10<20 -> 0/1, 1/1)
//    s1 = [ 5,   1,  9] -> ranks [0.5, 0.0, 1.0]
//    out[0] = (0.0+0.5)/2 = 0.25 ; out[1] = 0.0 (only s1) ; out[2] = (1+1)/2 = 1.0
// ============================================================================
TEST(CombinedSignalSource, RankAverage_NaNCell_SkippedPerConstituent) {
  auto s0 = scripted({10.0, kNaN, 20.0});
  auto s1 = scripted({5.0, 1.0, 9.0});
  std::vector<ISignalSource *> sources{s0.get(), s1.get()};
  CombinedSignalSource src{std::move(sources), combo({0.5, 0.5}), CombineMethod::RankAverage};

  const std::vector<f64> out = eval_copy(src);
  ASSERT_EQ(out.size(), 3U);
  EXPECT_NEAR(out[0], 0.25, 1e-12);
  EXPECT_NEAR(out[1], 0.0, 1e-12);
  EXPECT_NEAR(out[2], 1.0, 1e-12);
}

// ============================================================================
//  Linear blend, a NaN constituent at a cell is SKIPPED and the surviving set is
//  re-normalized by its OWN gross. w = [0.75, 0.25].
//    cell 0: both valid -> (0.75*4 + 0.25*1)/(0.75+0.25) = 3.25/1 = 3.25
//    cell 1: s0 NaN -> only s1 valid -> (0.25*8)/0.25 = 8.0 (the other's value)
//    cell 2: s1 NaN -> only s0 valid -> (0.75*6)/0.75 = 6.0
// ============================================================================
TEST(CombinedSignalSource, LinearBlend_NaNConstituent_SkippedAndRenormalized) {
  auto s0 = scripted({4.0, kNaN, 6.0});
  auto s1 = scripted({1.0, 8.0, kNaN});
  std::vector<ISignalSource *> sources{s0.get(), s1.get()};
  CombinedSignalSource src{std::move(sources), combo({0.75, 0.25}), CombineMethod::IcWeighted};

  const std::vector<f64> out = eval_copy(src);
  ASSERT_EQ(out.size(), 3U);
  EXPECT_NEAR(out[0], 3.25, 1e-12);
  EXPECT_NEAR(out[1], 8.0, 1e-12) << "s0 NaN -> renorm by |w1| -> s1's value";
  EXPECT_NEAR(out[2], 6.0, 1e-12) << "s1 NaN -> renorm by |w0| -> s0's value";
}

// ============================================================================
//  All-NaN cell -> NaN (no opinion survives). Linear method, w = [0.5, 0.5].
//    cell 0: both valid -> (0.5*7 + 0.5*3)/(0.5+0.5) = 5/1 = 5.0
//    cell 1: both NaN -> empty surviving set -> NaN
// ============================================================================
TEST(CombinedSignalSource, LinearBlend_AllNaNCell_IsNaN) {
  auto s0 = scripted({7.0, kNaN});
  auto s1 = scripted({3.0, kNaN});
  std::vector<ISignalSource *> sources{s0.get(), s1.get()};
  CombinedSignalSource src{std::move(sources), combo({0.5, 0.5}), CombineMethod::EqualWeight};

  const std::vector<f64> out = eval_copy(src);
  ASSERT_EQ(out.size(), 2U);
  EXPECT_NEAR(out[0], 5.0, 1e-12);
  EXPECT_TRUE(std::isnan(out[1])) << "every constituent NaN at this cell -> NaN";
}

// ============================================================================
//  RankAverage all-NaN cell -> NaN.
//    s0 = [1, NaN]; s1 = [2, NaN] -> cell 1 has no valid constituent -> NaN.
// ============================================================================
TEST(CombinedSignalSource, RankAverage_AllNaNCell_IsNaN) {
  auto s0 = scripted({1.0, kNaN});
  auto s1 = scripted({2.0, kNaN});
  std::vector<ISignalSource *> sources{s0.get(), s1.get()};
  CombinedSignalSource src{std::move(sources), combo({0.5, 0.5}), CombineMethod::RankAverage};

  const std::vector<f64> out = eval_copy(src);
  ASSERT_EQ(out.size(), 2U);
  // singleton valid set -> percentile 0.5 (CsRank convention), averaged over the
  // single valid constituent.
  EXPECT_NEAR(out[0], 0.5, 1e-12);
  EXPECT_TRUE(std::isnan(out[1]));
}

// ============================================================================
//  max_lookback = max over constituents (so the loop sizes its RollingPanel for
//  the deepest constituent). 0 for an empty source.
// ============================================================================
TEST(CombinedSignalSource, MaxLookback_IsMaxOverConstituents) {
  auto s0 = scripted({1.0, 2.0}, /*lookback=*/5U);
  auto s1 = scripted({3.0, 4.0}, /*lookback=*/12U);
  auto s2 = scripted({5.0, 6.0}, /*lookback=*/3U);
  std::vector<ISignalSource *> sources{s0.get(), s1.get(), s2.get()};
  CombinedSignalSource src{std::move(sources), combo({0.4, 0.4, 0.2}), CombineMethod::EqualWeight};
  EXPECT_EQ(src.max_lookback(), 12U);

  CombinedSignalSource empty{{}, combo({}), CombineMethod::EqualWeight};
  EXPECT_EQ(empty.max_lookback(), 0U);
}

// ============================================================================
//  Boundary: a SINGLE constituent. out[k] = (w0*s0[k])/|w0| = sign(w0)*s0[k].
//    A negative weight FLIPS the sign (the gross renorm divides by |w0|, leaving
//    the magnitude unchanged); a positive weight passes the alpha through.
// ============================================================================
TEST(CombinedSignalSource, SingleConstituent_NegativeWeight_SignFlips) {
  auto s0 = scripted({3.0, 5.0, -2.0});
  std::vector<ISignalSource *> sources{s0.get()};
  CombinedSignalSource src{std::move(sources), combo({-2.0}), CombineMethod::EqualWeight};

  const std::vector<f64> out = eval_copy(src);
  ASSERT_EQ(out.size(), 3U);
  EXPECT_NEAR(out[0], -3.0, 1e-12);
  EXPECT_NEAR(out[1], -5.0, 1e-12);
  EXPECT_NEAR(out[2], 2.0, 1e-12);
}

TEST(CombinedSignalSource, SingleConstituent_PositiveWeight_PassesThrough) {
  auto s0 = scripted({3.0, 5.0, -2.0});
  std::vector<ISignalSource *> sources{s0.get()};
  CombinedSignalSource src{std::move(sources), combo({0.4}), CombineMethod::EqualWeight};

  const std::vector<f64> out = eval_copy(src);
  ASSERT_EQ(out.size(), 3U);
  EXPECT_NEAR(out[0], 3.0, 1e-12);
  EXPECT_NEAR(out[1], 5.0, 1e-12);
  EXPECT_NEAR(out[2], -2.0, 1e-12);
}

// ============================================================================
//  Weights summing to 0 (a dollar-neutral combo): Σw = 0 but Σ|w| = 1 > 0, so
//  the gross-preserving renorm divides by the GROSS (not Σw) and stays finite —
//  there is NO div-by-zero. w = [0.5, -0.5]; s0 = [10,4]; s1 = [2,4].
//    out[0] = (0.5*10 - 0.5*2)/1 = 4.0 ; out[1] = (0.5*4 - 0.5*4)/1 = 0.0
// ============================================================================
TEST(CombinedSignalSource, DollarNeutralWeights_GrossRenorm_Finite) {
  auto s0 = scripted({10.0, 4.0});
  auto s1 = scripted({2.0, 4.0});
  std::vector<ISignalSource *> sources{s0.get(), s1.get()};
  CombinedSignalSource src{std::move(sources), combo({0.5, -0.5}), CombineMethod::EqualWeight};

  const std::vector<f64> out = eval_copy(src);
  ASSERT_EQ(out.size(), 2U);
  EXPECT_NEAR(out[0], 4.0, 1e-12);
  EXPECT_NEAR(out[1], 0.0, 1e-12) << "dollar-neutral, gross renorm -> finite";
  EXPECT_FALSE(std::isnan(out[0]));
  EXPECT_FALSE(std::isnan(out[1]));
}

// ============================================================================
//  Zero gross at a cell -> NaN. If the ONLY surviving constituent has weight 0,
//  Σ|w_i| over V is 0, so out[k] = NaN (no well-defined blend).
//    w = [0.0, 0.6]; cell 1: s1 NaN -> only s0 (weight 0) valid -> gross 0 -> NaN.
// ============================================================================
TEST(CombinedSignalSource, ZeroGrossSurvivingSet_IsNaN) {
  auto s0 = scripted({5.0, 9.0});
  auto s1 = scripted({1.0, kNaN});
  std::vector<ISignalSource *> sources{s0.get(), s1.get()};
  CombinedSignalSource src{std::move(sources), combo({0.0, 0.6}), CombineMethod::EqualWeight};

  const std::vector<f64> out = eval_copy(src);
  ASSERT_EQ(out.size(), 2U);
  // cell 0: (0*5 + 0.6*1)/(0 + 0.6) = 0.6/0.6 = 1.0
  EXPECT_NEAR(out[0], 1.0, 1e-12);
  // cell 1: only s0 valid, |w0| == 0 -> zero gross -> NaN.
  EXPECT_TRUE(std::isnan(out[1]));
}

// ----------------------------------------------------------------------------
//  The REAL BacktestLoop integration test (the §8 capstone).
//
//  Mirrors backtest_loop_test.cpp's run_backtest EXACTLY: Phase-1 spine
//  (InMemoryBarFeed -> SimClock -> EventBus) -> RollingPanel -> SIGNAL SOURCE ->
//  WeightPolicy -> ExecutionSimulator -> Portfolio, sampled into a BacktestResult.
//  The ONLY substitution is the signal source: instead of a single
//  ScriptedSignalSource, we feed a CombinedSignalSource over TWO scripted
//  constituents. The loop is UNCHANGED (the seam's whole point). The combined
//  signal is engineered (via the blend) to reproduce the Phase-2 [3,2,1] ramp, so
//  the deterministic fills/positions/equity match the single-alpha Phase-2 run.
// ----------------------------------------------------------------------------

constexpr usize kCap = 8;
constexpr Timestamp ts(i64 ns) noexcept { return Timestamp::from_unix_nanos(ns); }

const Symbol kA{1};
const Symbol kB{2};
const Symbol kC{3};

[[nodiscard]] ExecutionSimulator make_frictionless_sim() {
  return ExecutionSimulator{
      FillCfg{},
      SlippageCfg{SlippageMode::VolumeShare, /*k=*/0.0, /*bps=*/0.0, /*cap_volshare=*/0.0,
                  /*cap_bps=*/0.0},
      ImpactCfg{/*Y=*/0.0, /*delta=*/0.5, /*gamma=*/0.0},
      CommissionCfg{CommissionMode::PerShare, /*per_share=*/0.0, /*min_fee=*/0.0, /*max_pct=*/1.0,
                    /*per_dollar_bps=*/0.0},
      LatencyCfg{/*latency_nanos=*/0},
      VolumeCapCfg{/*volume_limit=*/1.0}};
}

[[nodiscard]] BarRow bar_row(const Symbol &symbol, i64 k, i64 price) {
  Bar bar{};
  bar.ts = ts(k);
  bar.open = Price::from_int(price);
  bar.high = Price::from_int(price);
  bar.low = Price::from_int(price);
  bar.close = Price::from_int(price);
  bar.volume = Quantity::from_int(1'000'000);
  return BarRow{symbol, bar, ts(k), /*delisted_final=*/false};
}

struct Outcome {
  BacktestResult result;
  std::array<i64, 3> qty{};
  f64 equity{0.0};
};

// Drive the FULL real loop with a CombinedSignalSource over two scripted
// constituents. The two constituent schedules + weights are chosen so the blend
// equals the Phase-2 [3,2,1] ramp EVERY rebalance:
//   s0 = [4,2,1], s1 = [1,2,1], w = [1,-1] (gross 2, dollar-neutral).
//   out = (1*s0 - 1*s1)/(|1|+|1|) = ([4-1,2-2,1-1])/2 = [1.5, 0.0, 0.0]  (WRONG)
// — instead use a gross-1 EqualWeight blend that lands on [3,2,1] directly:
//   s0 = [3,2,1], s1 = [3,2,1], w = [0.5,0.5] -> out = [3,2,1] every cell.
// The mega-alpha thus produces the IDENTICAL signal the Phase-2 single alpha did,
// so the loop's deterministic fills/positions/equity match Phase-2 exactly.
[[nodiscard]] Outcome run_combined_backtest(int n_bars, usize every) {
  std::vector<InstrumentId> universe{kA, kB, kC};

  std::vector<BarRow> a;
  std::vector<BarRow> b;
  std::vector<BarRow> c;
  for (int k = 1; k <= n_bars; ++k) {
    a.push_back(bar_row(kA, k, 100));
    b.push_back(bar_row(kB, k, 50));
    c.push_back(bar_row(kC, k, 200));
  }
  std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a}, std::span<const BarRow>{b},
                                             std::span<const BarRow>{c}};
  const std::span<const std::span<const BarRow>> srcs{spans};

  auto bus = std::make_unique<EventBus<>>();
  SimClock clock;
  InMemoryBarFeed feed{srcs, clock, *bus};

  RollingPanel<kCap> panel{std::span<const InstrumentId>{universe}, /*max_lookback=*/1};

  // Two scripted constituents, one [3,2,1] vector per rebalance fire. The schedule
  // length must cover every fire (n_bars rebalances at every=1).
  const std::vector<std::vector<f64>> sched(static_cast<usize>(n_bars),
                                            std::vector<f64>{3.0, 2.0, 1.0});
  ScriptedSignalSource c0{sched, /*universe_size=*/3, /*max_lookback=*/1};
  ScriptedSignalSource c1{sched, /*universe_size=*/3, /*max_lookback=*/1};
  std::vector<ISignalSource *> constituents{&c0, &c1};
  CombinedSignalSource mega{std::move(constituents), combo({0.5, 0.5}), CombineMethod::EqualWeight};

  const WeightPolicy policy{};
  ExecutionSimulator sim = make_frictionless_sim();
  Portfolio portfolio{Decimal::from_int(100'000), std::span<const InstrumentId>{universe}};
  Market market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{}};
  const Schedule schedule{every};

  BacktestLoop<kCap> loop{
      feed, clock, *bus, panel, mega, policy, sim, portfolio, market, Universe{universe}, schedule};

  Outcome out;
  out.result = loop.run();
  out.qty = {portfolio.holding(kA).qty, portfolio.holding(kB).qty, portfolio.holding(kC).qty};
  out.equity = portfolio.equity();
  return out;
}

// ============================================================================
//  The mega-alpha runs in the REAL loop and reproduces the Phase-2 single-alpha
//  end-to-end result EXACTLY: signal [3,2,1] -> rank -> dollar-neutral gross-1
//  weights A=+0.5, B=0, C=-0.5 -> targets A=+500, C=-250 (B flat), frictionless
//  -> equity stays 100,000. Decided at slice 0, filled at slice 1 (no look-ahead).
// ============================================================================
TEST(CombinedSignalSource, RunsInRealBacktestLoop_MatchesPhase2SingleAlpha) {
  const Outcome o = run_combined_backtest(/*n_bars=*/10, /*every=*/1);

  EXPECT_EQ(o.qty[0], 500) << "A target = trunc(0.5*100000/100)";
  EXPECT_EQ(o.qty[1], 0) << "B has zero weight";
  EXPECT_EQ(o.qty[2], -250) << "C target = trunc(-0.5*100000/200)";

  ASSERT_EQ(o.result.fills.size(), 2U) << "exactly the two opening fills (A, C)";
  for (const auto &f : o.result.fills) {
    EXPECT_EQ(f.t.unix_nanos(), 2) << "decided at slice 0 (t=1), filled at slice 1 (t=2)";
  }
  EXPECT_NEAR(o.equity, 100'000.0, 1e-6) << "frictionless dollar-neutral -> equity unchanged";
  EXPECT_EQ(o.result.slices, 10U);
  EXPECT_EQ(o.result.rebalances, 10U);
}

// ============================================================================
//  Determinism: a repeat run over the SAME feed + SAME Combination produces
//  byte-identical fills/positions/equity (no RNG, fixed-order reductions).
// ============================================================================
TEST(CombinedSignalSource, RealLoop_RepeatRun_IsByteIdentical) {
  const Outcome a = run_combined_backtest(/*n_bars=*/10, /*every=*/1);
  const Outcome b = run_combined_backtest(/*n_bars=*/10, /*every=*/1);

  EXPECT_EQ(a.qty, b.qty);
  EXPECT_EQ(a.equity, b.equity) << "exact bit equality, not EXPECT_NEAR";
  ASSERT_EQ(a.result.fills.size(), b.result.fills.size());
  for (usize i = 0; i < a.result.fills.size(); ++i) {
    EXPECT_EQ(a.result.fills[i].qty, b.result.fills[i].qty);
    EXPECT_TRUE(a.result.fills[i].price == b.result.fills[i].price) << "exact fill-price equality";
    EXPECT_EQ(a.result.fills[i].t.unix_nanos(), b.result.fills[i].t.unix_nanos());
  }
}

} // namespace
