// stage_optimize_dead_alpha_determinism_test.cpp -- p9 S1-2: determinism
// hardening for the S1-1 dead-alpha wire (twice-run + dead-id order invariance).
//
// Suite: AtxImplOptimizeDeadAlphaDeterminism
//
// S1-1 built a deterministic path BY CONSTRUCTION (no RNG, no clock, no
// unordered-map iteration in the new helpers), so this unit is a regression
// GUARD, not a new-behavior RED->GREEN -- both tests are expected green
// immediately. They exist so a future change that silently introduces
// nondeterminism (e.g. a map-ordered dead-id scan, or an extract_dead_factors
// that stops re-sorting its input) fails loudly here.
//
// (c) twice-run     -- the full wire (open-library -> collect-ids -> augment ->
//     optimize) reproduces byte-identical books across two independent
//     run_optimize calls against the SAME on-disk library + panel + config.
// (d) seq==parallel analog -- build_risk_model's artifact does not depend on the
//     CALLER's dead_ids ORDERING: extract_dead_factors sorts its input by
//     ascending AlphaId internally (dead_factor.hpp, the R1 bit-reproducibility
//     contract), so feeding the SAME dead-id set ascending vs. shuffled must
//     yield byte-identical artifacts -- proving collect_dead_alpha_ids's own
//     ascending-scan order is a documented convention, never a correctness
//     dependency.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stage_riskmodel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/data/factor_model_artifact.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/library/lifecycle.hpp"
#include "atx/engine/library/record.hpp"
#include "atx/engine/risk/factor_model.hpp"

namespace atxtest_stage_optimize_dead_alpha_determinism {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
namespace data = atx::engine::data;
namespace risk = atx::engine::risk;
namespace lib = atx::engine::library;

using atx::f64;
using atx::usize;
using lib::AlphaId;

// -- fixture helpers (local; this suite keeps no cross-file test dependency,
//    matching stage_optimize_dead_alpha_wire_test.cpp's own convention) --------

// A gently-trending panel serialized to disk (for the run_optimize path).
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

// The same trending panel built IN-MEMORY (for the direct build_risk_model
// order-invariance path, which takes a Panel, not a file).
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

// combo: fixed long-first-half / short-second-half alpha, constant across time.
static atx::core::Result<std::string> make_pair_combo(const fs::path& out, usize M, usize D) {
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

// An ON-DISK library at `dir` with n_dead alphas all concentrated on `center`
// (rank-1 overlap), flushed and closed so a later independent Library::open
// (the one run_optimize's wire performs) sees every admit on disk.
void seed_crowded_library(const fs::path& dir, usize n_dead, usize m, usize center) {
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  lib::Library library = lib::Library::open(dir.string(), permissive_gate_cfg(), {777ULL});
  const atx::engine::combine::AlphaGate gate{permissive_gate_cfg()};
  constexpr usize kT = 2U;
  for (usize k = 0; k < n_dead; ++k) {
    std::vector<f64> pnl(kT, 0.0);
    pnl[1] = 0.01 + 0.0001 * static_cast<f64>(k);
    std::vector<f64> pos(kT * m, 0.0);
    for (usize i = 0; i < m; ++i) {
      const f64 d = (static_cast<f64>(i) - static_cast<f64>(center)) / static_cast<f64>(m);
      pos[1 * m + i] = std::cos(std::numbers::pi * d);
    }
    const lib::AlphaCandidate cand{0x300ULL + k, pnl, pos, passing_metrics(),
                                   lib::Provenance{"dead", std::vector<atx::u64>{}, 0, 100 + k},
                                   0U, nullptr};
    const auto v = library.admit(cand, gate);
    ASSERT_EQ(v.kind, lib::AdmitKind::Accept);
  }
  ASSERT_TRUE(library.flush_all().has_value());
}

// An IN-PROCESS library at `dir` with n_dead alphas concentrated on `center`,
// walked to LifecycleState::Dead -- mirrors p8's stage_riskmodel_dead_factor_
// test.cpp::make_dead_lib, returning a live handle + its ascending dead ids so
// the order-invariance test can call build_risk_model directly. Holdings are
// stored at as_of == 1 (the second stored period), matching that fixture.
struct DeadLibFixture {
  std::unique_ptr<lib::Library> library;
  std::vector<AlphaId> dead_ids; // ascending {0,1,...} by admission order
  usize as_of = 1U;
};

[[nodiscard]] DeadLibFixture make_dead_lib(const fs::path& dir, usize n_dead, usize m, usize center) {
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  DeadLibFixture fx;
  fx.as_of = 1U;
  fx.library = std::make_unique<lib::Library>(
      lib::Library::open(dir.string(), permissive_gate_cfg(), {777ULL}));
  const atx::engine::combine::AlphaGate gate{permissive_gate_cfg()};
  constexpr usize kT = 2U;
  std::vector<AlphaId> ids;
  ids.reserve(n_dead);
  for (usize k = 0; k < n_dead; ++k) {
    std::vector<f64> pnl(kT, 0.0);
    pnl[1] = 0.01 + 0.0001 * static_cast<f64>(k);
    std::vector<f64> pos(kT * m, 0.0);
    for (usize i = 0; i < m; ++i) {
      const f64 d = (static_cast<f64>(i) - static_cast<f64>(center)) / static_cast<f64>(m);
      pos[fx.as_of * m + i] = std::cos(std::numbers::pi * d); // SAME center -> rank-1 overlap
    }
    const lib::AlphaCandidate cand{0x200ULL + k, pnl, pos, passing_metrics(),
                                   lib::Provenance{"dead", std::vector<atx::u64>{}, 0, 100 + k},
                                   0U, nullptr};
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

class AtxImplOptimizeDeadAlphaDeterminism : public ::testing::Test {
protected:
  fs::path tmp_dir_;
  void SetUp() override {
    tmp_dir_ = fs::temp_directory_path() / "atx_p9_s1_determinism_test";
    std::error_code ec;
    fs::remove_all(tmp_dir_, ec);
    fs::create_directories(tmp_dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_dir_, ec);
  }
};

// -- (c) twice-run: the full wire is byte-identical across two run_optimize
//    calls against the SAME on-disk library + panel + config. -----------------
TEST_F(AtxImplOptimizeDeadAlphaDeterminism, TwiceRunByteIdentical) {
  constexpr usize M = 10, D = 40;
  const usize center = 3U;
  const fs::path research_path = tmp_dir_ / "research.bin";
  const fs::path combo_path = tmp_dir_ / "combo.bin";
  ASSERT_TRUE(make_trend_research(research_path, M, D).has_value());
  ASSERT_TRUE(make_pair_combo(combo_path, M, D).has_value());
  seed_crowded_library(tmp_dir_ / "lib", /*n_dead=*/3U, M, center);

  atx::impl::RunConfig cfg;
  cfg.panel = research_path.string();
  cfg.combo = combo_path.string();
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "weekly";
  cfg.risk_aversion = 1.0;
  cfg.set_flags.emplace("risk-aversion");
  cfg.dead_alpha_factors = true;
  cfg.dead_alpha_lib_dir = (tmp_dir_ / "lib").string();

  cfg.books_out = (tmp_dir_ / "books_a.bin").string();
  auto r1 = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();

  cfg.books_out = (tmp_dir_ / "books_b.bin").string();
  auto r2 = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  EXPECT_EQ(r1->digest, r2->digest)
      << "the dead-alpha wire must reproduce an identical books digest across two "
         "independent run_optimize calls (same library + panel + config)";

  std::ifstream fa((tmp_dir_ / "books_a.bin").string(), std::ios::binary);
  std::ifstream fb((tmp_dir_ / "books_b.bin").string(), std::ios::binary);
  const std::vector<char> da((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
  const std::vector<char> db((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
  EXPECT_EQ(da, db) << "books.bin not byte-identical across the two dead-alpha-wire runs";
}

// -- (d) seq==parallel analog: build_risk_model's artifact does not depend on
//    the caller's dead_ids ORDER (extract_dead_factors re-sorts internally). ---
TEST_F(AtxImplOptimizeDeadAlphaDeterminism, DeadIdOrderInvariant) {
  constexpr usize M = 10, D = 40;
  const usize center = 3U;
  auto panel_r = make_trend_panel(M, D);
  ASSERT_TRUE(panel_r.has_value()) << panel_r.error().message();

  DeadLibFixture fx = make_dead_lib(tmp_dir_ / "deadlib", /*n_dead=*/3U, M, center);
  ASSERT_EQ(fx.dead_ids.size(), 3U);

  // The wire's own collection order is ascending by construction; feed the SAME
  // set in ascending and a non-trivially shuffled order.
  const std::vector<AlphaId> ascending = fx.dead_ids;                                  // {0,1,2}
  const std::vector<AlphaId> shuffled = {fx.dead_ids[2], fx.dead_ids[0], fx.dead_ids[1]}; // {2,0,1}
  ASSERT_NE(ascending, shuffled) << "shuffled order must genuinely differ from ascending";

  risk::RiskModelConfig cfg; // Diagonal base; dead-alpha augmentation is orthogonal to kind
  cfg.dead_alpha_factors = true;

  auto a_asc = atx::impl::build_risk_model(*panel_r, cfg, /*group_id=*/{}, fx.library.get(),
                                           ascending, fx.as_of);
  auto a_shuf = atx::impl::build_risk_model(*panel_r, cfg, /*group_id=*/{}, fx.library.get(),
                                            shuffled, fx.as_of);
  ASSERT_TRUE(a_asc.has_value()) << a_asc.error().message();
  ASSERT_TRUE(a_shuf.has_value()) << a_shuf.error().message();

  // Sanity: the augmentation actually fired (else "order invariance" would be a
  // vacuous equality of two un-augmented artifacts). A rank-1 dead pool adds
  // exactly one crowding column, so the augmented X has more columns than a
  // bare Diagonal base.
  risk::RiskModelConfig off_cfg;
  off_cfg.dead_alpha_factors = false;
  auto a_base = atx::impl::build_risk_model(*panel_r, off_cfg);
  ASSERT_TRUE(a_base.has_value()) << a_base.error().message();
  EXPECT_GT(a_asc->X.cols(), a_base->X.cols())
      << "dead-alpha augmentation did not fire -- order-invariance check would be vacuous";

  EXPECT_EQ(data::serialize_artifact(*a_asc), data::serialize_artifact(*a_shuf))
      << "build_risk_model must produce a byte-identical artifact regardless of the "
         "caller's dead_ids ordering (extract_dead_factors re-sorts internally)";
}

} // namespace atxtest_stage_optimize_dead_alpha_determinism
