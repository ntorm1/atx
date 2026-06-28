// capacity_vector_test.cpp — p7-S4-2: compute_capacity_vector, the per-alpha
// capacity-AUM vector that replaces the constant-1.0 stub in decorrelate_weights.
//
// compute_capacity_vector(streams, panel, sim, target_aum) returns, per alpha (in
// ascending alpha order), the capacity AUM (the zero-crossing of the net-edge
// curve) of that alpha's LAST-period target book, swept over a 20-point log-spaced
// grid from 0.01*target_aum to 10*target_aum. It REUSES capacity_for_alpha +
// capacity_point (no second cost model). A capacity of +inf means the grid never
// crossed zero (no penalty downstream); 0 means the last-period book has no
// positive frictionless edge.
//
// Suite: CapacityVector
//   (a) off-path: NOT exercised here (no source call site is modified — proven by
//       the reviewer's empty stage_combine.cpp diff + the factory byte-identity
//       gate run in CI). A determinism pin stands in.
//   (b) HighParticipationIsCapacityConstrained — a concentrated, tiny-ADV alpha has
//       a strictly lower capacity than a diffuse one, and below target_aum.
//   (c) TwiceRunBitIdentical — same inputs -> bit-identical vector.
//   (d) ThreadSafePureFunction — two threads, same input -> identical output.

#include <cmath>   // std::isfinite, std::isinf, std::isnan
#include <cstring> // std::memcmp (bitwise determinism)
#include <limits>  // std::numeric_limits (quiet NaN sentinel)
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/streams.hpp"      // alpha::AlphaStreams
#include "atx/engine/cost/capacity.hpp"      // cost::compute_capacity_vector
#include "atx/engine/exec/execution_sim.hpp" // ExecutionSimulator, ImpactCfg, …
#include "atx/engine/loop/panel_types.hpp"   // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"         // InstrumentId (Symbol)

namespace atxtest_capacity_vector_test {

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

[[nodiscard]] ExecutionSimulator sim_with_Y(f64 y) {
  ImpactCfg impact{};
  impact.Y = y;
  return ExecutionSimulator{FillCfg{},       SlippageCfg{}, impact,
                            CommissionCfg{}, LatencyCfg{},  VolumeCapCfg{}};
}

// A volatile rising panel with PER-NAME volume: name i trades `vol[i]` shares each
// row (so ADV_i scales with vol[i]). Alternating hi/lo returns -> sigma_i > 0, a
// positive average edge, and a non-trivial √-impact term that grows with AUM.
[[nodiscard]] PanelFixture make_panel_with_volumes(usize n_rows, usize n_inst, f64 base, f64 hi,
                                                   f64 lo, const std::vector<f64> &vol) {
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst, 0.0));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, 0.0));
  for (usize i = 0; i < n_inst; ++i) {
    close[0][i] = base;
    for (usize r = 1; r < n_rows; ++r) {
      const f64 ratio = (r % 2U == 1U) ? (1.0 + hi) : (1.0 + lo);
      close[r][i] = close[r - 1][i] / ratio;
    }
    for (usize r = 0; r < n_rows; ++r) {
      volume[r][i] = vol[i];
    }
  }
  return PanelFixture{n_rows, n_inst, close, volume};
}

// Build a single-period AlphaStreams whose last-period book for each alpha is the
// supplied weight cross-section. pnl is unused by compute_capacity_vector (it reads
// positions only), so it is a per-alpha zero stub.
[[nodiscard]] AlphaStreams make_streams(const std::vector<std::vector<f64>> &books, usize n_inst) {
  const usize n_alphas = books.size();
  AlphaStreams s;
  s.n_alphas_ = n_alphas;
  s.n_periods_ = 1U;
  s.n_instruments_ = n_inst;
  s.pnl_flat.assign(n_alphas * 1U, 0.0);
  s.pos_flat.assign(n_alphas * 1U * n_inst, 0.0);
  for (usize a = 0; a < n_alphas; ++a) {
    for (usize j = 0; j < n_inst; ++j) {
      s.pos_flat[a * n_inst + j] = books[a][j];
    }
  }
  return s;
}

// ===========================================================================
//  (b) On-path RED->GREEN: a high-participation alpha is capacity-constrained.
//      Alpha 0: uniform weight over 50 names, each with ample ADV (diffuse).
//      Alpha 1: 90% of weight on one TINY-ADV name (concentrated). Its capacity
//      must be strictly lower than alpha 0's AND below the target AUM.
// ===========================================================================
TEST(CapacityVector, HighParticipationIsCapacityConstrained) {
  const usize rows = 80U, inst = 50U;
  const f64 target_aum = 1.0e8; // $100M

  // Per-name ADV: every name liquid EXCEPT name 0, which is illiquid (tiny volume).
  std::vector<f64> vol(inst, 1.0e6); // 1M shares/row -> ample dollar ADV
  vol[0] = 1.0e2;                     // name 0: very thin -> high participation

  PanelFixture fx = make_panel_with_volumes(rows, inst, /*base=*/100.0, /*hi=*/0.003,
                                            /*lo=*/0.0002, vol);

  // Alpha 0: uniform long book over all 50 names (modest per-name participation).
  std::vector<f64> diffuse(inst, 1.0 / static_cast<f64>(inst));
  // Alpha 1: 90% on the thin name 0, 10% spread over the rest -> high participation
  // concentrated where ADV is tiny.
  std::vector<f64> concentrated(inst, 0.10 / static_cast<f64>(inst - 1U));
  concentrated[0] = 0.90;
  // Alphas 2 and 3: more diffuse/concentrated variants to exercise the loop length.
  std::vector<f64> alpha2(inst, 1.0 / static_cast<f64>(inst));
  std::vector<f64> alpha3(inst, 0.05 / static_cast<f64>(inst - 1U));
  alpha3[0] = 0.95;

  AlphaStreams streams = make_streams({diffuse, concentrated, alpha2, alpha3}, inst);
  const ExecutionSimulator sim = sim_with_Y(1.0);

  const std::vector<f64> cap = cost::compute_capacity_vector(streams, fx.view(), sim, target_aum);

  ASSERT_EQ(cap.size(), 4U);
  for (const f64 c : cap) {
    EXPECT_FALSE(std::isnan(c)) << "capacity must never be NaN";
    EXPECT_GE(c, 0.0) << "capacity AUM is non-negative (0, finite, or +inf)";
  }
  // The concentrated, thin-ADV alpha is MORE capacity-constrained than the diffuse.
  EXPECT_GT(cap[0], cap[1]) << "diffuse alpha must out-capacity the concentrated one";
  // And it crosses zero BELOW the target AUM (it IS constrained at $100M).
  EXPECT_LT(cap[1], target_aum) << "the high-participation alpha is constrained at $100M";
  // The even-more-concentrated alpha 3 is the most constrained of the four.
  EXPECT_LT(cap[3], cap[1]) << "alpha 3 (95% on the thin name) must be tighter still";
}

// ===========================================================================
//  (c) Twice-run: bit-identical vector on two consecutive calls.
// ===========================================================================
TEST(CapacityVector, TwiceRunBitIdentical) {
  const usize rows = 60U, inst = 8U;
  std::vector<f64> vol(inst, 5.0e3);
  vol[2] = 1.0e2; // one thin name
  PanelFixture fx = make_panel_with_volumes(rows, inst, 50.0, 0.0015, 0.0003, vol);

  std::vector<f64> book0(inst, 1.0 / static_cast<f64>(inst));
  std::vector<f64> book1(inst, 0.0);
  book1[2] = 0.7;
  book1[3] = 0.3;
  AlphaStreams streams = make_streams({book0, book1}, inst);
  const ExecutionSimulator sim = sim_with_Y(1.2);

  const std::vector<f64> a = cost::compute_capacity_vector(streams, fx.view(), sim, 1.0e7);
  const std::vector<f64> b = cost::compute_capacity_vector(streams, fx.view(), sim, 1.0e7);
  ASSERT_EQ(a.size(), b.size());
  EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size() * sizeof(f64)), 0)
      << "compute_capacity_vector must be bit-deterministic";
}

// ===========================================================================
//  (d) Pure function: two threads with the same input produce identical output
//      (no shared mutable state).
// ===========================================================================
TEST(CapacityVector, ThreadSafePureFunction) {
  const usize rows = 60U, inst = 6U;
  std::vector<f64> vol(inst, 5.0e3);
  vol[1] = 2.0e2;
  PanelFixture fx = make_panel_with_volumes(rows, inst, 80.0, 0.002, 0.0004, vol);

  std::vector<f64> book0(inst, 1.0 / static_cast<f64>(inst));
  std::vector<f64> book1(inst, 0.0);
  book1[1] = 0.8;
  book1[0] = 0.2;
  AlphaStreams streams = make_streams({book0, book1}, inst);
  const ExecutionSimulator sim = sim_with_Y(1.0);
  const PanelView panel = fx.view();

  std::vector<f64> out_a;
  std::vector<f64> out_b;
  std::thread t1([&] { out_a = cost::compute_capacity_vector(streams, panel, sim, 1.0e7); });
  std::thread t2([&] { out_b = cost::compute_capacity_vector(streams, panel, sim, 1.0e7); });
  t1.join();
  t2.join();

  ASSERT_EQ(out_a.size(), out_b.size());
  EXPECT_EQ(std::memcmp(out_a.data(), out_b.data(), out_a.size() * sizeof(f64)), 0)
      << "parallel calls with identical input must be bit-identical (pure function)";
}

} // namespace atxtest_capacity_vector_test
