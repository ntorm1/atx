// backtest_integration_test.cpp — Phase-2 exit-criteria evidence (P2-8).
//
// The proof that the assembled loop (Phase-1 spine -> RollingPanel ->
// ScriptedSignalSource -> WeightPolicy -> ExecutionSimulator -> Portfolio) honours
// the three audit invariants end-to-end, plus survivorship. Each is a TEST, not a
// hope:
//
//   * DETERMINISM   — identical feed => byte-identical fills / equity (a wyhash
//                     folded over the ordered (t, fill, position, equity-bits)
//                     stream is equal across two runs); a MUTATION (perturb one
//                     price, add a late bar) flips the hash (not vacuously passing).
//   * COST-HONESTY  — a sized rebalance's fills are provably worse than the
//                     frictionless mark (buy pays MORE, sell receives LESS); more
//                     participation costs MORE (monotone); turning every cost off
//                     recovers the frictionless equity exactly.
//   * NO-LOOK-AHEAD — truncating the feed after date t leaves every fill / equity
//                     at <= t byte-identical (the future is invisible); an order
//                     decided at t never appears as a fill at t.
//   * SURVIVORSHIP  — a delisted symbol trades up to its final bar and is excluded
//                     thereafter; its earlier position is never retroactively
//                     removed and its mark freezes at the delisting close.
//
// The digest folds SEMANTIC fields (exact Decimal mantissas, unix nanos, canonical
// f64 bits) — never raw struct bytes — so it depends only on observable run state,
// never on padding (the discipline established by the Phase-1 replay determinism
// test).

#include <array>
#include <bit>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/hash.hpp"
#include "atx/core/types.hpp"

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
using atx::u64;
using atx::usize;
using atx::core::Decimal;
using atx::core::hash_combine;
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
using atx::engine::data::BarRow;
using atx::engine::data::InMemoryBarFeed;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::FillPayload;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using Bus = EventBus<>;
using Timestamp = atx::core::time::Timestamp;

constexpr usize kCap = 8;
const Symbol kA{1};
const Symbol kB{2};
const Symbol kC{3};

[[nodiscard]] constexpr Timestamp ts(i64 ns) noexcept { return Timestamp::from_unix_nanos(ns); }

// One constant-OHLCV bar for `symbol` at slice `k` (ts == knowledge_ts == k).
[[nodiscard]] BarRow bar_row(const Symbol &symbol, i64 k, i64 price, i64 vol = 1'000'000,
                             bool delisted_final = false) {
  Bar bar{};
  bar.ts = ts(k);
  bar.open = Price::from_int(price);
  bar.high = Price::from_int(price);
  bar.low = Price::from_int(price);
  bar.close = Price::from_int(price);
  bar.volume = Quantity::from_int(vol);
  return BarRow{symbol, bar, ts(k), delisted_final};
}

// The cost stack to run with (one InstrumentStats applied to every instrument).
struct CostConfig {
  FillCfg fill{};
  SlippageCfg slip{};
  ImpactCfg impact{};
  CommissionCfg comm{};
  LatencyCfg latency{};
  VolumeCapCfg cap{/*volume_limit=*/1.0};
  InstrumentStats stats{};
};

// Every coefficient zeroed -> a fill lands exactly at the bar close. Frictionless.
[[nodiscard]] CostConfig frictionless() {
  CostConfig cc;
  cc.slip = SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0};
  cc.impact = ImpactCfg{0.0, 0.5, 0.0};
  cc.comm = CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0};
  cc.stats = InstrumentStats{};
  return cc;
}

// A realistic, fully-engaged cost stack: spread floor + quadratic slippage +
// √-impact (temp into price, perm into mark) + per-share commission.
[[nodiscard]] CostConfig costly() {
  CostConfig cc;
  cc.slip = SlippageCfg{SlippageMode::VolumeShare, /*k=*/0.1, 0.0, /*cap_volshare=*/0.025, 0.10};
  cc.impact = ImpactCfg{/*Y=*/1.0, /*delta=*/0.5, /*gamma=*/0.314};
  cc.comm = CommissionCfg{CommissionMode::PerShare, /*per_share=*/0.005, /*min_fee=*/1.0,
                          /*max_pct=*/0.005, 0.0};
  cc.stats = InstrumentStats{/*adv=*/1.0e6, /*sigma=*/0.02, /*spread=*/0.05};
  return cc;
}

// Costs WITHOUT permanent impact (gamma = 0). Permanent impact shifts the mark
// FAVOURABLY to the just-opened position (you pushed the price your way), which can
// offset — or even exceed — the spread/slippage/commission drain and make an
// "equity strictly lower" assertion ambiguous. Removing perm impact isolates the
// unambiguous, monotone cost drain (worse fill price + commission) for the
// equity-drain / participation-monotonicity tests. Temp impact + slippage +
// commission remain (all pure drains on the fill price / cash).
[[nodiscard]] CostConfig costly_no_perm() {
  CostConfig cc = costly();
  cc.impact = ImpactCfg{/*Y=*/1.0, /*delta=*/0.5, /*gamma=*/0.0};
  return cc;
}

// What one run exposes for assertions.
struct RunOut {
  BacktestResult result;
  std::array<i64, 3> qty{};
  std::array<f64, 3> marks{};
  f64 equity = 0.0;
};

// Drive one backtest over caller-built per-symbol sources. The source vectors are
// taken BY VALUE so they outlive the feed inside this call (the feed indexes into
// their storage every step).
[[nodiscard]] RunOut run(std::vector<BarRow> a, std::vector<BarRow> b, std::vector<BarRow> c,
                         const std::vector<std::vector<f64>> &schedule, usize every,
                         const CostConfig &cc, i64 starting_cash = 100'000) {
  std::vector<InstrumentId> universe{kA, kB, kC};
  std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a}, std::span<const BarRow>{b},
                                             std::span<const BarRow>{c}};
  const std::span<const std::span<const BarRow>> sources{spans};

  auto bus = std::make_unique<Bus>();
  SimClock clock;
  InMemoryBarFeed feed{sources, clock, *bus};

  std::vector<InstrumentStats> stats(universe.size(), cc.stats);
  RollingPanel<kCap> panel{std::span<const InstrumentId>{universe}, /*max_lookback=*/1};
  ScriptedSignalSource src{schedule, /*universe_size=*/3, /*max_lookback=*/1};
  const WeightPolicy policy{};
  ExecutionSimulator sim{cc.fill, cc.slip, cc.impact, cc.comm, cc.latency, cc.cap};
  Portfolio portfolio{Decimal::from_int(starting_cash), std::span<const InstrumentId>{universe}};
  Market market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}};
  const Schedule schedule_cfg{every};

  BacktestLoop<kCap> loop{feed,        clock, *bus,      panel,  src,
                          policy,      sim,   portfolio, market, Universe{universe},
                          schedule_cfg};

  RunOut out;
  out.result = loop.run();
  out.qty = {portfolio.holding(kA).qty, portfolio.holding(kB).qty, portfolio.holding(kC).qty};
  out.marks = {market.mark(kA), market.mark(kB), market.mark(kC)};
  out.equity = portfolio.equity();
  return out;
}

// Build constant-price sources for `n_bars` over the three instruments.
struct Sources {
  std::vector<BarRow> a, b, c;
};
[[nodiscard]] Sources flat_sources(int n_bars, std::array<i64, 3> px = {100, 50, 200}) {
  Sources s;
  for (int k = 1; k <= n_bars; ++k) {
    s.a.push_back(bar_row(kA, k, px[0]));
    s.b.push_back(bar_row(kB, k, px[1]));
    s.c.push_back(bar_row(kC, k, px[2]));
  }
  return s;
}

[[nodiscard]] std::vector<std::vector<f64>> ramp_schedule(int count) {
  return std::vector<std::vector<f64>>(static_cast<usize>(count), std::vector<f64>{3.0, 2.0, 1.0});
}

// ---- determinism digest ----------------------------------------------------
// Fold the ordered run output (fills + equity samples + EOF state) into a wyhash
// seed via SEMANTIC fields only: exact Decimal mantissas, unix nanos, and
// canonical f64 bits (std::bit_cast). Order-sensitive (ordinals folded first).
[[nodiscard]] u64 digest_full(const BacktestResult &r) {
  u64 d = 0;
  u64 ord = 0;
  for (const FillPayload &f : r.fills) {
    d = hash_combine(d, ord, static_cast<u64>(f.id.id), static_cast<u64>(f.qty),
                     static_cast<u64>(f.price.raw()), static_cast<u64>(f.fee.raw()),
                     static_cast<u64>(f.t.unix_nanos()));
    ++ord;
  }
  for (const auto &s : r.equity_curve) {
    d = hash_combine(d, ord, static_cast<u64>(s.t.unix_nanos()), std::bit_cast<u64>(s.equity),
                     std::bit_cast<u64>(s.gross), std::bit_cast<u64>(s.net));
    ++ord;
  }
  return hash_combine(d, static_cast<u64>(r.final_cash.raw()), std::bit_cast<u64>(r.final_equity),
                      std::bit_cast<u64>(r.turnover), static_cast<u64>(r.slices),
                      static_cast<u64>(r.rebalances));
}

// Fold only the PREFIX of the run with sample/fill timestamp <= cutoff (for the
// feed-truncation invariance check).
[[nodiscard]] u64 digest_prefix(const BacktestResult &r, i64 cutoff_ns) {
  u64 d = 0;
  u64 ord = 0;
  for (const FillPayload &f : r.fills) {
    if (f.t.unix_nanos() > cutoff_ns) {
      continue;
    }
    d = hash_combine(d, ord, static_cast<u64>(f.id.id), static_cast<u64>(f.qty),
                     static_cast<u64>(f.price.raw()), static_cast<u64>(f.t.unix_nanos()));
    ++ord;
  }
  for (const auto &s : r.equity_curve) {
    if (s.t.unix_nanos() > cutoff_ns) {
      continue;
    }
    d = hash_combine(d, ord, static_cast<u64>(s.t.unix_nanos()), std::bit_cast<u64>(s.equity));
    ++ord;
  }
  return d;
}

// ============================================================================
//  DETERMINISM
// ============================================================================
TEST(BacktestIntegration, Determinism_IdenticalFeed_IdenticalDigest) {
  const Sources s = flat_sources(10);
  const RunOut r1 = run(s.a, s.b, s.c, ramp_schedule(10), 1, costly());
  const RunOut r2 = run(s.a, s.b, s.c, ramp_schedule(10), 1, costly());

  const u64 d1 = digest_full(r1.result);
  EXPECT_EQ(d1, digest_full(r2.result)) << "identical feed must replay byte-identically";
  EXPECT_NE(d1, 0U) << "fixture must produce fills/samples (else the test is vacuous)";
}

TEST(BacktestIntegration, Determinism_PerturbedPrice_DifferentDigest) {
  const Sources base = flat_sources(10);
  const u64 baseline =
      digest_full(run(base.a, base.b, base.c, ramp_schedule(10), 1, costly()).result);

  // Perturb a single bar's close (A's 5th bar 100 -> 101); everything else equal.
  Sources mut = flat_sources(10);
  mut.a[4] = bar_row(kA, 5, 101);
  const u64 perturbed =
      digest_full(run(mut.a, mut.b, mut.c, ramp_schedule(10), 1, costly()).result);

  EXPECT_NE(baseline, perturbed) << "a changed bar value must change the digest";
}

TEST(BacktestIntegration, Determinism_AddedLateBar_DifferentDigest) {
  const Sources base = flat_sources(10);
  const u64 baseline =
      digest_full(run(base.a, base.b, base.c, ramp_schedule(10), 1, costly()).result);

  Sources mut = flat_sources(10);
  mut.a.push_back(bar_row(kA, 11, 100)); // one extra late bar on A only
  const u64 longer = digest_full(run(mut.a, mut.b, mut.c, ramp_schedule(11), 1, costly()).result);

  EXPECT_NE(baseline, longer) << "an added late bar must change the digest";
}

// ============================================================================
//  COST-HONESTY
// ============================================================================

// Frictionless dollar-neutral book -> equity is EXACTLY preserved (no drag).
TEST(BacktestIntegration, CostHonesty_Frictionless_EquityExactlyPreserved) {
  const Sources s = flat_sources(10);
  const RunOut r = run(s.a, s.b, s.c, ramp_schedule(10), 1, frictionless());
  EXPECT_NEAR(r.result.final_equity, 100'000.0, 1e-6);
}

// With a fully-engaged cost stack the same book pays to trade -> equity DROPS.
TEST(BacktestIntegration, CostHonesty_CostsOn_EquityStrictlyLower) {
  const Sources s = flat_sources(10);
  const RunOut frictionless_run = run(s.a, s.b, s.c, ramp_schedule(10), 1, frictionless());
  const RunOut costly_run = run(s.a, s.b, s.c, ramp_schedule(10), 1, costly_no_perm());

  EXPECT_LT(costly_run.result.final_equity, frictionless_run.result.final_equity)
      << "spread + slippage + temp impact + commission must drain equity";
}

// A buy fill prices ABOVE the mark and a sell fill BELOW it (the cost crosses
// adverse to the trade direction). Prices are constant 100/200, so the mark at
// fill time is the bar close.
TEST(BacktestIntegration, CostHonesty_BuyAboveMark_SellBelowMark) {
  const Sources s = flat_sources(3);
  const RunOut r = run(s.a, s.b, s.c, ramp_schedule(3), 1, costly());

  ASSERT_FALSE(r.result.fills.empty());
  bool saw_buy = false;
  bool saw_sell = false;
  for (const FillPayload &f : r.result.fills) {
    const f64 px = f.price.to_double();
    if (f.id.id == kA.id && f.qty > 0) { // A is the long leg (buy)
      EXPECT_GT(px, 100.0) << "a buy fills ABOVE the mark by the modeled cost";
      saw_buy = true;
    }
    if (f.id.id == kC.id && f.qty < 0) { // C is the short leg (sell)
      EXPECT_LT(px, 200.0) << "a sell fills BELOW the mark by the modeled cost";
      saw_sell = true;
    }
  }
  EXPECT_TRUE(saw_buy);
  EXPECT_TRUE(saw_sell);
}

// More participation costs more: a 2x-gross book trades larger -> higher turnover
// AND a larger equity drain than the 1x book (monotone in size).
TEST(BacktestIntegration, CostHonesty_MoreParticipation_CostsMore) {
  const Sources s = flat_sources(3);

  WeightPolicy lev1; // gross_leverage 1.0 (default)
  WeightPolicy lev2;
  lev2.gross_leverage = 2.0;

  // Two runs differing only in the weight policy's gross leverage. Inline the loop
  // wiring (the shared `run` helper fixes the policy) so the only delta is leverage.
  const auto run_with = [&](const WeightPolicy &policy) {
    std::vector<InstrumentId> universe{kA, kB, kC};
    std::vector<BarRow> a = s.a;
    std::vector<BarRow> b = s.b;
    std::vector<BarRow> c = s.c;
    std::vector<std::span<const BarRow>> spans{
        std::span<const BarRow>{a}, std::span<const BarRow>{b}, std::span<const BarRow>{c}};
    const std::span<const std::span<const BarRow>> sources{spans};
    auto bus = std::make_unique<Bus>();
    SimClock clock;
    InMemoryBarFeed feed{sources, clock, *bus};
    const CostConfig cc = costly_no_perm();
    std::vector<InstrumentStats> stats(universe.size(), cc.stats);
    RollingPanel<kCap> panel{std::span<const InstrumentId>{universe}, 1};
    ScriptedSignalSource src{ramp_schedule(3), 3, 1};
    ExecutionSimulator sim{cc.fill, cc.slip, cc.impact, cc.comm, cc.latency, cc.cap};
    Portfolio pf{Decimal::from_int(100'000), std::span<const InstrumentId>{universe}};
    Market market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}};
    BacktestLoop<kCap> loop{
        feed, clock, *bus, panel, src, policy, sim, pf, market, Universe{universe}, Schedule{1}};
    return loop.run();
  };

  const BacktestResult r1 = run_with(lev1);
  const BacktestResult r2 = run_with(lev2);

  EXPECT_GT(r2.turnover, r1.turnover) << "2x leverage trades a larger notional";
  EXPECT_LT(r2.final_equity, r1.final_equity) << "the larger book pays more total cost";
}

// ============================================================================
//  NO-LOOK-AHEAD
// ============================================================================

// Truncating the feed after date t leaves every fill / equity at <= t identical:
// the future is invisible to past decisions. Run 10 bars vs 5 bars and compare the
// prefix digest at the cutoff.
TEST(BacktestIntegration, NoLookAhead_FeedTruncation_PrefixByteIdentical) {
  const Sources full = flat_sources(10);
  const Sources trunc = flat_sources(5);

  const RunOut full_run = run(full.a, full.b, full.c, ramp_schedule(10), 1, costly());
  const RunOut trunc_run = run(trunc.a, trunc.b, trunc.c, ramp_schedule(5), 1, costly());

  const i64 cutoff = 5; // last sealed bar of the truncated feed (ts == 5)
  EXPECT_EQ(digest_prefix(full_run.result, cutoff), digest_prefix(trunc_run.result, cutoff))
      << "fills/equity at <= t must not depend on bars after t";
}

// An order decided on a slice never fills on that same slice: the earliest fill is
// on slice 1 (ts=2), one slice after the slice-0 (ts=1) decision.
TEST(BacktestIntegration, NoLookAhead_OrderNeverFillsOnDecisionSlice) {
  const Sources s = flat_sources(10);
  const RunOut r = run(s.a, s.b, s.c, ramp_schedule(10), 1, costly());

  ASSERT_FALSE(r.result.fills.empty());
  for (const FillPayload &f : r.result.fills) {
    EXPECT_GT(f.t.unix_nanos(), 1) << "no fill lands on the slice-0 decision time";
  }
}

// ============================================================================
//  SURVIVORSHIP
// ============================================================================

// A symbol that delists at bar 3 (delisted_final, no later bars) trades up to that
// bar; its position is NOT retroactively removed and its mark freezes at the
// delisting close. A,B run the full 6 bars. C is shorted at slice 0 and held.
TEST(BacktestIntegration, Survivorship_DelistedTradesToFinalBar_NotRetroactivelyRemoved) {
  std::vector<InstrumentId> universe{kA, kB, kC};

  std::vector<BarRow> a;
  std::vector<BarRow> b;
  std::vector<BarRow> c;
  for (int k = 1; k <= 6; ++k) {
    a.push_back(bar_row(kA, k, 100));
    b.push_back(bar_row(kB, k, 50));
  }
  // C trades bars 1..3, delisting on bar 3 (last bar carries delisted_final).
  c.push_back(bar_row(kC, 1, 200));
  c.push_back(bar_row(kC, 2, 200));
  c.push_back(bar_row(kC, 3, /*price=*/200, /*vol=*/1'000'000, /*delisted_final=*/true));

  std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a}, std::span<const BarRow>{b},
                                             std::span<const BarRow>{c}};
  const std::span<const std::span<const BarRow>> sources{spans};

  auto bus = std::make_unique<Bus>();
  SimClock clock;
  InMemoryBarFeed feed{sources, clock, *bus};

  const CostConfig cc = frictionless();
  std::vector<InstrumentStats> stats(universe.size(), cc.stats);
  RollingPanel<kCap> panel{std::span<const InstrumentId>{universe}, 1};
  ScriptedSignalSource src{ramp_schedule(6), 3, 1}; // C scored lowest -> short C
  const WeightPolicy policy{};
  ExecutionSimulator sim{cc.fill, cc.slip, cc.impact, cc.comm, cc.latency, cc.cap};
  Portfolio portfolio{Decimal::from_int(100'000), std::span<const InstrumentId>{universe}};
  Market market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}};

  BacktestLoop<kCap> loop{feed,       clock, *bus,      panel,  src,
                          policy,     sim,   portfolio, market, Universe{universe},
                          Schedule{1}};
  const BacktestResult r = loop.run();

  // The full 6-bar merged stream ran (A/B carry it past C's delisting).
  EXPECT_EQ(r.slices, 6U);

  // C was shorted before delisting and the position is STILL on the book at EOF —
  // never retroactively removed.
  EXPECT_LT(portfolio.holding(kC).qty, 0) << "the delisted short persists to EOF";

  // C's mark froze at its delisting close (200); A/B continued at their closes.
  EXPECT_NEAR(market.mark(kC), 200.0, 1e-9) << "a delisted symbol's mark freezes";
  EXPECT_NEAR(market.mark(kA), 100.0, 1e-9);

  // Every C fill happened at or before its delisting bar (ts <= 3) — none after.
  for (const FillPayload &f : r.fills) {
    if (f.id.id == kC.id) {
      EXPECT_LE(f.t.unix_nanos(), 3) << "no fill for a delisted symbol after its final bar";
    }
  }
}

} // namespace
