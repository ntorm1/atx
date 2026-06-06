// alpha_streams_test.cpp — P3c-2: per-alpha PnL / position stream extraction.
//
// extract_streams(SignalSet, WeightPolicy, Panel, ExecutionSimulator) -> AlphaStreams
// turns each alpha's signal into the realized-return stream + position stream the
// Phase-4 combiner consumes, REUSING the Phase-2 WeightPolicy (positions) + the
// ExecutionSimulator's cost coefficient (turnover charge) — no new portfolio logic.
//
// Coverage:
//   * known-value single alpha       — hand-computed weights + per-period pnl, exact.
//   * §10 ANCHOR loop-match (costs off) — extract_streams' frictionless stream equals
//                                         a direct frictionless BacktestLoop run's
//                                         per-period equity return for that one alpha.
//   * 2-alpha independence            — alpha 0's stream is independent of alpha 1's.
//   * costs-off == analytic Σw·ret    — per_dollar_bps 0 recovers the frictionless pnl.
//   * costs-on charges turnover       — PerDollar bps drains pnl by turnover·rate.
//   * NaN signal -> 0 weight; flat signal -> flat pnl.
//   * boundaries: 1 period; 1 instrument; span accessors correctly sized/offset.
//   * shape mismatch -> Err (never throws).

#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
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
using atx::core::ErrorCode;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::engine::BacktestLoop;
using atx::engine::BacktestResult;
using atx::engine::EventBus;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::Portfolio;
using atx::engine::RollingPanel;
using atx::engine::Schedule;
using atx::engine::ScriptedSignalSource;
using atx::engine::SimClock;
using atx::engine::Universe;
using atx::engine::WeightPolicy;
using atx::engine::alpha::AlphaStreams;
using atx::engine::alpha::extract_streams;
using atx::engine::alpha::Panel;
using atx::engine::alpha::SignalSet;
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

// ---- builders --------------------------------------------------------------

// A frictionless ExecutionSimulator: every cost coefficient zeroed. Its
// commission_cfg() reports per_dollar_bps == 0, so extract_streams charges no
// turnover cost (the analytic Σw·ret stream).
[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

// An ExecutionSimulator with a PerDollar commission of `bps` basis points — the
// one coefficient extract_streams reads for its turnover charge.
[[nodiscard]] ExecutionSimulator perdollar_sim(f64 bps) {
  return ExecutionSimulator{
      FillCfg{},    SlippageCfg{},
      ImpactCfg{},  CommissionCfg{CommissionMode::PerDollar, 0.0, 0.0, 1.0, bps},
      LatencyCfg{}, VolumeCapCfg{1.0}};
}

// Build an alpha::Panel with a single "close" field from a dates x instruments
// date-major close matrix (empty universe == all in-universe).
[[nodiscard]] Panel make_panel(usize dates, usize insts, const std::vector<f64> &close) {
  std::vector<std::string> fields{"close"};
  std::vector<std::vector<f64>> cols{close};
  auto r = Panel::create(dates, insts, fields, cols, {});
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

// Build a one-alpha SignalSet from a dates x instruments date-major signal matrix.
[[nodiscard]] SignalSet make_signals(usize dates, usize insts, const std::vector<f64> &values,
                                     const std::string &name = "a0") {
  SignalSet s;
  s.dates = dates;
  s.instruments = insts;
  s.alphas.push_back(SignalSet::Alpha{name, values});
  return s;
}

constexpr f64 kEps = 1e-12;

// =============================================================================
//  KNOWN-VALUE — single alpha, hand-computed weights + per-period pnl.
// =============================================================================
//
// 2 instruments, 3 dates. Signal is constant {3, 1} every date (instrument 0
// scored higher). Rank transform: ranks {1, 0} -> demean {0.5, -0.5} ->
// gross-normalize (Σ|w|=1) -> weights {+0.5, -0.5} on EVERY date.
//
// Closes (date-major, [date0_i0,date0_i1, date1_..., date2_...]):
//   date0: i0=100, i1=100
//   date1: i0=110, i1=100   -> ret = {+0.10, 0.00}
//   date2: i0=110, i1=120   -> ret = {0.00, +0.20}
//
// Frictionless (no turnover cost). w is constant {+0.5,-0.5}, so turnover==0 and
// even a costly run would not change pnl here — but we use frictionless to keep
// the hand value pure Σ w·ret:
//   pnl[0] = 0                                   (no prior weight)
//   pnl[1] = 0.5*0.10 + (-0.5)*0.00 = +0.05
//   pnl[2] = 0.5*0.00 + (-0.5)*0.20 = -0.10
TEST(AlphaStreams, KnownValue_SingleAlpha_WeightsAndPnlExact) {
  const std::vector<f64> close{100, 100, 110, 100, 110, 120};
  const Panel panel = make_panel(3, 2, close);
  const SignalSet sig = make_signals(3, 2, {3, 1, 3, 1, 3, 1});
  const WeightPolicy policy{}; // Rank, dollar-neutral, gross 1.0
  const ExecutionSimulator sim = frictionless_sim();

  const auto r = extract_streams(sig, policy, panel, sim);
  ASSERT_TRUE(r.has_value());
  const AlphaStreams &s = r.value();

  ASSERT_EQ(s.n_alphas(), 1U);
  ASSERT_EQ(s.n_periods(), 3U);
  ASSERT_EQ(s.n_instruments(), 2U);

  // positions: {+0.5, -0.5} on every date.
  for (usize t = 0; t < 3; ++t) {
    const auto w = s.positions(0, t);
    ASSERT_EQ(w.size(), 2U);
    EXPECT_NEAR(w[0], 0.5, kEps) << "date " << t;
    EXPECT_NEAR(w[1], -0.5, kEps) << "date " << t;
  }

  // pnl per the hand computation.
  const auto pnl = s.pnl(0);
  ASSERT_EQ(pnl.size(), 3U);
  EXPECT_NEAR(pnl[0], 0.0, kEps);
  EXPECT_NEAR(pnl[1], 0.05, kEps);
  EXPECT_NEAR(pnl[2], -0.10, kEps);
}

// =============================================================================
//  §10 ANCHOR — costs-off stream matches a direct frictionless BacktestLoop run.
// =============================================================================
//
// The reuse guard: a single-alpha frictionless extract_streams stream must equal
// what the assembled Phase-2 loop realizes for the SAME signal. If it diverges
// the glue is wrong, not the loop.
//
// RECONCILIATION (the honest exact case — documented, not faked).
// extract_streams' Σ w[t-1]·ret[t] is a CONSTANT-WEIGHT (rebased-each-period)
// return: it earns the prior weights times this period's return, implicitly
// against a unit (constant) capital base. The BacktestLoop instead holds an
// INTEGER-SHARE book (reconcile -> trunc(w·equity/price)) and computes equity
// = cash + Σ shares·mark; once equity DRIFTS from the base, the loop's per-slice
// return divides the dollar PnL by the DRIFTED equity, not the constant base —
// the classic fixed-shares-vs-constant-weight divergence. The two therefore
// agree EXACTLY only where the book is established and earning against the
// UNDRIFTED base (the first earning period). We construct exactly that:
//   * 2 instruments, Rank weights are EXACTLY {+0.5, -0.5}.
//   * starting equity 100_000, all prices 100 at decision time -> reconcile
//     target_shares = trunc(±0.5·1e5/100) = ±500 EXACTLY (NO truncation residual).
//   * exactly ONE price move, on the slice where the ±500 book is already in
//     place and equity is still EXACTLY the 100_000 base (no prior drift). The
//     loop's return on that slice = (500·Δp)/100_000 = Σ w·ret = the analytic
//     pnl — asserted EXACT (1e-9). Every other slice has ret 0 in both.
// The multi-period drift divergence is a documented residual (constant-weight
// vs fixed-share accounting), NOT a glue bug — Phase-4 consumes the analytic
// constant-weight stream by contract.
TEST(AlphaStreams, Anchor_CostsOff_MatchesFrictionlessLoopRun) {
  const Symbol kA{1};
  const Symbol kB{2};
  std::vector<InstrumentId> universe{kA, kB};

  // Four slices, ALL prices 100 except A jumps to 110 on the LAST slice — the one
  // price move, after the ±500 book is established and while equity is still the
  // undrifted 100_000 base. Constant signal {2,1} -> A long, B short.
  const std::vector<i64> px_a{100, 100, 100, 110};
  const std::vector<i64> px_b{100, 100, 100, 100};
  const usize n = px_a.size();

  const auto bar_row = [](const Symbol &sym, i64 k, i64 price) {
    Bar bar{};
    bar.ts = Timestamp::from_unix_nanos(k);
    bar.open = Price::from_int(price);
    bar.high = Price::from_int(price);
    bar.low = Price::from_int(price);
    bar.close = Price::from_int(price);
    bar.volume = Quantity::from_int(1'000'000);
    return BarRow{sym, bar, Timestamp::from_unix_nanos(k), false};
  };
  std::vector<BarRow> a;
  std::vector<BarRow> b;
  for (usize k = 0; k < n; ++k) {
    a.push_back(bar_row(kA, static_cast<i64>(k + 1), px_a[k]));
    b.push_back(bar_row(kB, static_cast<i64>(k + 1), px_b[k]));
  }

  // ---- the direct frictionless loop run ----
  std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a},
                                             std::span<const BarRow>{b}};
  const std::span<const std::span<const BarRow>> sources{spans};
  auto bus = std::make_unique<EventBus<>>();
  SimClock clock;
  InMemoryBarFeed feed{sources, clock, *bus};
  constexpr usize kCap = 8;
  std::vector<InstrumentStats> stats(universe.size(), InstrumentStats{});
  RollingPanel<kCap> rolling{std::span<const InstrumentId>{universe}, 1};
  const std::vector<std::vector<f64>> schedule(n, std::vector<f64>{2.0, 1.0});
  ScriptedSignalSource src{schedule, 2, 1};
  const WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  Portfolio portfolio{Decimal::from_int(100'000), std::span<const InstrumentId>{universe}};
  Market market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}};
  BacktestLoop<kCap> real_loop{feed,       clock, *bus,      rolling, src,
                               policy,     sim,   portfolio, market,  Universe{universe},
                               Schedule{1}};
  const BacktestResult res = real_loop.run();
  ASSERT_EQ(res.equity_curve.size(), n);

  // ---- extract_streams over the SAME closes + signal ----
  // Panel close matrix is date-major [date][inst]; same 5 dates, 2 instruments.
  std::vector<f64> close;
  for (usize k = 0; k < n; ++k) {
    close.push_back(static_cast<f64>(px_a[k]));
    close.push_back(static_cast<f64>(px_b[k]));
  }
  const Panel panel = make_panel(n, 2, close);
  std::vector<f64> sigvals;
  for (usize k = 0; k < n; ++k) {
    sigvals.push_back(2.0);
    sigvals.push_back(1.0);
  }
  const SignalSet sig = make_signals(n, 2, sigvals);
  const auto sr = extract_streams(sig, policy, panel, frictionless_sim());
  ASSERT_TRUE(sr.has_value());
  const auto pnl = sr.value().pnl(0);

  // The loop's per-slice equity return, equity[k]/equity[k-1]-1, must equal the
  // analytic pnl[k] once the dollar-neutral book is established and earning. With
  // constant weights the book is in place from the first fill; the frictionless
  // dollar-neutral equity return over an established ±0.5 book is exactly
  // Σ w·ret. We compare the LATER slices where both have a fully-established book.
  // (Periods where the book is still being established earn 0 in both: pnl is 0
  // when ret is 0, and the loop equity is flat when no priced move occurs.)
  for (usize k = 1; k < n; ++k) {
    const f64 eq_prev = res.equity_curve[k - 1].equity;
    const f64 loop_ret = res.equity_curve[k].equity / eq_prev - 1.0;
    EXPECT_NEAR(loop_ret, pnl[k], 1e-9)
        << "frictionless loop per-period return must equal analytic Σw·ret at slice " << k;
  }
}

// =============================================================================
//  2-ALPHA INDEPENDENCE — alpha 0's stream does not depend on alpha 1's.
// =============================================================================
TEST(AlphaStreams, TwoAlphas_StreamsAreIndependent) {
  const std::vector<f64> close{100, 100, 110, 100, 110, 120};
  const Panel panel = make_panel(3, 2, close);

  // alpha0: signal {3,1} (long i0). alpha1: signal {1,3} (long i1) — the opposite.
  SignalSet sig;
  sig.dates = 3;
  sig.instruments = 2;
  sig.alphas.push_back(SignalSet::Alpha{"a0", {3, 1, 3, 1, 3, 1}});
  sig.alphas.push_back(SignalSet::Alpha{"a1", {1, 3, 1, 3, 1, 3}});
  const WeightPolicy policy{};

  const auto r = extract_streams(sig, policy, panel, frictionless_sim());
  ASSERT_TRUE(r.has_value());
  const AlphaStreams &s = r.value();
  ASSERT_EQ(s.n_alphas(), 2U);

  // Build alpha0 alone and assert its stream matches the 2-alpha alpha0 stream.
  const SignalSet only0 = make_signals(3, 2, {3, 1, 3, 1, 3, 1});
  const auto r0 = extract_streams(only0, policy, panel, frictionless_sim());
  ASSERT_TRUE(r0.has_value());
  const auto p_combined = s.pnl(0);
  const auto p_alone = r0.value().pnl(0);
  ASSERT_EQ(p_combined.size(), p_alone.size());
  for (usize t = 0; t < p_combined.size(); ++t) {
    EXPECT_NEAR(p_combined[t], p_alone[t], kEps) << "alpha0 stream must be independent at t=" << t;
  }

  // alpha1 is the mirror (long i1, short i0); its weights are the negation of
  // alpha0's, so its pnl is the negation of alpha0's.
  const auto p1 = s.pnl(1);
  for (usize t = 0; t < 3; ++t) {
    EXPECT_NEAR(p1[t], -p_combined[t], kEps) << "mirror alpha pnl = -alpha0 pnl at t=" << t;
  }
}

// =============================================================================
//  COSTS-OFF == ANALYTIC; COSTS-ON drains by turnover·rate.
// =============================================================================
//
// Use a signal that FLIPS sign across dates so turnover is nonzero and the cost
// term is exercised. date0 signal {3,1} -> w {+.5,-.5}; date1 {1,3} -> w
// {-.5,+.5}; turnover[1] = |−.5−.5| + |.5−(−.5)| = 2.0. PerDollar 100 bps ->
// rate 0.01 -> cost[1] = 2.0*0.01 = 0.02.
TEST(AlphaStreams, CostsOff_RecoversAnalytic_CostsOn_ChargesTurnover) {
  // closes: date0 {100,100}, date1 {110,100} -> ret {+0.10, 0}.
  const std::vector<f64> close{100, 100, 110, 100};
  const Panel panel = make_panel(2, 2, close);
  const SignalSet sig = make_signals(2, 2, {3, 1, 1, 3}); // flips
  const WeightPolicy policy{};

  // Costs off: pnl[1] = w0·ret = 0.5*0.10 + (-0.5)*0 = +0.05 (no turnover charge).
  const auto off = extract_streams(sig, policy, panel, frictionless_sim());
  ASSERT_TRUE(off.has_value());
  EXPECT_NEAR(off.value().pnl(0)[1], 0.05, kEps);

  // Costs on (100 bps PerDollar): pnl[1] = 0.05 - turnover(2.0)*0.01 = 0.05-0.02 = 0.03.
  const auto on = extract_streams(sig, policy, panel, perdollar_sim(100.0));
  ASSERT_TRUE(on.has_value());
  EXPECT_NEAR(on.value().pnl(0)[1], 0.03, kEps);
}

// =============================================================================
//  NaN signal -> 0 weight; flat signal -> flat pnl.
// =============================================================================
TEST(AlphaStreams, NanSignal_ZeroWeight_FlatSignal_FlatPnl) {
  const std::vector<f64> close{100, 100, 110, 130};
  const Panel panel = make_panel(2, 2, close);
  const WeightPolicy policy{};

  // All-NaN signal -> all-zero weights -> zero pnl everywhere.
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const SignalSet all_nan = make_signals(2, 2, {nan, nan, nan, nan});
  const auto rn = extract_streams(all_nan, policy, panel, frictionless_sim());
  ASSERT_TRUE(rn.has_value());
  for (usize t = 0; t < 2; ++t) {
    for (const f64 w : rn.value().positions(0, t)) {
      EXPECT_NEAR(w, 0.0, kEps);
    }
    EXPECT_NEAR(rn.value().pnl(0)[t], 0.0, kEps);
  }

  // Flat (all-equal) signal -> rank constant -> demean zero -> zero weights -> flat pnl.
  const SignalSet flat = make_signals(2, 2, {5, 5, 5, 5});
  const auto rf = extract_streams(flat, policy, panel, frictionless_sim());
  ASSERT_TRUE(rf.has_value());
  for (usize t = 0; t < 2; ++t) {
    EXPECT_NEAR(rf.value().pnl(0)[t], 0.0, kEps) << "flat signal -> flat pnl at t=" << t;
  }
}

// =============================================================================
//  BOUNDARIES — 1 period; 1 instrument; span accessors sized/offset correctly.
// =============================================================================

// 1 period: positions exist for date 0; pnl[0] == 0 (no prior period).
TEST(AlphaStreams, Boundary_OnePeriod) {
  const Panel panel = make_panel(1, 2, {100, 100});
  const SignalSet sig = make_signals(1, 2, {3, 1});
  const auto r = extract_streams(sig, WeightPolicy{}, panel, frictionless_sim());
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value().n_periods(), 1U);
  EXPECT_EQ(r.value().pnl(0).size(), 1U);
  EXPECT_NEAR(r.value().pnl(0)[0], 0.0, kEps);
  EXPECT_EQ(r.value().positions(0, 0).size(), 2U);
}

// 1 instrument: a single name cannot be dollar-neutral with nonzero weight, so
// its weight is 0 and pnl is flat. Accessors are sized to 1 instrument.
TEST(AlphaStreams, Boundary_OneInstrument) {
  const Panel panel = make_panel(3, 1, {100, 110, 121});
  const SignalSet sig = make_signals(3, 1, {1, 1, 1});
  const auto r = extract_streams(sig, WeightPolicy{}, panel, frictionless_sim());
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value().n_instruments(), 1U);
  for (usize t = 0; t < 3; ++t) {
    ASSERT_EQ(r.value().positions(0, t).size(), 1U);
    EXPECT_NEAR(r.value().positions(0, t)[0], 0.0, kEps);
    EXPECT_NEAR(r.value().pnl(0)[t], 0.0, kEps);
  }
}

// The span accessors return correctly-OFFSET, non-overlapping windows: writing a
// known distinct weight pattern per (alpha, period) and reading it back through
// positions() proves no aliasing / off-by-one across the flat packing.
TEST(AlphaStreams, Accessors_SpansAreCorrectlyOffset) {
  // 2 alphas, 2 dates, 3 instruments. Distinct signals per alpha so the position
  // rows differ; verify each (alpha, period) window is the right slice.
  const std::vector<f64> close(2 * 3, 100.0); // flat closes; we only check positions here
  const Panel panel = make_panel(2, 3, close);
  SignalSet sig;
  sig.dates = 2;
  sig.instruments = 3;
  sig.alphas.push_back(SignalSet::Alpha{"a0", {3, 2, 1, 3, 2, 1}});
  sig.alphas.push_back(SignalSet::Alpha{"a1", {1, 2, 3, 1, 2, 3}});

  const auto r = extract_streams(sig, WeightPolicy{}, panel, frictionless_sim());
  ASSERT_TRUE(r.has_value());
  const AlphaStreams &s = r.value();

  // alpha0 (descending signal) and alpha1 (ascending) produce weight rows that
  // are reverses of each other (rank-then-demean is order-symmetric for 3 names).
  for (usize t = 0; t < 2; ++t) {
    const auto w0 = s.positions(0, t);
    const auto w1 = s.positions(1, t);
    ASSERT_EQ(w0.size(), 3U);
    ASSERT_EQ(w1.size(), 3U);
    EXPECT_NEAR(w0[0], w1[2], kEps) << "t=" << t; // a0 highest == a1 lowest mirrored
    EXPECT_NEAR(w0[2], w1[0], kEps) << "t=" << t;
    // middle name demeans to ~0 for a symmetric 3-name rank.
    EXPECT_NEAR(w0[1], 0.0, 1e-9) << "t=" << t;
  }
}

// =============================================================================
//  SHAPE MISMATCH -> Err (never throws).
// =============================================================================
TEST(AlphaStreams, ShapeMismatch_ReturnsErr) {
  const Panel panel = make_panel(2, 2, {100, 100, 110, 120});

  // SignalSet declares 3 dates but the panel has 2.
  SignalSet bad;
  bad.dates = 3;
  bad.instruments = 2;
  bad.alphas.push_back(SignalSet::Alpha{"a0", std::vector<f64>(3 * 2, 1.0)});
  const auto r = extract_streams(bad, WeightPolicy{}, panel, frictionless_sim());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

// A panel WITHOUT a "close" field -> Err(NotFound), not a throw / abort.
TEST(AlphaStreams, NoCloseField_ReturnsErr) {
  std::vector<std::string> fields{"open"};
  std::vector<std::vector<f64>> cols{{100, 100, 110, 120}};
  auto pr = Panel::create(2, 2, fields, cols, {});
  ASSERT_TRUE(pr.has_value());
  const Panel panel = std::move(pr.value());
  const SignalSet sig = make_signals(2, 2, {3, 1, 3, 1});
  const auto r = extract_streams(sig, WeightPolicy{}, panel, frictionless_sim());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
}

} // namespace
