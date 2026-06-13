// atx::engine::cost — End-to-end S6 chain integration suite (S6-5).
//
// The per-unit suites (Calibration, TempPerm, Capacity, CostAware, Borrow) each
// prove ONE seam. This suite proves the WHOLE chain composes — the S6-1
// calibrated coefficients flow into a reconstructed sim, that sim drives S6-3
// capacity, and the same CalibratedCost drives the S6-4 decision knobs — and that
// the chain keeps the C-rule firewalls it pins per-unit:
//
//   EndToEnd_CalibrateThenCapacityThenGate — the chain composes; calibrated
//       coefficients reach the sim, capacity is finite, the gate knob is live.
//   FullChain_ReplaysByteIdentical (C5)    — the calibration is byte-stable: the
//       same observations digest identically across two runs.
//   CostModelNeverPeeks_TruncationInvariant (C2) — a trailing-window fit over a
//       short prefix vs a longer prefix (same window) yields identical impact
//       coefficients; appending future fills cannot move a closed window.
//
// Fixtures are NOT shared across the cost test suites (each replicates its own in
// an anonymous namespace), so the synthetic-obs / panel / sim helpers are
// re-created file-local here, mirroring calibration_test.cpp and capacity_test.cpp.

#include <gtest/gtest.h>

#include <array>
#include <cmath>  // std::isfinite, std::pow
#include <limits> // std::numeric_limits (NaN panel sentinel)
#include <span>
#include <vector>

#include "atx/core/datetime.hpp" // Timestamp
#include "atx/core/hash.hpp"     // hash_bytes (C2 / C5 byte identity)
#include "atx/core/random.hpp"   // Xoshiro256pp (synthetic fixtures only)
#include "atx/core/types.hpp"    // f64, i64, u32, u64, usize

#include "atx/engine/cost/calibration.hpp"   // calibrate_from_obs, calibrate_impact, CostObs
#include "atx/engine/cost/capacity.hpp"      // capacity_for_book, capacity_point
#include "atx/engine/cost/cost_aware.hpp"    // cost_aware_knobs, CostKnobs
#include "atx/engine/exec/execution_sim.hpp" // ExecutionSimulator + cfg structs
#include "atx/engine/exec/payloads.hpp"      // FillPayload
#include "atx/engine/loop/market.hpp"        // Market, InstrumentStats
#include "atx/engine/loop/panel_types.hpp"   // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"         // InstrumentId (Symbol)

namespace {

using atx::f64;
using atx::i64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::core::domain::Symbol;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::kPanelFieldCount;
using atx::engine::Market;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::FillPayload;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::VolumeCapCfg;
namespace cost = atx::engine::cost;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

[[nodiscard]] InstrumentId inst(u32 id) noexcept { return InstrumentId{id}; }

// --- synthetic_obs — verbatim shape from calibration_test.cpp. Each row is
// built from the EXACT structural laws the calibration inverts:
//   temp = Y · σ · p^δ ;   perm = 0.5 · γ · σ · p,  with light multiplicative noise.
[[nodiscard]] std::vector<cost::CostObs>
synthetic_obs(f64 Y, f64 delta, f64 gamma, usize n, u64 seed) {
  atx::core::Xoshiro256pp rng{seed};
  std::vector<cost::CostObs> obs;
  obs.reserve(n);
  for (usize i = 0; i < n; ++i) {
    const f64 sigma = 0.02;
    const f64 p = rng.uniform(0.001, 0.10);
    const f64 temp = Y * sigma * std::pow(p, delta) * (1.0 + 0.02 * rng.normal());
    const f64 perm = 0.5 * gamma * sigma * p * (1.0 + 0.02 * rng.normal());
    obs.push_back(cost::CostObs{p, sigma, temp, perm});
  }
  return obs;
}

// ===========================================================================
//  PanelFixture — file-local copy (mirrors capacity_test.cpp exactly). Owns the
//  backing storage for one PanelView used by the capacity leg of the chain.
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

  void set(PanelField f, usize phys, usize inst_i, f64 v) noexcept {
    const usize block = static_cast<usize>(f) * cap_ * n_inst_;
    fields_[block + phys * n_inst_ + inst_i] = v;
  }

  usize n_rows_;
  usize n_inst_;
  usize cap_;
  usize mask_words_;
  std::vector<InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<u64> mask_;
};

// Volatile rising panel — mirrors make_volatile_rising_panel from capacity_test.cpp.
[[nodiscard]] PanelFixture make_volatile_rising_panel(usize n_rows, usize n_inst, f64 base, f64 hi,
                                                      f64 lo, f64 vol) {
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

// Hash the three impact coefficients (C2 / C5 byte identity).
[[nodiscard]] u64 hash_impact(const ImpactCfg& c) {
  const std::array<f64, 3> buf{c.Y, c.delta, c.gamma};
  return atx::core::hash_bytes(buf.data(), buf.size() * sizeof(f64));
}

// =============================================================================
//  Suite: CostIntegration
// =============================================================================

// The whole chain composes: calibrate (S6-1) -> reconstruct a sim with the
// calibrated coefficients -> capacity (S6-3) -> decision knobs (S6-4). Each stage
// consumes the previous stage's output unchanged.
TEST(CostIntegration, EndToEnd_CalibrateThenCapacityThenGate) {
  auto obs = synthetic_obs(/*Y=*/0.8, /*d=*/0.55, /*g=*/0.3, /*n=*/2000, /*seed=*/9);
  auto cc = cost::calibrate_from_obs(obs, ImpactCfg{});

  // Calibrated coefficients flow into a reconstructed sim (the cfg the sim charges
  // IS the calibrated cfg — no second model).
  auto sim = ExecutionSimulator{FillCfg{},       SlippageCfg{}, cc.impact,
                                CommissionCfg{}, LatencyCfg{},  VolumeCapCfg{}};
  EXPECT_NEAR(sim.impact_cfg().delta, 0.55, 0.03);
  EXPECT_NEAR(sim.impact_cfg().delta, cc.impact.delta, 1e-12); // identity, not a re-fit

  // S6-3 capacity over the calibrated sim — finite on a real-edge volatile book.
  auto panel = make_volatile_rising_panel(80, 2, 100.0, 0.003, 0.0002, 1.0e3);
  const std::vector<f64> book_w{0.5, 0.5};
  const std::vector<f64> grid{1.0e3, 1.0e5, 1.0e7, 1.0e9, 1.0e11, 1.0e13, 1.0e15};
  const f64 cap = cost::capacity_point(cost::capacity_for_book(
      std::span<const f64>{book_w}, panel.view(), sim, std::span<const f64>{grid}));
  EXPECT_TRUE(std::isfinite(cap));
  EXPECT_GT(cap, 0.0);

  // S6-4 decision knobs from the SAME CalibratedCost — a positive κ at a
  // representative (participation, σ) over a finite horizon.
  auto knobs = cost::cost_aware_knobs(cc, /*ref_participation=*/0.01, /*ref_sigma=*/0.02,
                                      /*horizon_days=*/5.0);
  EXPECT_GT(knobs.kappa, 0.0);
}

// C5 — full-chain determinism. The calibration is byte-stable: the same
// observations digest to the same impact coefficients across two runs (RNG-free fit).
TEST(CostIntegration, FullChain_ReplaysByteIdentical) {
  auto digest = [] {
    auto cc = cost::calibrate_from_obs(synthetic_obs(0.8, 0.55, 0.3, 2000, 9), ImpactCfg{});
    const std::array<f64, 3> b{cc.impact.Y, cc.impact.delta, cc.impact.gamma};
    return atx::core::hash_bytes(b.data(), b.size() * sizeof(f64));
  };
  EXPECT_EQ(digest(), digest());
}

// C2 — the cost model never peeks, at the chain level. A trailing-window fit over
// a short in-window prefix and over a longer prefix that only ADDS strictly-later
// fills (same window cutoff) recovers identical impact coefficients: future fills
// cannot move a closed window. (The same firewall the S6-1 unit test pins, proven
// here end-to-end on a real fill stream + Market.)
TEST(CostIntegration, CostModelNeverPeeks_TruncationInvariant) {
  std::array<InstrumentId, 1> uni{inst(7)};
  std::array<InstrumentStats, 1> stats{
      InstrumentStats{/*adv=*/1.0e6, /*sigma=*/0.02, /*spread=*/0.05}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{stats}};

  const i64 window = 1000;
  atx::core::Xoshiro256pp rng{99ULL};
  std::vector<FillPayload> in_window;
  for (i64 k = 0; k < window; ++k) {
    FillPayload f{};
    f.id = inst(7);
    f.qty = static_cast<i64>(rng.uniform(100.0, 50000.0));
    f.impact = 1.0 * 0.02 * std::pow(static_cast<f64>(f.qty) / 1.0e6, 0.5);
    f.t = atx::core::time::Timestamp::from_unix_nanos(k);
    in_window.push_back(f);
  }
  std::vector<FillPayload> longer = in_window;
  for (i64 k = window; k < window + 500; ++k) {
    FillPayload f{};
    f.id = inst(7);
    f.qty = static_cast<i64>(rng.uniform(100.0, 50000.0));
    f.impact = 1.0 * 0.02 * std::pow(static_cast<f64>(f.qty) / 1.0e6, 0.5);
    f.t = atx::core::time::Timestamp::from_unix_nanos(k); // strictly later — excluded
    longer.push_back(f);
  }

  auto a = cost::calibrate_impact(std::span<const FillPayload>{in_window}, mkt,
                                  static_cast<usize>(window), ImpactCfg{});
  auto b = cost::calibrate_impact(std::span<const FillPayload>{longer}, mkt,
                                  static_cast<usize>(window), ImpactCfg{});
  EXPECT_EQ(hash_impact(a.impact), hash_impact(b.impact));
}

} // namespace
