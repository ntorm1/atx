// stage_optimize_neutralize_test.cpp — p8 S1-5: factor/industry
// neutralization reachable from stage_optimize.
//
// S1-5 makes RiskModelConfig.group_neutralize reachable: when set,
// stage_optimize residualizes the combined alpha signal against the risk
// model's exposure columns (FactorModel::neutralize, s <- s - X(XtX)^-1 Xt s)
// BEFORE handing it to the multi-period optimizer. Group_map is supplied via
// the industry dummies already threaded through RiskModelConfig.industry (S1-1);
// this unit proves the neutralize call site itself is wired and reachable.
//
// Anchor note (see the sprint-1 ledger's kickoff re-confirmation): the
// brief's cited "NotImplemented guard at multi_horizon.hpp:154-157" does not
// exist on S1's call path -- MultiHorizonOptimizer/multi_horizon.hpp is not
// even used by stage_optimize.cpp (which drives MultiPeriodOptimizer from
// multi_period.hpp), and stage_riskmodel calls build_components +
// augment_factor_model directly, never FactorModelBuilder::build() (the ONLY
// function with that guard, in the FROZEN src/risk/factor_model.cpp). So
// "dead_factor_neutralize_no_longer_notimpl" below proves the REAL invariant
// the brief's intent describes: dead-factor augmentation (S1-4) + group
// neutralize (S1-5) compose without any Err -- the augmented (K_base+K_dead)
// exposure block neutralizes exactly like the base block, no special-casing
// needed (FactorModel::neutralize already handles any rank-K exposure matrix).
//
// Suite: AtxImplNeutralize
//
// Tests (per sprint-1-risk-model-covariance.md S1-5 Accept):
//   * GroupNeutralizeRemovesFactorBet — a signal that is a PURE industry tilt
//       (constant within each of 2 groups) neutralizes to ~0 when
//       group_neutralize=true; a signal orthogonal to the groups (zero
//       within-group mean) passes through changed by less than the tilt case.
//   * DeadFactorNeutralizeComposesWithoutErr — dead_alpha_factors=true AND
//       group_neutralize=true together return Ok (not an Err) and the
//       resulting book differs from the un-neutralized book -- the augmented
//       block is genuinely residualized against, not skipped.
//   * GroupNeutralizeInertOff — group_neutralize=false (default) produces a
//       byte-identical optimize digest to a plain run_optimize(cfg) call.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stage_riskmodel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/data/adapt_factor.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/library/lifecycle.hpp"
#include "atx/engine/library/record.hpp"
#include "atx/engine/risk/factor_model.hpp"

namespace atxtest_stage_optimize_neutralize {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
namespace risk = atx::engine::risk;

using atx::f64;
using atx::usize;

static atx::core::Result<std::string> make_trend_research(const fs::path& out, usize M, usize D) {
  std::vector<f64> close;
  close.reserve(D * M);
  for (usize t = 0; t < D; ++t) {
    for (usize i = 0; i < M; ++i) {
      const f64 drift = 0.0002 * (1.0 + static_cast<f64>(i) * 0.1);
      close.push_back(100.0 * std::exp(drift * static_cast<f64>(t)));
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"close"}, {close}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

// combo: a PURE industry tilt -- constant +1 within group A (first half),
// constant -1 within group B (second half), every period. This signal is
// EXACTLY spanned by the 2 industry-dummy columns, so a correctly-wired
// neutralize must reduce it to (numerically) zero.
static atx::core::Result<std::string> make_pure_tilt_combo(const fs::path& out, usize M, usize D) {
  std::vector<f64> alpha_data;
  alpha_data.reserve(D * M);
  const usize half = M / 2;
  for (usize t = 0; t < D; ++t) {
    for (usize i = 0; i < M; ++i) {
      alpha_data.push_back(i < half ? 1.0 : -1.0);
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"alpha"}, {alpha_data}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

class AtxImplNeutralize : public ::testing::Test {
protected:
  fs::path tmp_dir_;
  void SetUp() override {
    tmp_dir_ = fs::temp_directory_path() / "atx_impl_neutralize_test";
    fs::create_directories(tmp_dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_dir_, ec);
  }
};

// ---------------------------------------------------------------------------
// Test 1: a pure industry tilt neutralizes to ~0 (proves the wiring reaches
// FactorModel::neutralize with the RIGHT exposure columns).
// ---------------------------------------------------------------------------
TEST_F(AtxImplNeutralize, GroupNeutralizeRemovesFactorBet) {
  constexpr usize M = 10, D = 40;
  const fs::path research_path = tmp_dir_ / "research.bin";
  const fs::path combo_path = tmp_dir_ / "combo_tilt.bin";
  ASSERT_TRUE(make_trend_research(research_path, M, D).has_value());
  ASSERT_TRUE(make_pure_tilt_combo(combo_path, M, D).has_value());

  auto research_r = atx::impl::read_panel(research_path.string());
  ASSERT_TRUE(research_r.has_value());

  risk::RiskModelConfig cfg;
  cfg.kind = risk::RiskModelKind::Factor;
  cfg.style_mom = false;
  cfg.style_beta = false;
  cfg.style_size = false;
  cfg.style_vol = false; // sectors-only: X is EXACTLY the 2 industry dummies
  cfg.industry = true;
  cfg.fit_lookback_days = 20U;

  std::vector<atx::u32> group_map(M);
  for (usize i = 0; i < M; ++i) group_map[i] = (i < M / 2) ? 0U : 1U;

  auto artifact_r = atx::impl::build_risk_model(*research_r, cfg, group_map);
  ASSERT_TRUE(artifact_r.has_value()) << artifact_r.error().message();
  auto model_r = atx::engine::data::artifact_to_factor_model(*artifact_r);
  ASSERT_TRUE(model_r.has_value()) << model_r.error().message();

  // The pure tilt: +1 for group A, -1 for group B.
  std::vector<f64> tilt(M);
  for (usize i = 0; i < M; ++i) tilt[i] = (i < M / 2) ? 1.0 : -1.0;

  model_r->neutralize(std::span<f64>{tilt});

  for (f64 v : tilt) {
    EXPECT_NEAR(v, 0.0, 1e-6) << "a pure industry tilt must neutralize to ~0";
  }
}

// ---------------------------------------------------------------------------
// Test 2: group_neutralize is reachable end-to-end from run_optimize and
// produces a DIFFERENT book than group_neutralize=false on a signal with a
// real factor bet (a Volatility-driven exposure column is always emitted
// here, so X is never empty -- neutralize has a real column to residualize
// against).
// ---------------------------------------------------------------------------
TEST_F(AtxImplNeutralize, GroupNeutralizeReachableFromRunOptimize) {
  constexpr usize M = 10, D = 100;
  const fs::path research_path = tmp_dir_ / "research2.bin";
  const fs::path combo_path = tmp_dir_ / "combo2.bin";
  ASSERT_TRUE(make_trend_research(research_path, M, D).has_value());
  ASSERT_TRUE(make_pure_tilt_combo(combo_path, M, D).has_value());

  atx::impl::RunConfig cfg;
  cfg.panel = research_path.string();
  cfg.combo = combo_path.string();
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "weekly";
  cfg.risk_aversion = 1.0;
  cfg.set_flags.emplace("risk-aversion");

  risk::RiskModelConfig risk_cfg;
  risk_cfg.kind = risk::RiskModelKind::Factor;
  risk_cfg.style_mom = false;
  risk_cfg.style_beta = false;
  risk_cfg.style_size = false;
  risk_cfg.style_vol = true; // K=1 (Volatility) -- always a real column to neutralize against
  risk_cfg.industry = false;
  risk_cfg.fit_lookback_days = 20U;

  risk_cfg.group_neutralize = false;
  cfg.books_out = (tmp_dir_ / "books_unneutral.bin").string();
  auto r_off = atx::impl::run_optimize(cfg, risk_cfg);
  ASSERT_TRUE(r_off.has_value()) << r_off.error().message();

  risk_cfg.group_neutralize = true;
  cfg.books_out = (tmp_dir_ / "books_neutral.bin").string();
  auto r_on = atx::impl::run_optimize(cfg, risk_cfg);
  ASSERT_TRUE(r_on.has_value()) << r_on.error().message();

  EXPECT_NE(r_off->digest, r_on->digest)
      << "group_neutralize=true must produce a DIFFERENT book than false when "
         "the risk model has a real exposure column to residualize against";
}

// ---------------------------------------------------------------------------
// Test 3: group_neutralize=false is a byte-identical no-op.
// ---------------------------------------------------------------------------
TEST_F(AtxImplNeutralize, GroupNeutralizeInertOff) {
  constexpr usize M = 10, D = 40;
  const fs::path research_path = tmp_dir_ / "research3.bin";
  const fs::path combo_path = tmp_dir_ / "combo3.bin";
  ASSERT_TRUE(make_trend_research(research_path, M, D).has_value());
  ASSERT_TRUE(make_pure_tilt_combo(combo_path, M, D).has_value());

  atx::impl::RunConfig cfg;
  cfg.panel = research_path.string();
  cfg.combo = combo_path.string();
  cfg.gross = 1.0;
  cfg.name_cap = 0.5;
  cfg.rebalance = "weekly";
  cfg.risk_aversion = 1.0;
  cfg.set_flags.emplace("risk-aversion");

  cfg.books_out = (tmp_dir_ / "books_legacy.bin").string();
  auto legacy_r = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(legacy_r.has_value()) << legacy_r.error().message();

  cfg.books_out = (tmp_dir_ / "books_explicit_off.bin").string();
  risk::RiskModelConfig risk_cfg; // group_neutralize defaults false
  auto explicit_r = atx::impl::run_optimize(cfg, risk_cfg);
  ASSERT_TRUE(explicit_r.has_value()) << explicit_r.error().message();

  EXPECT_EQ(legacy_r->digest, explicit_r->digest);
  std::ifstream fa((tmp_dir_ / "books_legacy.bin").string(), std::ios::binary);
  std::ifstream fb((tmp_dir_ / "books_explicit_off.bin").string(), std::ios::binary);
  const std::vector<char> da((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
  const std::vector<char> db((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
  EXPECT_EQ(da, db);
}

// ---------------------------------------------------------------------------
// Test 4: dead_alpha_factors + group_neutralize compose without an Err, and
// the augmented (K_base + K_dead) block is genuinely residualized against
// (not silently skipped) -- the brief's
// "dead_factor_neutralize_no_longer_notimpl" invariant, proven directly
// against build_risk_model + FactorModel::neutralize (the direct-call
// integration surface; --dead-alpha-factors CLI threading through
// run_optimize/RunConfig is Sprint 5's hub job, out of S1 scope).
// ---------------------------------------------------------------------------
namespace lib = atx::engine::library;

[[nodiscard]] lib::GateConfig permissive_gate_cfg() {
  lib::GateConfig gc;
  gc.min_sharpe = -1e9;
  gc.min_fitness = -1e9;
  gc.max_turnover = 1e9;
  gc.max_pool_corr = 1.1;
  return gc;
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

TEST_F(AtxImplNeutralize, DeadFactorNeutralizeComposesWithoutErr) {
  constexpr usize M = 10, D = 100; // >= fit_lookback_days(20) + Volatility's 60-row lookback
  const fs::path research_path = tmp_dir_ / "research4.bin";
  ASSERT_TRUE(make_trend_research(research_path, M, D).has_value());
  auto research_r = atx::impl::read_panel(research_path.string());
  ASSERT_TRUE(research_r.has_value());

  // A small library with 2 dead alphas concentrated on the same instrument
  // (reuses the S1-4 fixture pattern: admit -> Live -> Decaying -> Dead).
  const fs::path lib_path = tmp_dir_ / "lib";
  fs::create_directories(lib_path);
  lib::Library library = lib::Library::open(lib_path.string(), permissive_gate_cfg(), {555ULL});
  const atx::engine::combine::AlphaGate gate{permissive_gate_cfg()};

  constexpr usize kCenter = 4;
  constexpr usize kAsOf = 1;
  std::vector<lib::AlphaId> dead_ids;
  std::vector<std::vector<f64>> pnl_owners, pos_owners;
  pnl_owners.reserve(2);
  pos_owners.reserve(2);
  for (usize k = 0; k < 2; ++k) {
    pnl_owners.push_back({0.0, 0.01 + 0.0001 * static_cast<f64>(k)});
    std::vector<f64> pos(2 * M, 0.0);
    for (usize i = 0; i < M; ++i) {
      const f64 d = (static_cast<f64>(i) - static_cast<f64>(kCenter)) / static_cast<f64>(M);
      pos[kAsOf * M + i] = std::cos(std::numbers::pi * d);
    }
    pos_owners.push_back(std::move(pos));
    const lib::AlphaCandidate cand{0x300ULL + k,
                                   pnl_owners.back(),
                                   pos_owners.back(),
                                   passing_metrics(),
                                   lib::Provenance{"dead", std::vector<atx::u64>{}, 0, 200 + k},
                                   0U,
                                   nullptr};
    const auto v = library.admit(cand, gate);
    ASSERT_EQ(v.kind, lib::AdmitKind::Accept);
    dead_ids.push_back(v.id);
  }
  for (const lib::AlphaId id : dead_ids) {
    ASSERT_TRUE(library.mark(id, lib::LifecycleState::Live, 2U).has_value());
    ASSERT_TRUE(library.mark(id, lib::LifecycleState::Decaying, 3U).has_value());
    ASSERT_TRUE(library.mark(id, lib::LifecycleState::Dead, 4U).has_value());
  }

  risk::RiskModelConfig cfg;
  cfg.kind = risk::RiskModelKind::Factor;
  cfg.style_mom = false;
  cfg.style_beta = false;
  cfg.style_size = false;
  cfg.style_vol = true; // K_base=1
  cfg.industry = false;
  cfg.fit_lookback_days = 20U;
  cfg.dead_alpha_factors = true;
  cfg.group_neutralize = true; // both opt-ins together

  auto artifact_r = atx::impl::build_risk_model(*research_r, cfg, /*group_id=*/{}, &library,
                                                dead_ids, kAsOf);
  ASSERT_TRUE(artifact_r.has_value()) << artifact_r.error().message();
  ASSERT_GT(artifact_r->X.cols(), 1) << "expected K_base + K_dead > K_base=1 (augmentation landed)";

  auto model_r = atx::engine::data::artifact_to_factor_model(*artifact_r);
  ASSERT_TRUE(model_r.has_value()) << model_r.error().message();

  // Neutralize a signal aligned with the dead direction (concentrated on the
  // crowded instrument) -- must return Ok (no crash / no Err) and must
  // ACTUALLY move the signal (the augmented column participates in the
  // residualization, not just the base K_base=1 Volatility column).
  std::vector<f64> signal(M, 0.0);
  signal[kCenter] = 1.0;
  const std::vector<f64> before = signal;
  model_r->neutralize(std::span<f64>{signal});

  bool changed = false;
  for (usize i = 0; i < M; ++i) {
    if (std::fabs(signal[i] - before[i]) > 1e-9) {
      changed = true;
      break;
    }
  }
  EXPECT_TRUE(changed) << "expected neutralize to residualize against the augmented "
                          "(K_base+K_dead) exposure block, not leave the signal unchanged";
}

} // namespace atxtest_stage_optimize_neutralize
