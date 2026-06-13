// capacity_test.cpp — S6-3: per-alpha + per-mega-book capacity wrappers +
// capacity_point root-find.  Suite: Capacity.
//
// C6: every assertion verifies behaviour of the risk::capacity_curve wrapper;
// no second impact formula lives in cost/capacity.hpp.
//
// Tests:
//   CurveMonotoneDecreasingInAum    — §0.4 concave impact
//   CapacityPoint_FiniteOnRealEdgeFixture — spec exit criterion
//   ReusesSimImpactCfg_NoSecondModel — C6: more impact -> lower capacity
//   PerAlpha_FromStreams              — per-alpha last-period weight path

#include <cmath>   // std::isfinite, std::pow
#include <limits>  // std::numeric_limits
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/streams.hpp"       // alpha::AlphaStreams
#include "atx/engine/cost/capacity.hpp"        // cost::capacity_for_book, capacity_for_alpha, capacity_point
#include "atx/engine/exec/execution_sim.hpp"   // ExecutionSimulator, ImpactCfg, …
#include "atx/engine/loop/panel_types.hpp"     // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"           // InstrumentId (Symbol)

namespace {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::core::domain::Symbol;
using atx::engine::InstrumentId;
using atx::engine::kPanelFieldCount;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::alpha::AlphaStreams;
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
//  PanelFixture — file-local copy (mirrors risk_capacity_test.cpp exactly).
//  Owns the backing storage for one PanelView used by the Capacity tests.
// ===========================================================================
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>>& close,
               const std::vector<std::vector<f64>>& volume)
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
        set(PanelField::Open,   phys, i, c);
        set(PanelField::High,   phys, i, c);
        set(PanelField::Low,    phys, i, c);
        set(PanelField::Close,  phys, i, c);
        set(PanelField::Volume, phys, i, v);
        if (!std::isnan(c)) {
          mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
        }
      }
    }
  }

  [[nodiscard]] PanelView view() const noexcept {
    return PanelView{fields_.data(), mask_.data(),
                     std::span<const InstrumentId>{universe_},
                     cap_, head_(), n_rows_, mask_words_};
  }

private:
  [[nodiscard]] usize head_() const noexcept { return (n_rows_ == 0U) ? 0U : n_rows_ - 1U; }

  static usize pow2_ceil(usize n) noexcept {
    usize p = 1U;
    while (p < n) { p <<= 1U; }
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

// A sim with calibrated impact-scale Y; everything else at defaults.
[[nodiscard]] ExecutionSimulator sim_with_Y(f64 y) {
  ImpactCfg impact{};
  impact.Y = y;
  return ExecutionSimulator{FillCfg{}, SlippageCfg{}, impact,
                            CommissionCfg{}, LatencyCfg{}, VolumeCapCfg{}};
}

// Volatile rising panel: alternating hi/lo returns -> sigma > 0 -> sqrt-impact
// cost grows with AUM and the curve crosses zero on a wide enough grid.
// Mirrors make_volatile_rising_panel from risk_capacity_test.cpp exactly.
[[nodiscard]] PanelFixture make_volatile_rising_panel(usize n_rows, usize n_inst, f64 base,
                                                      f64 hi, f64 lo, f64 vol) {
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst, 0.0));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, vol));
  for (usize i = 0; i < n_inst; ++i) {
    close[0][i] = base;
    for (usize r = 1; r < n_rows; ++r) {
      const f64 ratio = (r % 2U == 1U) ? (1.0 + hi) : (1.0 + lo);
      close[r][i] = close[r - 1][i] / ratio;
    }
  }
  return PanelFixture{n_rows, n_inst, close, volume};
}

// ===========================================================================
//  Suite: Capacity
// ===========================================================================

// §0.4 concave impact: net edge must be monotone non-increasing across an
// ascending AUM grid for a positive-edge volatile book.
TEST(Capacity, CurveMonotoneDecreasingInAum) {
  auto fx = make_volatile_rising_panel(80, 2, 100.0, 0.003, 0.0002, 1.0e3);
  const std::vector<f64> w{0.5, 0.5};
  const std::vector<f64> grid{1.0e3, 1.0e5, 1.0e7, 1.0e9, 1.0e11, 1.0e13, 1.0e15};
  auto curve = cost::capacity_for_book(std::span<const f64>{w}, fx.view(),
                                       sim_with_Y(1.0), std::span<const f64>{grid});
  ASSERT_EQ(curve.size(), grid.size());
  for (size_t i = 1; i < curve.size(); ++i) {
    EXPECT_LE(curve[i].net_edge_bps, curve[i - 1].net_edge_bps + 1e-9);
  }
}

// Spec exit criterion: capacity_point returns a finite, positive AUM for the
// real-edge volatile fixture on a wide grid.
TEST(Capacity, CapacityPoint_FiniteOnRealEdgeFixture) {
  auto fx = make_volatile_rising_panel(80, 2, 100.0, 0.003, 0.0002, 1.0e3);
  const std::vector<f64> w{0.5, 0.5};
  const std::vector<f64> grid{1.0e3, 1.0e5, 1.0e7, 1.0e9, 1.0e11, 1.0e13, 1.0e15};
  const f64 cap = cost::capacity_point(
      cost::capacity_for_book(std::span<const f64>{w}, fx.view(),
                              sim_with_Y(1.0), std::span<const f64>{grid}));
  EXPECT_GT(cap, 0.0);
  EXPECT_TRUE(std::isfinite(cap));
}

// C6: capacity_for_book reads the sim's ImpactCfg (no second model).
// A higher Y means steeper impact -> lower capacity point.
TEST(Capacity, ReusesSimImpactCfg_NoSecondModel) {
  auto fx = make_volatile_rising_panel(80, 2, 100.0, 0.003, 0.0002, 1.0e3);
  const std::vector<f64> w{0.5, 0.5};
  const std::vector<f64> grid{1.0e3, 1.0e5, 1.0e7, 1.0e9, 1.0e11, 1.0e13, 1.0e15};
  const f64 cap_lo = cost::capacity_point(
      cost::capacity_for_book(std::span<const f64>{w}, fx.view(),
                              sim_with_Y(0.5), std::span<const f64>{grid}));
  const f64 cap_hi = cost::capacity_point(
      cost::capacity_for_book(std::span<const f64>{w}, fx.view(),
                              sim_with_Y(2.0), std::span<const f64>{grid}));
  // More impact (higher Y) -> lower capacity.
  EXPECT_LT(cap_hi, cap_lo);
}

// Per-alpha path: build a minimal AlphaStreams (1 alpha, 1 period, 2 instruments)
// with last-period weights {0.5, 0.5}; capacity_for_alpha uses those weights.
TEST(Capacity, PerAlpha_FromStreams) {
  auto fx = make_volatile_rising_panel(80, 2, 100.0, 0.003, 0.0002, 1.0e3);
  const std::vector<f64> grid{1.0e3, 1.0e5, 1.0e7, 1.0e9};

  // Aggregate-init: members are public (per streams.hpp:95-103).
  // pnl_flat: [n_alphas * n_periods] = [1*1] = {0.0}
  // pos_flat: [n_alphas * n_periods * n_instruments] = [1*1*2] = {0.5, 0.5}
  AlphaStreams streams{
      /*pnl_flat=*/std::vector<f64>{0.0},
      /*pos_flat=*/std::vector<f64>{0.5, 0.5},
      /*n_alphas_=*/1U,
      /*n_periods_=*/1U,
      /*n_instruments_=*/2U,
  };

  auto curve = cost::capacity_for_alpha(streams, /*alpha_idx=*/0U, fx.view(),
                                        sim_with_Y(1.0), std::span<const f64>{grid});
  EXPECT_FALSE(curve.empty());
  ASSERT_EQ(curve.size(), grid.size());
  // Basic sanity: the curve is non-increasing.
  for (size_t i = 1; i < curve.size(); ++i) {
    EXPECT_LE(curve[i].net_edge_bps, curve[i - 1].net_edge_bps + 1e-9);
  }
}

} // namespace
