// risk_capacity_test.cpp — P4-10a: the capacity curve (√-impact edge erosion).
//
// capacity_curve(weights, panel, sim, aum_grid) sweeps AUM through the SAME
// exec::ExecutionSimulator √-impact cost surface the rest of the engine uses (it
// reads sim.impact_cfg(), never a second hardcoded cost number) and reports, per
// AUM, the net frictionless edge AFTER market-impact erosion. RenTech §9.6:
// report capacity, not just return.
//
// Model under test (see capacity.hpp for the full derivation):
//   gross_edge_bps = 1e4 * mean_{r in [0,W_edge)} ( Σ_i w[i] * ret_i(r) )
//   cost_bps(aum)  = 1e4 * Σ_i |w[i]| * (Y * sigma_i * part_i^delta)
//     where ret_i(r)=close(r,i)/close(r+1,i)-1, sigma_i=popstd(ret over W_vol),
//     ADV_i=mean(close*volume over W_adv), shares_i=(aum*|w[i]|)/close(0,i),
//     part_i=shares_i/ADV_i.
//   net_edge_bps(aum) = gross_edge_bps - cost_bps(aum).
//
// Coverage (plan §8 P4-10 capacity contract — the six pins + boundaries):
//   pin 1 MonotoneNonIncreasing : net edge is non-increasing across ascending AUM.
//   pin 2 ZeroCrossing          : a positive-edge book crosses zero on a wide grid.
//   pin 3 SmallAumApproxGross   : at AUM→0 net edge ≈ gross edge (impact→0).
//   pin 4 OneCostSurface        : a larger ImpactCfg.Y -> strictly larger cost.
//   pin 5 Determinism           : identical inputs -> bit-identical CapacityPoint vec.
//   pin 6 GridOrderAndLength    : output length == aum_grid.size(), aum maps 1:1.
//   boundaries EmptyGrid / ZeroWeightContributesNothing / NaNWeightExcluded /
//              ShortPanelDegenerates / HandComputedNetEdge.

#include <cmath>   // std::isnan, std::pow, std::sqrt, std::fabs
#include <cstring> // std::memcmp (bitwise determinism check)
#include <limits>  // std::numeric_limits (quiet NaN sentinel)
#include <span>
#include <vector>

#include <gtest/gtest.h> // gtest macros

#include "atx/core/types.hpp"

#include "atx/engine/exec/execution_sim.hpp" // ExecutionSimulator, ImpactCfg
#include "atx/engine/loop/panel_types.hpp"   // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"         // InstrumentId (Symbol)
#include "atx/engine/risk/capacity.hpp"

namespace {

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

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// ===========================================================================
//  PanelFixture — owns a PanelView's backing storage for one test.
//  (Mirrors risk_exposures_test.cpp: a column-major-per-field ring block exactly
//  as RollingPanel would build it. Input grid row 0 = NEWEST cross-section;
//  cap = next pow2 >= n_rows; head = n_rows-1 so newest-first r maps to physical
//  row n_rows-1-r — a clean, no-wrap layout.)
// ===========================================================================
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close,
               const std::vector<std::vector<f64>> &volume)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2_ceil(n_rows)},
        mask_words_{(n_inst + 63U) / 64U} {
    universe_.reserve(n_inst);
    for (usize i = 0; i < n_inst; ++i) {
      universe_.push_back(Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(kPanelFieldCount * cap_ * n_inst_, kNaN);
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r; // newest-first r -> physical row
      for (usize i = 0; i < n_inst_; ++i) {
        const f64 c = close[r][i];
        const f64 v = volume[r][i];
        set(PanelField::Open, phys, i, c);
        set(PanelField::High, phys, i, c);
        set(PanelField::Low, phys, i, c);
        set(PanelField::Close, phys, i, c);
        set(PanelField::Volume, phys, i, v);
        if (!std::isnan(c)) {
          mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
        }
      }
    }
  }

  [[nodiscard]] PanelView view() const noexcept {
    return PanelView{fields_.data(), mask_.data(), std::span<const InstrumentId>{universe_},
                     cap_,           head_(),      n_rows_,
                     mask_words_};
  }

private:
  [[nodiscard]] usize head_() const noexcept { return (n_rows_ == 0U) ? 0U : n_rows_ - 1U; }

  static usize pow2_ceil(usize n) noexcept {
    usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }

  void set(PanelField f, usize phys, usize inst, f64 v) noexcept {
    const usize block = static_cast<usize>(f) * cap_ * n_inst_;
    fields_[block + phys * n_inst_ + inst] = v;
  }

  usize n_rows_;
  usize n_inst_;
  usize cap_;
  usize mask_words_;
  std::vector<InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<atx::u64> mask_;
};

// A sim with everything default EXCEPT the impact-scale Y (the §0-G coefficient
// the capacity sweep reads). Lets a test prove "one cost surface" by varying Y.
[[nodiscard]] ExecutionSimulator sim_with_Y(f64 y) {
  ImpactCfg impact{};
  impact.Y = y; // delta/gamma stay at their Appendix-A defaults (delta=0.5)
  return ExecutionSimulator{FillCfg{},       SlippageCfg{}, impact,
                            CommissionCfg{}, LatencyCfg{},  VolumeCapCfg{}};
}

// ===========================================================================
//  A "rising" panel: each instrument's close grows by a fixed per-step ratio so
//  every per-step return is a known positive constant -> a positive gross edge
//  for a long book, and a constant return series (sigma small but non-zero only
//  where we perturb). We build many rows so the W_vol window has data. Row 0 is
//  newest; OLDER rows have LOWER prices so close(r)/close(r+1) > 1 (positive ret).
// ===========================================================================
[[nodiscard]] PanelFixture make_rising_panel(usize n_rows, usize n_inst, f64 base, f64 step_ret,
                                             f64 vol) {
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst, 0.0));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, vol));
  for (usize i = 0; i < n_inst; ++i) {
    // close at newest row 0 = base; older rows scaled DOWN by (1+step_ret) each
    // step back, so close(r)/close(r+1) = (1+step_ret) -> ret = step_ret > 0.
    for (usize r = 0; r < n_rows; ++r) {
      close[r][i] = base / std::pow(1.0 + step_ret, static_cast<f64>(r));
    }
  }
  return PanelFixture{n_rows, n_inst, close, volume};
}

// A rising panel with GENUINE return variance (so sigma_i > 0 and the √-impact
// term actually grows with AUM): per-step returns alternate between `hi` and `lo`
// (both positive -> a positive average edge), injecting volatility. Row 0 newest;
// older rows are scaled DOWN by the per-step ratio so close(r)/close(r+1) > 1.
[[nodiscard]] PanelFixture make_volatile_rising_panel(usize n_rows, usize n_inst, f64 base, f64 hi,
                                                      f64 lo, f64 vol) {
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst, 0.0));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, vol));
  for (usize i = 0; i < n_inst; ++i) {
    close[0][i] = base;
    for (usize r = 1; r < n_rows; ++r) {
      const f64 ratio = (r % 2U == 1U) ? (1.0 + hi) : (1.0 + lo); // ret(r-1) alternates
      close[r][i] = close[r - 1][i] / ratio; // older = newer / (1+ret)
    }
  }
  return PanelFixture{n_rows, n_inst, close, volume};
}

// ===========================================================================
//  pin 6 + 1 + 2: a wide ascending grid yields one point per AUM (grid order),
//  monotone non-increasing, and crosses zero for a positive-edge long book.
// ===========================================================================
TEST(RiskCapacity, GridOrderMonotoneAndZeroCrossing) {
  const usize rows = 80U, inst = 2U;
  // A panel with REAL return variance: sigma_i > 0 so the √-impact cost actually
  // grows with AUM (a constant-return panel has sigma==0 -> zero impact, no
  // crossing). hi/lo are both positive -> a positive average gross edge.
  PanelFixture fx =
      make_volatile_rising_panel(rows, inst, /*base=*/100.0, /*hi=*/0.003, /*lo=*/0.0002,
                                 /*vol=*/1.0e3);
  const std::vector<f64> w{0.5, 0.5}; // long book, gross-scaled

  // A wide ascending AUM grid: tiny -> enormous so cost overtakes the edge.
  const std::vector<f64> grid{1.0e3, 1.0e5, 1.0e7, 1.0e9, 1.0e11, 1.0e13, 1.0e15};
  const ExecutionSimulator sim = sim_with_Y(1.0);

  const std::vector<CapacityPoint> pts =
      capacity_curve(std::span<const f64>{w}, fx.view(), sim, std::span<const f64>{grid});

  // pin 6: one point per grid entry, in grid order, aum mapped 1:1.
  ASSERT_EQ(pts.size(), grid.size());
  for (usize k = 0; k < grid.size(); ++k) {
    EXPECT_EQ(pts[k].aum, grid[k]);
  }

  // pin 1: net edge is non-increasing across ascending AUM (cost grows with AUM).
  for (usize k = 0; k + 1U < pts.size(); ++k) {
    EXPECT_LE(pts[k + 1U].net_edge_bps, pts[k].net_edge_bps + 1e-9)
        << "monotonicity violated at k=" << k;
  }

  // pin 2: starts positive (small AUM) and goes negative (large AUM) -> crosses 0.
  EXPECT_GT(pts.front().net_edge_bps, 0.0);
  EXPECT_LT(pts.back().net_edge_bps, 0.0);
}

// ===========================================================================
//  pin 3: at AUM→0 the impact term vanishes, so net edge ≈ gross edge. We pin
//  the gross edge analytically: a long book on a panel whose every per-step
//  return is exactly step_ret has gross_edge_bps = 1e4 * (Σ_i w[i]) * step_ret.
// ===========================================================================
TEST(RiskCapacity, SmallAumApproxGross) {
  const usize rows = 80U, inst = 2U;
  const f64 step_ret = 0.001;
  PanelFixture fx = make_rising_panel(rows, inst, 100.0, step_ret, 1.0e3);
  const std::vector<f64> w{0.5, 0.5};

  const std::vector<f64> grid{0.0, 1.0}; // aum=0 -> zero impact by construction
  const ExecutionSimulator sim = sim_with_Y(1.0);
  const std::vector<CapacityPoint> pts =
      capacity_curve(std::span<const f64>{w}, fx.view(), sim, std::span<const f64>{grid});

  ASSERT_EQ(pts.size(), 2U);
  // gross = 1e4 * (0.5+0.5) * step_ret. Every per-step return is exactly step_ret.
  const f64 expected_gross = 1.0e4 * (0.5 + 0.5) * step_ret;
  EXPECT_NEAR(pts[0].net_edge_bps, expected_gross, 1e-6); // aum=0 -> pure gross
  // a tiny AUM is still essentially gross (impact negligible).
  EXPECT_NEAR(pts[1].net_edge_bps, expected_gross, 1e-3);
}

// ===========================================================================
//  pin 4 (ONE COST SURFACE): a sim with a LARGER ImpactCfg.Y produces strictly
//  larger cost / strictly smaller net edge at the same AUM. Proves the sweep
//  reads sim.impact_cfg().Y, not a hardcoded constant.
// ===========================================================================
TEST(RiskCapacity, OneCostSurfaceReadsSimY) {
  const usize rows = 80U, inst = 2U;
  // A panel with REAL return variance so sigma_i > 0 (impact is non-zero). We
  // alternate the per-step ratio to inject volatility while keeping a positive
  // average edge.
  std::vector<std::vector<f64>> close(rows, std::vector<f64>(inst, 0.0));
  std::vector<std::vector<f64>> volume(rows, std::vector<f64>(inst, 1.0e3));
  for (usize i = 0; i < inst; ++i) {
    f64 px = 100.0;
    for (usize r = 0; r < rows; ++r) {
      close[r][i] = px;                              // row 0 newest
      const f64 ratio = (r % 2U == 0U) ? 1.002 : 1.0004; // older = newer/ratio
      px /= ratio;
    }
  }
  PanelFixture fx{rows, inst, close, volume};
  const std::vector<f64> w{0.5, 0.5};
  const std::vector<f64> grid{1.0e8}; // a single mid AUM where impact matters

  const std::vector<CapacityPoint> small_y =
      capacity_curve(std::span<const f64>{w}, fx.view(), sim_with_Y(0.5),
                     std::span<const f64>{grid});
  const std::vector<CapacityPoint> big_y =
      capacity_curve(std::span<const f64>{w}, fx.view(), sim_with_Y(2.0),
                     std::span<const f64>{grid});

  ASSERT_EQ(small_y.size(), 1U);
  ASSERT_EQ(big_y.size(), 1U);
  // Larger Y -> larger impact cost -> strictly SMALLER net edge at the same AUM.
  EXPECT_LT(big_y[0].net_edge_bps, small_y[0].net_edge_bps);
}

// ===========================================================================
//  pin 5: determinism — identical inputs produce a BIT-IDENTICAL point vector.
// ===========================================================================
TEST(RiskCapacity, DeterministicBitwise) {
  const usize rows = 80U, inst = 3U;
  PanelFixture fx = make_rising_panel(rows, inst, 50.0, 0.0007, 5.0e3);
  const std::vector<f64> w{0.4, -0.3, 0.3}; // a dollar-ish mixed book
  const std::vector<f64> grid{1.0e4, 1.0e6, 1.0e8, 1.0e10};
  const ExecutionSimulator sim = sim_with_Y(1.3);

  const std::vector<CapacityPoint> a =
      capacity_curve(std::span<const f64>{w}, fx.view(), sim, std::span<const f64>{grid});
  const std::vector<CapacityPoint> b =
      capacity_curve(std::span<const f64>{w}, fx.view(), sim, std::span<const f64>{grid});

  ASSERT_EQ(a.size(), b.size());
  // Bitwise identity (not just EXPECT_DOUBLE_EQ) — determinism §3.2.
  ASSERT_EQ(a.size() * sizeof(CapacityPoint), b.size() * sizeof(CapacityPoint));
  EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size() * sizeof(CapacityPoint)), 0);
}

// ===========================================================================
//  boundary: an empty AUM grid -> an empty point vector.
// ===========================================================================
TEST(RiskCapacity, EmptyGridYieldsEmpty) {
  const usize rows = 80U, inst = 2U;
  PanelFixture fx = make_rising_panel(rows, inst, 100.0, 0.001, 1.0e3);
  const std::vector<f64> w{0.5, 0.5};
  const ExecutionSimulator sim = sim_with_Y(1.0);
  const std::vector<CapacityPoint> pts =
      capacity_curve(std::span<const f64>{w}, fx.view(), sim, std::span<const f64>{});
  EXPECT_TRUE(pts.empty());
}

// ===========================================================================
//  boundary: a zero-weight name contributes nothing — adding a third instrument
//  with weight 0 leaves the curve identical to the 2-name book.
// ===========================================================================
TEST(RiskCapacity, ZeroWeightContributesNothing) {
  const usize rows = 80U, inst = 3U;
  PanelFixture fx = make_rising_panel(rows, inst, 100.0, 0.001, 1.0e3);
  const std::vector<f64> w2{0.5, 0.5, 0.0}; // third name carries no weight
  const std::vector<f64> grid{1.0e3, 1.0e8, 1.0e12};
  const ExecutionSimulator sim = sim_with_Y(1.0);

  const std::vector<CapacityPoint> with_zero =
      capacity_curve(std::span<const f64>{w2}, fx.view(), sim, std::span<const f64>{grid});

  // The same book without the dead name (2-instrument panel) must match exactly.
  PanelFixture fx2 = make_rising_panel(rows, /*n_inst=*/2U, 100.0, 0.001, 1.0e3);
  const std::vector<f64> w1{0.5, 0.5};
  const std::vector<CapacityPoint> without =
      capacity_curve(std::span<const f64>{w1}, fx2.view(), sim, std::span<const f64>{grid});

  ASSERT_EQ(with_zero.size(), without.size());
  for (usize k = 0; k < with_zero.size(); ++k) {
    EXPECT_NEAR(with_zero[k].net_edge_bps, without[k].net_edge_bps, 1e-9);
  }
}

// ===========================================================================
//  boundary: a NaN weight is excluded (no NaN leaks into gross OR cost). A book
//  with a NaN third weight must equal the same book with that name at weight 0.
// ===========================================================================
TEST(RiskCapacity, NaNWeightExcludedNoLeak) {
  const usize rows = 80U, inst = 3U;
  PanelFixture fx = make_rising_panel(rows, inst, 100.0, 0.001, 1.0e3);
  const std::vector<f64> w_nan{0.5, 0.5, kNaN};
  const std::vector<f64> w_zero{0.5, 0.5, 0.0};
  const std::vector<f64> grid{1.0e3, 1.0e8, 1.0e12};
  const ExecutionSimulator sim = sim_with_Y(1.0);

  const std::vector<CapacityPoint> nan_pts =
      capacity_curve(std::span<const f64>{w_nan}, fx.view(), sim, std::span<const f64>{grid});
  const std::vector<CapacityPoint> zero_pts =
      capacity_curve(std::span<const f64>{w_zero}, fx.view(), sim, std::span<const f64>{grid});

  ASSERT_EQ(nan_pts.size(), zero_pts.size());
  for (usize k = 0; k < nan_pts.size(); ++k) {
    EXPECT_FALSE(std::isnan(nan_pts[k].net_edge_bps)) << "NaN leaked at k=" << k;
    EXPECT_NEAR(nan_pts[k].net_edge_bps, zero_pts[k].net_edge_bps, 1e-9);
  }
}

// ===========================================================================
//  boundary: a single-row (too-short) panel degenerates gracefully — no OOB, no
//  div-by-zero. With one row there are NO valid returns, so gross edge and sigma
//  are zero -> net edge is 0 at every AUM (and finite, non-NaN).
// ===========================================================================
TEST(RiskCapacity, ShortPanelDegenerates) {
  const usize rows = 1U, inst = 2U;
  PanelFixture fx{rows, inst, {{100.0, 100.0}}, {{1.0e3, 1.0e3}}};
  const std::vector<f64> w{0.5, 0.5};
  const std::vector<f64> grid{0.0, 1.0e6, 1.0e12};
  const ExecutionSimulator sim = sim_with_Y(1.0);

  const std::vector<CapacityPoint> pts =
      capacity_curve(std::span<const f64>{w}, fx.view(), sim, std::span<const f64>{grid});
  ASSERT_EQ(pts.size(), 3U);
  for (const CapacityPoint &p : pts) {
    EXPECT_FALSE(std::isnan(p.net_edge_bps));
    EXPECT_NEAR(p.net_edge_bps, 0.0, 1e-12); // no returns -> no edge, no sigma
  }
}

// ===========================================================================
//  HAND-COMPUTED net_edge_bps for one AUM. A 1-instrument book on a panel built
//  so every per-step return equals exactly `g`, and a single perturbed step that
//  makes sigma analytically tractable would be messy; instead we pick a CONSTANT
//  return series (sigma == 0) so the impact term is exactly zero — then the net
//  edge equals the gross edge for ALL AUM, which we pin to 1e4*g. This is the
//  cleanest closed-form check that the gross-edge reduction is correct.
// ===========================================================================
TEST(RiskCapacity, HandComputedConstantReturnZeroSigma) {
  const usize rows = 70U, inst = 1U;
  const f64 g = 0.0005; // every per-step return is exactly g (constant ratio)
  PanelFixture fx = make_rising_panel(rows, inst, 100.0, g, 1.0e3);
  const std::vector<f64> w{1.0};
  const std::vector<f64> grid{1.0e6, 1.0e10}; // sigma==0 -> impact==0 at any AUM
  const ExecutionSimulator sim = sim_with_Y(1.0);

  const std::vector<CapacityPoint> pts =
      capacity_curve(std::span<const f64>{w}, fx.view(), sim, std::span<const f64>{grid});
  ASSERT_EQ(pts.size(), 2U);
  const f64 expected = 1.0e4 * 1.0 * g; // 1e4 * w * g = 1e4 * g
  // Constant returns -> sigma == 0 -> zero impact at every AUM -> net == gross.
  EXPECT_NEAR(pts[0].net_edge_bps, expected, 1e-6);
  EXPECT_NEAR(pts[1].net_edge_bps, expected, 1e-6);
}

} // namespace
