// stage_riskmodel_dead_factor_test.cpp — p8 S1-4: dead-alpha crowding
// factors wired into build_risk_model.
//
// S1-4 wires risk/dead_factor.hpp (Kakushadze-Yu holdings-overlap eigen-
// extraction) into the stage_riskmodel producer: when cfg.dead_alpha_factors,
// retired ("Dead" lifecycle) alpha holdings are pulled from the library,
// extract_dead_factors builds the crowding directions, and
// augment_factor_model raises variance on those directions BEFORE the
// artifact is serialized -- a pure transform on the already-built FactorModel
// (no re-estimation of the style/industry block).
//
// Suite: AtxImplDeadFactor
//
// Tests (per sprint-1-risk-model-covariance.md S1-4 Accept):
//   * RaisesCrowdedVariance   — with two synthetic "dead" alphas whose
//       holdings overlap a live book direction, dead_alpha_factors=true makes
//       risk(w) for that direction STRICTLY higher than without augmentation.
//   * InertOff                — dead_alpha_factors=false (default) => artifact
//       byte-identical to the pre-augmentation (S1-1) artifact.
//   * ERankTruncationAddsExactlyMinKErank — augmenting with k dead alphas
//       whose holdings concentrate on ONE direction (eRank == 1) adds EXACTLY
//       min(k, eRank) == 1 column to X.

#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/data/adapt_factor.hpp"
#include "atx/engine/data/factor_model_artifact.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/library/lifecycle.hpp"
#include "atx/engine/library/record.hpp"
#include "atx/engine/risk/factor_model.hpp"

#include "stage_riskmodel.hpp"

namespace atxtest_stage_riskmodel_dead_factor {

namespace alpha = atx::engine::alpha;
namespace data = atx::engine::data;
namespace risk = atx::engine::risk;
namespace lib = atx::engine::library;

using atx::f64;
using atx::usize;
using lib::AlphaId;

// A gently-trending research panel, M instruments, D dates -- shape matches
// stage_riskmodel_test.cpp's fixture (the Diagonal path is what dead-factor
// augmentation composes with by default in these tests: dead-factor
// augmentation is orthogonal to Diagonal-vs-Factor kind selection, so the
// simplest base model exercises it cleanly).
static atx::core::Result<alpha::Panel> make_trend_panel(usize M, usize D) {
  std::vector<f64> close;
  close.reserve(D * M);
  for (usize t = 0; t < D; ++t) {
    for (usize i = 0; i < M; ++i) {
      const f64 drift = 0.0002 * (1.0 + static_cast<f64>(i) * 0.1);
      close.push_back(100.0 * std::exp(drift * static_cast<f64>(t)));
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  return alpha::Panel::create(D, M, {"close"}, {close}, uni);
}

[[nodiscard]] std::string tmpdir(const std::string& tag) {
  const auto dir = std::filesystem::temp_directory_path() / "atx_s1_4_deadfac" / tag;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

[[nodiscard]] lib::GateConfig permissive_gate_cfg() {
  lib::GateConfig cfg;
  cfg.min_sharpe = -1e9;
  cfg.min_fitness = -1e9;
  cfg.max_turnover = 1e9;
  cfg.max_pool_corr = 1.1;
  return cfg;
}

[[nodiscard]] atx::engine::combine::AlphaMetrics passing_metrics() {
  atx::engine::combine::AlphaMetrics m{};
  m.sharpe = 5.0;
  m.turnover = 0.05;
  m.returns = 1.0;
  m.drawdown = 0.1;
  m.margin = 10.0;
  m.fitness = 5.0;
  m.holding_days = 20.0;
  return m;
}

// A dead alpha's holdings cross-section: a cosine bump over M instruments,
// centered at instrument `center` -- concentrated on a FEW names so the
// overlap matrix (across the dead pool) has a real, low eRank spectrum.
[[nodiscard]] std::vector<f64> bump_holdings(usize center, usize m) {
  std::vector<f64> h(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    const f64 d = (static_cast<f64>(i) - static_cast<f64>(center)) / static_cast<f64>(m);
    h[i] = std::cos(std::numbers::pi * d);
  }
  return h;
}

// Build a library with `n_dead` alphas, ALL concentrated on the SAME
// instrument center (so the overlap matrix is (near-)rank-1 -- eRank == 1) at
// as_of period `as_of`, walked to LifecycleState::Dead. Returns (library,
// dead_ids, as_of, M).
struct DeadLibFixture {
  std::unique_ptr<lib::Library> library;
  std::vector<AlphaId> dead_ids;
  usize as_of = 0;
  usize m = 0;
};

[[nodiscard]] DeadLibFixture make_dead_lib(usize n_dead, usize m, usize center) {
  DeadLibFixture fx;
  fx.m = m;
  fx.as_of = 1U;
  const std::string dir = tmpdir(std::to_string(n_dead) + "_" + std::to_string(m));
  fx.library = std::make_unique<lib::Library>(
      lib::Library::open(dir, permissive_gate_cfg(), {/*master seed*/ 777ULL}));
  const atx::engine::combine::AlphaGate gate{permissive_gate_cfg()};

  constexpr usize kT = 2U;
  struct Owner {
    std::vector<f64> pnl;
    std::vector<f64> pos_flat;
  };
  std::vector<Owner> owners(n_dead);
  std::vector<AlphaId> ids;
  ids.reserve(n_dead);

  for (usize k = 0; k < n_dead; ++k) {
    Owner& o = owners[k];
    o.pnl.assign(kT, 0.0);
    o.pnl[1] = 0.01 + 0.0001 * static_cast<f64>(k);
    o.pos_flat.assign(kT * m, 0.0);
    const std::vector<f64> h = bump_holdings(center, m); // SAME center every k -> rank-1 overlap
    for (usize i = 0; i < m; ++i) {
      o.pos_flat[fx.as_of * m + i] = h[i];
    }
    const lib::AlphaCandidate cand{/*canon_hash*/ 0x200ULL + k,
                                   o.pnl,
                                   o.pos_flat,
                                   passing_metrics(),
                                   lib::Provenance{"dead", std::vector<atx::u64>{}, 0, 100 + k},
                                   /*as_of*/ 0U,
                                   /*source*/ nullptr};
    const auto v = fx.library->admit(cand, gate);
    EXPECT_EQ(v.kind, lib::AdmitKind::Accept) << "dead candidate " << k << " not admitted";
    ids.push_back(v.id);
  }
  for (const AlphaId id : ids) {
    EXPECT_TRUE(fx.library->mark(id, lib::LifecycleState::Live, 2U).has_value());
    EXPECT_TRUE(fx.library->mark(id, lib::LifecycleState::Decaying, 3U).has_value());
    EXPECT_TRUE(fx.library->mark(id, lib::LifecycleState::Dead, 4U).has_value());
  }
  fx.dead_ids = std::move(ids);
  return fx;
}

// ---------------------------------------------------------------------------
// Test 1: dead-factor augmentation raises risk() on the crowded direction.
// ---------------------------------------------------------------------------
TEST(AtxImplDeadFactor, RaisesCrowdedVariance) {
  constexpr usize M = 10, D = 40;
  auto panel_r = make_trend_panel(M, D);
  ASSERT_TRUE(panel_r.has_value()) << panel_r.error().message();

  const usize center = 3U; // the crowded instrument
  DeadLibFixture fx = make_dead_lib(/*n_dead=*/2U, M, center);

  risk::RiskModelConfig cfg; // Diagonal base
  cfg.dead_alpha_factors = false;
  auto base_r = atx::impl::build_risk_model(*panel_r, cfg);
  ASSERT_TRUE(base_r.has_value()) << base_r.error().message();
  auto base_model_r = data::artifact_to_factor_model(*base_r);
  ASSERT_TRUE(base_model_r.has_value());

  cfg.dead_alpha_factors = true;
  auto aug_r = atx::impl::build_risk_model(*panel_r, cfg, /*group_id=*/{}, fx.library.get(),
                                           fx.dead_ids, fx.as_of);
  ASSERT_TRUE(aug_r.has_value()) << aug_r.error().message();
  auto aug_model_r = data::artifact_to_factor_model(*aug_r);
  ASSERT_TRUE(aug_model_r.has_value()) << aug_model_r.error().message();

  // A book aligned with the dead direction: concentrated on `center` (the
  // crowded instrument the dead pool overlapped), near-zero elsewhere.
  std::vector<f64> w(M, 0.0);
  w[center] = 1.0;

  const f64 base_risk = base_model_r->risk(w);
  const f64 aug_risk = aug_model_r->risk(w);
  EXPECT_GT(aug_risk, base_risk)
      << "expected dead-factor augmentation to STRICTLY raise risk() on the "
         "crowded direction: base=" << base_risk << " augmented=" << aug_risk;
}

// ---------------------------------------------------------------------------
// Test 2: dead_alpha_factors=false is a byte-identical no-op.
// ---------------------------------------------------------------------------
TEST(AtxImplDeadFactor, InertOff) {
  constexpr usize M = 10, D = 40;
  auto panel_r = make_trend_panel(M, D);
  ASSERT_TRUE(panel_r.has_value()) << panel_r.error().message();

  DeadLibFixture fx = make_dead_lib(2U, M, 3U);

  risk::RiskModelConfig cfg;
  cfg.dead_alpha_factors = false;

  auto no_lib_r = atx::impl::build_risk_model(*panel_r, cfg);
  ASSERT_TRUE(no_lib_r.has_value());

  // Even WITH a library + dead_ids supplied, dead_alpha_factors=false must be
  // a complete no-op -- the library/ids are simply never consulted.
  auto with_lib_r = atx::impl::build_risk_model(*panel_r, cfg, /*group_id=*/{}, fx.library.get(),
                                                fx.dead_ids, fx.as_of);
  ASSERT_TRUE(with_lib_r.has_value());

  EXPECT_EQ(data::serialize_artifact(*no_lib_r), data::serialize_artifact(*with_lib_r))
      << "dead_alpha_factors=false must be byte-identical regardless of library/dead_ids presence";
}

// ---------------------------------------------------------------------------
// Test 3: eRank truncation adds exactly min(k, eRank) columns.
// ---------------------------------------------------------------------------
TEST(AtxImplDeadFactor, ERankTruncationAddsExactlyMinKErank) {
  constexpr usize M = 10, D = 40;
  auto panel_r = make_trend_panel(M, D);
  ASSERT_TRUE(panel_r.has_value()) << panel_r.error().message();

  // All dead alphas concentrate on the SAME center -> the overlap matrix is
  // (numerically) rank-1 -> eRank == 1 -> exactly 1 column added regardless
  // of how many dead alphas (k) contributed to it.
  DeadLibFixture fx = make_dead_lib(/*n_dead=*/4U, M, /*center=*/5U);

  risk::RiskModelConfig cfg;
  cfg.dead_alpha_factors = false;
  auto base_r = atx::impl::build_risk_model(*panel_r, cfg);
  ASSERT_TRUE(base_r.has_value());
  const Eigen::Index base_k = base_r->X.cols();

  cfg.dead_alpha_factors = true;
  auto aug_r = atx::impl::build_risk_model(*panel_r, cfg, /*group_id=*/{}, fx.library.get(),
                                           fx.dead_ids, fx.as_of);
  ASSERT_TRUE(aug_r.has_value()) << aug_r.error().message();
  const Eigen::Index aug_k = aug_r->X.cols();

  EXPECT_EQ(aug_k, base_k + 1)
      << "expected exactly 1 (eRank==1) added column; base_k=" << base_k << " aug_k=" << aug_k;
}

} // namespace atxtest_stage_riskmodel_dead_factor
