// stage_metabook_dead_alpha_wire_test.cpp -- p9 S2-3 (amendment): prove S1's dead-alpha
// crowding defense reaches the MEGA-BOOK (run_metabook's Factor path), closing ROADMAP
// R3/R4. The flagship recipe sets --metabook, so stage_run.cpp:127 makes stage_optimize
// (S1's wire) NEVER run on it; crowding defense that lands only in stage_optimize is a
// Potemkin win on a dead path. S2-2's metabook Factor loop now threads the shared
// dead_alpha_wire (maybe_open_dead_lib / collect_dead_alpha_ids), so a crowded-pool library
// sizes DOWN the crowded instrument in the mega-book -- RED before the wire (nullptr =>
// identical weights), GREEN after.
//
// Honesty note: the augmentation is a PURE transform on the built FactorModel covariance
// (risk::augment_factor_model), so it can only bite when build_risk_model's Factor branch
// actually PRODUCES a model -- not the degenerate warm-up diagonal fallback (whose
// diag_fallback_cfg gates the augmentation off). A genuine Factor fit on a small synthetic
// fixture needs a well-determined K: we therefore drive the augmentation through the 3-arg
// build_metabook_result overload with a styles-off (K=1, Volatility-only) Factor cfg -- the
// SAME proven-fittable config stage_optimize_riskmodel_test uses. This isolates the R3/R4
// question ("does the mega-book sleeve optimizer CONSUME the augmented covariance?") from
// the orthogonal "is a synthetic fixture large enough for a K>=4 styles-on fit?" question
// (production data is; a 10-name toy panel is not). The fail-open guard below exercises the
// real 2-arg CLI forwarder path.
//
// Suite: MetabookDeadAlphaWire

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/library/lifecycle.hpp"
#include "atx/engine/library/record.hpp"
#include "atx/engine/risk/factor_model.hpp"

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stage_metabook.hpp"

namespace atxtest_stage_metabook_dead_alpha_wire {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
namespace lib = atx::engine::library;
namespace risk = atx::engine::risk;
using atx::impl::MetaBookStageConfig;
using atx::f64;
using atx::usize;

// --- fixtures (copied locally: this codebase forbids cross-file test deps) -------------

// A common-shock correlated research panel (matches stage_optimize_riskmodel_test.cpp's
// make_correlated_research). Enough real cross-sectional structure that build_risk_model's
// Factor branch genuinely fits (so the dead-alpha augmentation has a live FactorModel to
// transform).
[[nodiscard]] atx::core::Result<std::string> make_correlated_research(const fs::path &out,
                                                                      usize M, usize D) {
  std::vector<f64> common_shock(D);
  for (usize t = 0; t < D; ++t) {
    common_shock[t] = 0.01 * std::sin(0.31 * static_cast<f64>(t));
  }
  std::vector<f64> close(D * M, 100.0);
  for (usize i = 0; i < M; ++i) {
    const f64 idio_amp = 0.0005 * (1.0 + static_cast<f64>(i % 7));
    f64 level = 100.0;
    for (usize t = 0; t < D; ++t) {
      const f64 ret =
          common_shock[t] + idio_amp * std::sin(0.9 * static_cast<f64>(t) + static_cast<f64>(i));
      if (t > 0) level *= (1.0 + ret);
      close[t * M + i] = level;
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"close"}, {close}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

// combo: a fixed long-first-half / short-second-half alpha signal, CONSTANT across periods
// (matches stage_optimize_dead_alpha_wire_test.cpp::make_pair_combo). center (below) lives
// in the LONG half, so the base mega-book carries a positive weight there for the
// augmented covariance to size down.
[[nodiscard]] atx::core::Result<std::string> make_pair_combo(const fs::path &out, usize M,
                                                             usize D) {
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

// Build an ON-DISK library at `dir` with n_dead alphas all concentrated on instrument
// `center` (rank-1 overlap -- the SAME fixture shape S1's stage_optimize_dead_alpha_wire_
// test.cpp uses), then flush + let it go out of scope so a later independent Library::open
// (the one the metabook wire performs) sees every admit on disk.
void seed_crowded_library(const fs::path &dir, usize n_dead, usize m, usize center) {
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  lib::Library library = lib::Library::open(dir.string(), permissive_gate_cfg(), {777ULL});
  const atx::engine::combine::AlphaGate gate{permissive_gate_cfg()};
  constexpr usize kT = 2U;
  std::vector<std::vector<f64>> pnls(n_dead), positions(n_dead);
  for (usize k = 0; k < n_dead; ++k) {
    pnls[k].assign(kT, 0.0);
    pnls[k][1] = 0.01 + 0.0001 * static_cast<f64>(k);
    positions[k].assign(kT * m, 0.0);
    for (usize i = 0; i < m; ++i) {
      const f64 d = (static_cast<f64>(i) - static_cast<f64>(center)) / static_cast<f64>(m);
      positions[k][1 * m + i] = std::cos(std::numbers::pi * d);
    }
    const lib::AlphaCandidate cand{0x300ULL + k, pnls[k], positions[k], passing_metrics(),
                                   lib::Provenance{"dead", std::vector<atx::u64>{}, 0, 100 + k},
                                   0U, nullptr};
    const auto v = library.admit(cand, gate);
    ASSERT_EQ(v.kind, lib::AdmitKind::Accept);
  }
  ASSERT_TRUE(library.flush_all().has_value());
}

[[nodiscard]] std::string tmp_dir(const std::string &tag) {
  const auto dir = fs::temp_directory_path() / "atx_s2_mb_dead" / tag;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir.string();
}

// A styles-off (K=1, Volatility-only) Factor cfg -- the proven-fittable shape that lets
// build_risk_model's Factor branch produce a real model on a 10-name synthetic panel.
[[nodiscard]] risk::RiskModelConfig fittable_factor_cfg(bool dead_alpha_factors) {
  risk::RiskModelConfig rc{};
  rc.kind = risk::RiskModelKind::Factor;
  rc.fit_lookback_days = 100U;
  rc.style_size = false;
  rc.style_mom = false;
  rc.style_beta = false;
  rc.industry = false;
  rc.dead_alpha_factors = dead_alpha_factors;
  return rc;
}

// ===========================================================================
//  (b) on-path RED->GREEN: crowding sizes DOWN the crowded instrument in the mega-book.
// ===========================================================================
TEST(MetabookDeadAlphaWire, CrowdingDeleversMegaBook) {
  constexpr usize M = 10, D = 160;
  const usize center = 3U;
  const std::string dir = tmp_dir("delever");
  const fs::path research_path = fs::path(dir) / "research.bin";
  const fs::path combo_path = fs::path(dir) / "combo.bin";
  ASSERT_TRUE(make_correlated_research(research_path, M, D).has_value());
  ASSERT_TRUE(make_pair_combo(combo_path, M, D).has_value());
  const fs::path lib_dir = fs::path(dir) / "crowded_lib";
  seed_crowded_library(lib_dir, /*n_dead=*/3U, M, center);

  const MetaBookStageConfig scfg;

  // Baseline: kind==Factor, crowding defense OFF.
  atx::impl::RunConfig cfg_base;
  cfg_base.panel = research_path.string();
  cfg_base.combo = combo_path.string();
  cfg_base.gross = 1.0;
  cfg_base.name_cap = 1.0;
  cfg_base.rebalance = "weekly";
  auto r_base = atx::impl::build_metabook_result(cfg_base, scfg, fittable_factor_cfg(false));
  ASSERT_TRUE(r_base.has_value()) << r_base.error().message();

  // Delevered: same, but crowding defense ON with the crowded library.
  atx::impl::RunConfig cfg_delev = cfg_base;
  cfg_delev.dead_alpha_lib_dir = lib_dir.string();
  auto r_delev = atx::impl::build_metabook_result(cfg_delev, scfg, fittable_factor_cfg(true));
  ASSERT_TRUE(r_delev.has_value()) << r_delev.error().message();

  ASSERT_EQ(r_base->fund_books.size(), r_delev->fund_books.size());
  ASSERT_GT(r_base->fund_books.size(), 0U);
  const usize last = r_base->fund_books.size() - 1;
  ASSERT_GT(r_base->fund_books[last].size(), center);
  const f64 w_base = r_base->fund_books[last][center];
  const f64 w_delev = r_delev->fund_books[last][center];

  EXPECT_LT(std::fabs(w_delev), std::fabs(w_base))
      << "expected the crowded instrument's mega-book weight to shrink once the dead-alpha "
         "wire is live on the metabook Factor path: base="
      << w_base << " delevered=" << w_delev;
  EXPECT_NE(std::bit_cast<std::uint64_t>(w_base), std::bit_cast<std::uint64_t>(w_delev))
      << "the two mega-books must differ once the gate is live (else the wire did nothing)";
}

// ===========================================================================
//  (regression guard) mega-book fail-open: a nonexistent --dead-alpha-lib-dir with the gate
//  ON must be byte-identical to the gate-off run -- the metabook analog of S1's
//  FailOpen_MissingDirByteIdentical, exercised through the REAL 2-arg CLI forwarder path.
// ===========================================================================
TEST(MetabookDeadAlphaWire, MegaBookFailOpenByteIdentical) {
  constexpr usize M = 10, D = 160;
  const std::string dir = tmp_dir("failopen");
  const fs::path research_path = fs::path(dir) / "research.bin";
  const fs::path combo_path = fs::path(dir) / "combo.bin";
  ASSERT_TRUE(make_correlated_research(research_path, M, D).has_value());
  ASSERT_TRUE(make_pair_combo(combo_path, M, D).has_value());

  const MetaBookStageConfig scfg;

  atx::impl::RunConfig cfg;
  cfg.panel = research_path.string();
  cfg.combo = combo_path.string();
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "weekly";
  cfg.risk_model = "factor";

  // Gate ON, but the resolved dead-alpha dir does not exist -> must fail OPEN (no abort),
  // producing a book byte-identical to the gate-off run. Goes through the 2-arg forwarder
  // (the form stage_run.cpp:127 calls), which propagates cfg.dead_alpha_factors into risk_cfg.
  cfg.dead_alpha_factors = true;
  cfg.dead_alpha_lib_dir = (fs::path(dir) / "does_not_exist").string();
  cfg.books_out = (fs::path(dir) / "books_missing.bin").string();
  auto r_missing = atx::impl::run_metabook(cfg, scfg);
  ASSERT_TRUE(r_missing.has_value()) << r_missing.error().message(); // MUST NOT abort/crash

  cfg.dead_alpha_factors = false;
  cfg.dead_alpha_lib_dir = "";
  cfg.books_out = (fs::path(dir) / "books_off.bin").string();
  auto r_off = atx::impl::run_metabook(cfg, scfg);
  ASSERT_TRUE(r_off.has_value()) << r_off.error().message();

  EXPECT_EQ(r_missing->digest, r_off->digest)
      << "a --dead-alpha-lib-dir that does not exist on disk must fail OPEN on the metabook "
         "path, byte-identical to the gate-off run -- not abort";
}

} // namespace atxtest_stage_metabook_dead_alpha_wire
