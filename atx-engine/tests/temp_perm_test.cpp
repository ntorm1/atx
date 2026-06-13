// atx::engine::cost — S6-2 temp/perm zero-leakage proof + split-ratio + throttle.
//
// The C3 HARD EXIT CRITERION: the execution simulator's TEMPORARY impact lives
// only in the fill PRICE and never leaks into the forward MARK. This suite drives
// the EXISTING ExecutionSimulator + Market through cost::simulate_round_trip and
// asserts the exact mark identities the documented sim formula predicts:
//
//   * the mark moves by the PERMANENT component ONLY: (mark_after − mark_before) /
//     mark_before == perm_frac · dir, with perm_frac = 0.5·γ·σ·part;
//   * a later NO-TRADE bar leaves the mark untouched (temp does not leak / grow):
//     mark_next_bar == mark_after_fill;
//   * the TEMPORARY cost is in the PRICE: (fill − mark_before)/mark_before ==
//     temp_frac · dir, temp_frac = Y·σ·partᵟ, and |that| ≫ perm_frac.
//
// The expected values are computed HERE from the sim's documented formula (C6: no
// second cost model in temp_perm.hpp). A full single-bar fill is ASSERTED before
// `part` is computed so part = |order qty| / adv exactly.
//
// Naming: Subject_Condition_ExpectedResult.

#include <gtest/gtest.h>

#include <array>
#include <cmath> // std::pow, std::abs (expected-value math)
#include <span>

#include "atx/core/datetime.hpp"      // Timestamp
#include "atx/core/decimal.hpp"       // Decimal
#include "atx/core/domain/domain.hpp" // Bar, Price, Quantity
#include "atx/core/types.hpp"         // u32, i64, f64

#include "atx/engine/cost/temp_perm.hpp"     // simulate_round_trip, fit_split_ratio, should_trade
#include "atx/engine/exec/execution_sim.hpp" // ExecutionSimulator + cfg structs
#include "atx/engine/exec/payloads.hpp"      // OrderPayload, OrderType
#include "atx/engine/loop/market.hpp"        // Market, InstrumentStats
#include "atx/engine/loop/panel_types.hpp"   // MarketSlice, SliceRow
#include "atx/engine/loop/types.hpp"         // InstrumentId

namespace {

using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::time::Timestamp;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::MarketSlice;
using atx::engine::SliceRow;
using atx::engine::cost::RoundTrip;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::OrderPayload;
using atx::engine::exec::OrderType;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;

// ---- file-local fixture (own minimal versions; the sim test's are file-local) ---

[[nodiscard]] InstrumentId inst(atx::u32 id) noexcept { return InstrumentId{id}; }
[[nodiscard]] Timestamp ts(atx::i64 nanos) noexcept { return Timestamp::from_unix_nanos(nanos); }

[[nodiscard]] Bar bar(atx::i64 close, atx::i64 vol) noexcept {
  return Bar{ts(100),
             Price::from_int(close),
             Price::from_int(close),
             Price::from_int(close),
             Price::from_int(close),
             Quantity::from_int(vol)};
}

[[nodiscard]] SliceRow row(atx::u32 id, atx::i64 close, atx::i64 vol) noexcept {
  return SliceRow{inst(id), bar(close, vol), false};
}

// A market order with the canonical signed-qty direction (+buy / −sell).
[[nodiscard]] OrderPayload market_order(atx::i64 qty, atx::i64 queued_nanos) noexcept {
  return OrderPayload{inst(10), qty, OrderType::Market, Decimal{}, ts(queued_nanos)};
}

// Single-instrument (id 10) book priced at `close`/`vol` with the given stats.
struct Book {
  std::array<InstrumentId, 1> universe;
  std::array<InstrumentStats, 1> stats;
  Market market;

  Book(atx::i64 close, atx::i64 vol, InstrumentStats s)
      : universe{inst(10)}, stats{s},
        market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}} {
    std::array<SliceRow, 1> rows{row(10, close, vol)};
    market.update_prices(MarketSlice{ts(100), std::span<const SliceRow>{rows}});
  }
};

// Impact-only sim: temporary (Y, δ) folded into the price, permanent (γ) into the
// mark, slippage ZEROED (FixedBps bps=0) and no commission — so the fill price is
// exactly ref·(1+dir·temp) and the mark moves by exactly ref·perm·dir.
[[nodiscard]] ExecutionSimulator impact_only_sim() {
  const SlippageCfg slip{SlippageMode::FixedBps, /*k=*/0.0, /*bps=*/0.0,
                         /*cap_volshare=*/0.025, /*cap_bps=*/0.10};
  const ImpactCfg impact{/*Y=*/1.0, /*delta=*/0.5, /*gamma=*/0.314};
  const CommissionCfg comm{CommissionMode::PerShare, /*per_share=*/0.0, /*min_fee=*/0.0,
                           /*max_pct=*/0.005, /*per_dollar_bps=*/0.0};
  // Ample cap so a single order fills fully in one slice (the cap never binds).
  const VolumeCapCfg cap{/*volume_limit=*/0.5};
  return ExecutionSimulator{FillCfg{}, slip, impact, comm, LatencyCfg{}, cap};
}

// Stats with zero spread (so the slippage spread-floor is also zero) and σ = 0.02.
[[nodiscard]] InstrumentStats stats_sigma2() noexcept {
  return InstrumentStats{/*adv=*/1'000'000.0, /*sigma=*/0.02, /*spread=*/0.0};
}

// =====================================================================
//  C3 — the hard exit criterion: temporary cost does NOT leak into the mark.
// =====================================================================

TEST(TempPerm, TemporaryCost_DoesNotLeakIntoForwardMark) {
  Book b{/*close=*/100, /*vol=*/1'000'000, stats_sigma2()};
  ExecutionSimulator sim = impact_only_sim();

  const RoundTrip rt = atx::engine::cost::simulate_round_trip(
      sim, b.market, market_order(/*qty=*/5'000, /*queued_at=*/1000), ts(2000), ts(3000));

  // part = |filled| / adv, with the full fill guaranteed by the ample cap (the
  // harness ATX_CHECKs fills.size()==1 internally; the identities below only hold
  // if the WHOLE 5000 filled, so the part below is exact).
  const atx::f64 part = 5'000.0 / 1'000'000.0;
  const atx::f64 perm_frac = 0.5 * 0.314 * 0.02 * part;          // 0.5·γ·σ·part
  const atx::f64 temp_frac = 1.0 * 0.02 * std::pow(part, 0.5);   // Y·σ·partᵟ

  // The mark moved by the PERMANENT component ONLY (buy: dir = +1, mark moves UP).
  EXPECT_NEAR((rt.mark_after_fill - rt.mark_before) / rt.mark_before, perm_frac, 1e-9);
  // A later no-trade bar leaves the mark untouched: temp did not leak / grow.
  EXPECT_NEAR(rt.mark_next_bar, rt.mark_after_fill, 1e-12);
  // The TEMPORARY cost is in the PRICE (slippage zeroed -> exactly temp·dir).
  EXPECT_NEAR((rt.fill_price - rt.mark_before) / rt.mark_before, temp_frac, 1e-9);
  // ...and it DOMINATES the permanent mark move.
  EXPECT_GT(std::abs(rt.fill_price - rt.mark_before) / rt.mark_before, perm_frac);
}

// The SELL-side mirror: dir = −1, so the mark moves DOWN by perm_frac and the fill
// price is below the mark by temp_frac. Proves the SIGN of the split is correct.
TEST(TempPerm, TemporaryCost_DoesNotLeakIntoForwardMark_SellSide) {
  Book b{/*close=*/100, /*vol=*/1'000'000, stats_sigma2()};
  ExecutionSimulator sim = impact_only_sim();

  const RoundTrip rt = atx::engine::cost::simulate_round_trip(
      sim, b.market, market_order(/*qty=*/-5'000, /*queued_at=*/1000), ts(2000), ts(3000));

  const atx::f64 part = 5'000.0 / 1'000'000.0; // |qty| / adv
  const atx::f64 perm_frac = 0.5 * 0.314 * 0.02 * part;
  const atx::f64 temp_frac = 1.0 * 0.02 * std::pow(part, 0.5);

  // Mark moves DOWN by perm_frac (dir = −1).
  EXPECT_NEAR((rt.mark_after_fill - rt.mark_before) / rt.mark_before, -perm_frac, 1e-9);
  EXPECT_NEAR(rt.mark_next_bar, rt.mark_after_fill, 1e-12);
  // Fill below the mark by temp_frac (a sell receives LESS).
  EXPECT_NEAR((rt.fill_price - rt.mark_before) / rt.mark_before, -temp_frac, 1e-9);
  EXPECT_GT(std::abs(rt.fill_price - rt.mark_before) / rt.mark_before, perm_frac);
}

// =====================================================================
//  Throttle — decline a trade whose edge does not clear its modeled cost.
// =====================================================================

TEST(TempPerm, Throttle_DeclinesTradeBelowCost) {
  EXPECT_FALSE(atx::engine::cost::should_trade(/*edge=*/0.2, /*cost=*/0.5));
  EXPECT_TRUE(atx::engine::cost::should_trade(/*edge=*/2.0, /*cost=*/0.5));
  // Exact boundary: edge == cost is a STRICT `>` miss -> decline (no free trade at par).
  EXPECT_FALSE(atx::engine::cost::should_trade(/*edge=*/0.5, /*cost=*/0.5));
  // safety inflates the hurdle: an edge that clears 1x cost may not clear 2x.
  EXPECT_FALSE(atx::engine::cost::should_trade(/*edge=*/0.8, /*cost=*/0.5, /*safety=*/2.0));
}

// =====================================================================
//  Split ratio — positive + finite over a few varied round trips.
// =====================================================================

TEST(TempPerm, SplitRatio_PositiveAndStable) {
  std::array<atx::i64, 3> qtys{2'000, 5'000, 8'000};
  std::array<RoundTrip, 3> trips{};
  for (atx::usize i = 0; i < qtys.size(); ++i) {
    Book b{/*close=*/100, /*vol=*/1'000'000, stats_sigma2()};
    ExecutionSimulator sim = impact_only_sim();
    trips[i] = atx::engine::cost::simulate_round_trip(
        sim, b.market, market_order(qtys[i], /*queued_at=*/1000), ts(2000), ts(3000));
  }
  const atx::f64 ratio = atx::engine::cost::fit_split_ratio(std::span<const RoundTrip>{trips});
  EXPECT_GT(ratio, 0.0);
  EXPECT_TRUE(std::isfinite(ratio));
}

} // namespace
