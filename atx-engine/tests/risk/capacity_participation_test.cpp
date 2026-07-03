// capacity_participation_test.cpp — S4-1 [CORRECTNESS, B1]: the capacity
// participation UNIT bug. risk::capacity_curve's impact_cost_bps divided a
// SHARE count by a DOLLAR-ADV (`part = shares/adv = (notional/price)/adv`),
// off by exactly a factor of `price` versus the dimensionally-correct
// `part = notional/adv` (dollars/dollars, unitless -- what the sqrt-impact law
// and the execution simulator's own share-ADV participation both expect). The
// bug UNDERSTATES participation for any price > 1 and so INFLATES capacity.
//
// By-construction fixture: a single-instrument panel whose newest close is an
// exact `price`, whose per-step return alternates +hi/-hi over the full
// kCapacityVolWindow (=60) so sigma == hi exactly (population std of a
// symmetric two-value series) and mean return == 0 exactly (so gross_edge_bps
// == 0 and net_edge_bps == -cost_bps(aum), isolating the cost term), and whose
// PER-ROW volume is set so close(r)*volume(r) == a fixed dollar_adv EXACTLY at
// every row (so dollar-ADV is exact regardless of the alternating price path).
// The test sim uses Y=1, delta=1 so cost_bps(aum) = 1e4 * |w| * sigma * part
// -- LINEAR in participation, isolating `part` as the only free unknown.
//
//   aum = $1,000,000 (target_aum), |w| = 1.0, price = $100, dollar-ADV = $1e6.
//   Correct participation: part = notional/adv = (aum*|w|)/adv = 1e6/1e6 = 1.0.
//   Buggy participation:   part = shares/adv = (aum*|w|/price)/adv
//                                = (1e6/100)/1e6 = 1e4/1e6 = 0.01
//                          (off by exactly 1/price = 1/100 -- NOT the "1e-4"
//                          arithmetic the sprint-4 planning doc's worked
//                          example states, which is a slip in that doc's own
//                          arithmetic; the FORMULA it documents -- "off by
//                          exactly 1/price" -- is what this fixture proves).
//   sigma = hi = 0.01 (chosen). cost_bps = 1e4 * 1.0 * 0.01 * part.
//     buggy: cost_bps = 1e4*0.01*0.01   =   1.0 bps -> net_edge_bps = -1.0
//     fixed: cost_bps = 1e4*0.01*1.0    = 100.0 bps -> net_edge_bps = -100.0
//
// A second instrument at price=$10 (same aum, |w|, dollar-ADV, sigma) proves
// PRICE INVARIANCE after the fix: buggy part_B = (1e6/10)/1e6 = 0.1 (a
// DIFFERENT value from part_A=0.01 for an identical-notional book -- the bug
// scales with price); fixed part_B = 1e6/1e6 = 1.0 (IDENTICAL to part_A).

#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/panel_types.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/risk/capacity.hpp"

namespace atxtest_capacity_participation_test {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::domain::Symbol;
using atx::engine::InstrumentId;
using atx::engine::kPanelFieldCount;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::risk::capacity_curve;
using atx::engine::risk::CapacityPoint;

// One more row than kCapacityVolWindow(=60) so the vol/edge windows both see a
// FULL 60-return sample (usable_return_window clamps to rows-1 = 60).
constexpr usize kRows = 61U;

// Single-instrument fixture: row 0 close == `price` exactly; per-step return
// alternates +hi/-hi (ret(0)=+hi, ret(1)=-hi, ...) over all 60 usable returns
// (30 of each -> mean 0, population std == hi exactly); per-row volume set so
// close(r)*volume(r) == dollar_adv EXACTLY at every row (dollar-ADV is exact
// over any window regardless of the alternating close path).
class SingleNamePanel {
public:
  SingleNamePanel(f64 price, f64 dollar_adv, f64 hi)
      : cap_{pow2_ceil(kRows)}, mask_words_{1U} {
    universe_.push_back(Symbol{1U});
    fields_.assign(kPanelFieldCount * cap_ * 1U, std::numeric_limits<f64>::quiet_NaN());
    mask_.assign(cap_ * mask_words_, 0ULL);

    std::vector<f64> close(kRows, 0.0);
    close[0] = price;
    for (usize r = 1; r < kRows; ++r) {
      const f64 ratio = (r % 2U == 1U) ? (1.0 + hi) : (1.0 - hi); // alternating +hi/-hi
      close[r] = close[r - 1] / ratio;
    }
    for (usize r = 0; r < kRows; ++r) {
      const usize phys = (kRows - 1U) - r; // newest-first r -> physical row
      const f64 c = close[r];
      const f64 v = dollar_adv / c; // close*volume == dollar_adv exactly, every row
      set(PanelField::Open, phys, c);
      set(PanelField::High, phys, c);
      set(PanelField::Low, phys, c);
      set(PanelField::Close, phys, c);
      set(PanelField::Volume, phys, v);
      mask_[phys * mask_words_] |= 1ULL;
    }
  }

  [[nodiscard]] PanelView view() const noexcept {
    return PanelView{fields_.data(), mask_.data(), std::span<const InstrumentId>{universe_},
                     cap_,           kRows - 1U,   kRows,
                     mask_words_};
  }

private:
  static usize pow2_ceil(usize n) noexcept {
    usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }

  void set(PanelField f, usize phys, f64 v) noexcept {
    fields_[static_cast<usize>(f) * cap_ * 1U + phys] = v;
  }

  usize cap_;
  usize mask_words_;
  std::vector<InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<atx::u64> mask_;
};

// Y=1, delta=1 -> cost_bps(aum) = 1e4 * |w| * sigma * part -- LINEAR in `part`,
// isolating participation as the only unknown the test needs to solve for.
[[nodiscard]] ExecutionSimulator linear_impact_sim() {
  ImpactCfg impact{};
  impact.Y = 1.0;
  impact.delta = 1.0;
  return ExecutionSimulator{FillCfg{},       SlippageCfg{}, impact,
                            CommissionCfg{}, LatencyCfg{},  VolumeCapCfg{}};
}

// ===========================================================================
//  capacity_participation_dimension: part == notional/adv == 1.0 on the
//  by-construction fixture (aum=1e6, |w|=1, price=100, dollar-ADV=1e6). The
//  buggy shares/adv formula yields 0.01 (off by 1/price) -> cost_bps 1.0 bps
//  -> net_edge_bps -1.0; the fixed formula yields cost_bps 100.0 bps ->
//  net_edge_bps -100.0. RED asserts -100.0 (fails against the buggy -1.0).
// ===========================================================================
TEST(CapacityParticipation, ParticipationIsNotionalOverDollarAdv) {
  const SingleNamePanel fx{/*price=*/100.0, /*dollar_adv=*/1.0e6, /*hi=*/0.01};
  const std::vector<f64> w{1.0};
  const std::vector<f64> grid{1.0e6};
  const ExecutionSimulator sim = linear_impact_sim();

  const std::vector<CapacityPoint> pts =
      capacity_curve(std::span<const f64>{w}, fx.view(), sim, std::span<const f64>{grid});
  ASSERT_EQ(pts.size(), 1U);
  // gross_edge_bps == 0 exactly (symmetric +hi/-hi returns) -> net == -cost_bps.
  EXPECT_NEAR(pts[0].net_edge_bps, -100.0, 1e-6)
      << "participation must be notional/dollar-ADV (=1.0), not shares/dollar-ADV (=0.01)";
}

// ===========================================================================
//  capacity_curve_price_invariance: two books with IDENTICAL notional
//  participation (same aum, |w|, dollar-ADV) but DIFFERENT prices ($100 vs
//  $10) must produce the SAME √-impact cost after the fix. Pre-fix the buggy
//  shares/adv formula makes cost scale with 1/price (net_edge -1.0 vs -10.0,
//  a 10x gap matching the 10x price ratio); post-fix both are -100.0.
// ===========================================================================
TEST(CapacityParticipation, CostIsPriceInvariantForEqualNotional) {
  const SingleNamePanel fx_a{/*price=*/100.0, /*dollar_adv=*/1.0e6, /*hi=*/0.01};
  const SingleNamePanel fx_b{/*price=*/10.0, /*dollar_adv=*/1.0e6, /*hi=*/0.01};
  const std::vector<f64> w{1.0};
  const std::vector<f64> grid{1.0e6};
  const ExecutionSimulator sim = linear_impact_sim();

  const std::vector<CapacityPoint> pts_a =
      capacity_curve(std::span<const f64>{w}, fx_a.view(), sim, std::span<const f64>{grid});
  const std::vector<CapacityPoint> pts_b =
      capacity_curve(std::span<const f64>{w}, fx_b.view(), sim, std::span<const f64>{grid});
  ASSERT_EQ(pts_a.size(), 1U);
  ASSERT_EQ(pts_b.size(), 1U);
  EXPECT_NEAR(pts_a[0].net_edge_bps, pts_b[0].net_edge_bps, 1e-6)
      << "identical-notional books at different prices must cost the same after S4-1 "
      << "(pre-fix: " << pts_a[0].net_edge_bps << " vs " << pts_b[0].net_edge_bps << ")";
}

} // namespace atxtest_capacity_participation_test
