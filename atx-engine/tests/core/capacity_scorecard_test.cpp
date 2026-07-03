// capacity_scorecard_test.cpp — p7-S4-3: CapacityScorecard + emit_capacity_scorecard.
//
// emit_capacity_scorecard(weights, panel, sim, aum_grid, target_aum) packages the
// AUM->net-edge sweep (cost::capacity_for_book -> risk::capacity_curve) into a
// first-class artefact:
//   capacity_point_aum : the zero-crossing AUM (cost::capacity_point).
//   gross_edge_bps     : the AUM-independent frictionless edge (risk::detail::
//                        gross_edge_bps over the usable edge window).
//   net_edge_at_target : the curve net_edge_bps at the grid point nearest target_aum
//                        (or the last point if target_aum exceeds the grid).
//   curve              : the full (aum, net_edge_bps) series in grid order, monotone
//                        non-increasing (the capacity-model contract).
// No existing source is modified; S7 wires it to the report KV block.
//
// Suite: CapacityScorecard
//   (b) MonotoneCrossesNearAnalytic — curve non-increasing, capacity_point within 5%
//       of the analytic crossing, gross>0, net_at_target<gross.
//   (c) TwiceRunBitIdentical        — same inputs -> bit-identical scorecard.
//   (d) ThreadSafePureFunction      — two threads, same input -> identical scorecard.
// (Class (a) off-path: no source call site is modified — the reviewer gate + the
//  factory byte-identity slice complete it; a determinism pin stands in here.)

#include <algorithm> // std::sort (grid construction)
#include <cmath>     // std::pow, std::sqrt, std::fabs, std::isnan, std::isfinite
#include <cstring>   // std::memcmp (bitwise determinism)
#include <limits>    // std::numeric_limits
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/cost/capacity.hpp"      // cost::emit_capacity_scorecard, CapacityScorecard
#include "atx/engine/exec/execution_sim.hpp" // ExecutionSimulator, ImpactCfg, …
#include "atx/engine/loop/panel_types.hpp"   // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"         // InstrumentId (Symbol)

namespace atxtest_capacity_scorecard_test {

using atx::f64;
using atx::u32;
using atx::u64;
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
namespace cost = atx::engine::cost;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// ===========================================================================
//  PanelFixture — file-local copy (mirrors risk_capacity_test.cpp / capacity_test).
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
      const usize phys = (n_rows_ - 1U) - r;
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
  std::vector<u64> mask_;
};

// ImpactCfg with explicit Y/delta; everything else default.
[[nodiscard]] ExecutionSimulator sim_with(f64 y, f64 delta) {
  ImpactCfg impact{};
  impact.Y = y;
  impact.delta = delta;
  return ExecutionSimulator{FillCfg{},       SlippageCfg{}, impact,
                            CommissionCfg{}, LatencyCfg{},  VolumeCapCfg{}};
}

// ===========================================================================
//  (b) A 4-name, 60-period panel with IDENTICAL names (same close + volume), so
//      the per-name cost arithmetic collapses to a single representative term and
//      the capacity AUM is analytically solvable. Uniform book w=0.25 each.
//
//  Model (capacity.hpp, post-S4-1 [B1 fix]): for identical names with Σ|w|=1,
//    cost_bps(aum) = 1e4 * Y * sigma * part^delta, part = (aum*0.25)/adv (notional
//    over dollar-ADV -- dollars/dollars, unitless; NOT (aum*0.25/price)/adv, the
//    pre-S4-1 shares/adv formula that was off by a factor of price).
//    Capacity is where gross_bps == cost_bps:
//      part* = (gross_bps / (1e4*Y*sigma))^(1/delta)
//      aum*  = part* * adv / 0.25
//  We compute sigma (popstd of the model's per-step returns over the vol window),
//  adv (mean close*volume over the adv window), and gross from the fixture
//  exactly as the model does, then solve the closed form.
// ===========================================================================
TEST(CapacityScorecard, MonotoneCrossesNearAnalytic) {
  const usize rows = 60U, inst = 4U;
  const f64 base = 100.0;
  const f64 hi = 0.004, lo = 0.0006; // alternating per-step returns -> sigma>0, edge>0
  const f64 vol_shares = 2.0e4;      // constant per-row volume

  // Build identical names: close[0]=base; older rows scaled down by the alternating
  // ratio so close(r)/close(r+1) = ratio -> per-step return alternates hi/lo.
  std::vector<std::vector<f64>> close(rows, std::vector<f64>(inst, 0.0));
  std::vector<std::vector<f64>> volume(rows, std::vector<f64>(inst, vol_shares));
  for (usize i = 0; i < inst; ++i) {
    close[0][i] = base;
    for (usize r = 1; r < rows; ++r) {
      const f64 ratio = (r % 2U == 1U) ? (1.0 + hi) : (1.0 + lo);
      close[r][i] = close[r - 1][i] / ratio;
    }
  }
  PanelFixture fx{rows, inst, close, volume};
  const PanelView panel = fx.view();

  const f64 Y = 1.0, delta = 0.5;
  const ExecutionSimulator sim = sim_with(Y, delta);
  const std::vector<f64> w(inst, 0.25);

  // --- analytic capacity AUM from the fixture's own quantities -------------------
  // Per-step return ret(r) = close(r)/close(r+1) - 1 for r in [0, rows-1). Newest-
  // first: ret(r) alternates hi/lo. Edge window = ALL usable returns (rows-1).
  std::vector<f64> rets;
  rets.reserve(rows - 1U);
  for (usize r = 0; r + 1U < rows; ++r) {
    rets.push_back(close[r][0] / close[r + 1][0] - 1.0);
  }
  // gross_bps = 1e4 * mean_r ( Σ_i w_i * ret_i(r) ) = 1e4 * mean_r(ret) (Σw=1, all
  // names identical). The model averages over rows that have a contributing term.
  f64 ret_sum = 0.0;
  for (const f64 r : rets) {
    ret_sum += r;
  }
  const f64 gross_bps = 1.0e4 * (ret_sum / static_cast<f64>(rets.size()));

  // sigma = popstd of the newest kCapacityVolWindow (=60, clamped to rows-1) returns.
  const usize w_vol = rets.size(); // 60-clamped == 59 here
  f64 mean = 0.0;
  for (usize k = 0; k < w_vol; ++k) {
    mean += rets[k];
  }
  mean /= static_cast<f64>(w_vol);
  f64 ss = 0.0;
  for (usize k = 0; k < w_vol; ++k) {
    ss += (rets[k] - mean) * (rets[k] - mean);
  }
  const f64 sigma = std::sqrt(ss / static_cast<f64>(w_vol));

  // adv = mean over the newest kCapacityAdvWindow (=20) rows of close*volume.
  const usize w_adv = 20U;
  f64 adv = 0.0;
  for (usize r = 0; r < w_adv; ++r) {
    adv += close[r][0] * vol_shares;
  }
  adv /= static_cast<f64>(w_adv);

  const f64 part_star = std::pow(gross_bps / (1.0e4 * Y * sigma), 1.0 / delta);
  const f64 analytic_aum = part_star * adv / 0.25; // S4-1: notional/adv, no price factor

  // --- a grid that brackets the analytic crossing (two decades each side) --------
  std::vector<f64> grid;
  for (int e = -2; e <= 2; ++e) {
    grid.push_back(analytic_aum * std::pow(10.0, static_cast<f64>(e)));
  }
  // refine near the crossing for a tighter interpolation
  grid.push_back(analytic_aum * 0.5);
  grid.push_back(analytic_aum * 2.0);
  std::sort(grid.begin(), grid.end());

  const f64 target_aum = analytic_aum * 0.5; // operational AUM below capacity
  const cost::CapacityScorecard sc =
      cost::emit_capacity_scorecard(std::span<const f64>{w}, panel, sim,
                                    std::span<const f64>{grid}, target_aum);

  // curve length matches the grid, monotone non-increasing.
  ASSERT_EQ(sc.curve.size(), grid.size());
  for (usize k = 1; k < sc.curve.size(); ++k) {
    EXPECT_LE(sc.curve[k].net_edge_bps, sc.curve[k - 1].net_edge_bps + 1e-9)
        << "curve must be monotone non-increasing at k=" << k;
  }
  // gross edge positive and matches the analytic gross.
  EXPECT_GT(sc.gross_edge_bps, 0.0);
  EXPECT_NEAR(sc.gross_edge_bps, gross_bps, 1e-6 * gross_bps)
      << "scorecard gross must equal the analytic frictionless edge";
  // capacity point within 5% of the analytic crossing.
  EXPECT_TRUE(std::isfinite(sc.capacity_point_aum));
  EXPECT_NEAR(sc.capacity_point_aum, analytic_aum, 0.05 * analytic_aum)
      << "capacity_point_aum=" << sc.capacity_point_aum << " analytic=" << analytic_aum;
  // net edge at the (below-capacity) target is positive but eroded below gross.
  EXPECT_LT(sc.net_edge_at_target, sc.gross_edge_bps)
      << "cost erodes the edge at any positive AUM";
  EXPECT_GT(sc.net_edge_at_target, 0.0) << "target below capacity -> still positive net edge";
}

// ===========================================================================
//  (c) Twice-run: bit-identical scorecard (capacity point, gross, net@target, and
//      the full curve) on two consecutive calls.
// ===========================================================================
TEST(CapacityScorecard, TwiceRunBitIdentical) {
  const usize rows = 60U, inst = 3U;
  std::vector<std::vector<f64>> close(rows, std::vector<f64>(inst, 0.0));
  std::vector<std::vector<f64>> volume(rows, std::vector<f64>(inst, 1.0e4));
  for (usize i = 0; i < inst; ++i) {
    close[0][i] = 80.0;
    for (usize r = 1; r < rows; ++r) {
      const f64 ratio = (r % 2U == 1U) ? 1.003 : 1.0005;
      close[r][i] = close[r - 1][i] / ratio;
    }
  }
  PanelFixture fx{rows, inst, close, volume};
  const std::vector<f64> w{0.4, 0.3, 0.3};
  const std::vector<f64> grid{1.0e5, 1.0e7, 1.0e9, 1.0e11};
  const ExecutionSimulator sim = sim_with(1.0, 0.5);

  const cost::CapacityScorecard a = cost::emit_capacity_scorecard(
      std::span<const f64>{w}, fx.view(), sim, std::span<const f64>{grid}, 1.0e7);
  const cost::CapacityScorecard b = cost::emit_capacity_scorecard(
      std::span<const f64>{w}, fx.view(), sim, std::span<const f64>{grid}, 1.0e7);

  EXPECT_EQ(std::memcmp(&a.capacity_point_aum, &b.capacity_point_aum, sizeof(f64)), 0);
  EXPECT_EQ(std::memcmp(&a.gross_edge_bps, &b.gross_edge_bps, sizeof(f64)), 0);
  EXPECT_EQ(std::memcmp(&a.net_edge_at_target, &b.net_edge_at_target, sizeof(f64)), 0);
  ASSERT_EQ(a.curve.size(), b.curve.size());
  EXPECT_EQ(std::memcmp(a.curve.data(), b.curve.data(),
                        a.curve.size() * sizeof(atx::engine::risk::CapacityPoint)),
            0)
      << "the curve series must be bit-identical on re-run";
}

// ===========================================================================
//  (d) Pure function: two threads with identical input produce identical scorecards.
// ===========================================================================
TEST(CapacityScorecard, ThreadSafePureFunction) {
  const usize rows = 50U, inst = 3U;
  std::vector<std::vector<f64>> close(rows, std::vector<f64>(inst, 0.0));
  std::vector<std::vector<f64>> volume(rows, std::vector<f64>(inst, 8.0e3));
  for (usize i = 0; i < inst; ++i) {
    close[0][i] = 120.0;
    for (usize r = 1; r < rows; ++r) {
      const f64 ratio = (r % 2U == 1U) ? 1.0025 : 1.0004;
      close[r][i] = close[r - 1][i] / ratio;
    }
  }
  PanelFixture fx{rows, inst, close, volume};
  const std::vector<f64> w{0.34, 0.33, 0.33};
  const std::vector<f64> grid{1.0e5, 1.0e7, 1.0e9, 1.0e11};
  const ExecutionSimulator sim = sim_with(1.0, 0.5);
  const PanelView panel = fx.view();

  cost::CapacityScorecard a;
  cost::CapacityScorecard b;
  std::thread t1([&] {
    a = cost::emit_capacity_scorecard(std::span<const f64>{w}, panel, sim,
                                      std::span<const f64>{grid}, 1.0e7);
  });
  std::thread t2([&] {
    b = cost::emit_capacity_scorecard(std::span<const f64>{w}, panel, sim,
                                      std::span<const f64>{grid}, 1.0e7);
  });
  t1.join();
  t2.join();

  EXPECT_EQ(std::memcmp(&a.capacity_point_aum, &b.capacity_point_aum, sizeof(f64)), 0);
  EXPECT_EQ(std::memcmp(&a.gross_edge_bps, &b.gross_edge_bps, sizeof(f64)), 0);
  EXPECT_EQ(std::memcmp(&a.net_edge_at_target, &b.net_edge_at_target, sizeof(f64)), 0);
  ASSERT_EQ(a.curve.size(), b.curve.size());
  EXPECT_EQ(std::memcmp(a.curve.data(), b.curve.data(),
                        a.curve.size() * sizeof(atx::engine::risk::CapacityPoint)),
            0);
}

} // namespace atxtest_capacity_scorecard_test
