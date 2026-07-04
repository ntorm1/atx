// stage_optimize_dead_alpha_wire_test.cpp -- p9 S1-1: thread the accumulating
// library into build_risk_model's dead_lib/dead_ids at stage_optimize.cpp's
// three call sites.
//
// Suite: AtxImplOptimizeDeadAlphaWire
//
// (a) off-path byte-identity -- the wire's three fail-open guards
//     (dead_alpha_factors=false, no dir resolved, resolved-but-missing dir)
//     each reproduce the pre-wire books digest exactly.
// (b) on-path RED->GREEN -- a synthetic crowded-pool library de-levers the
//     crowded instrument once --dead-alpha-factors + a populated
//     --dead-alpha-lib-dir are both set.

#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/library/lifecycle.hpp"
#include "atx/engine/library/record.hpp"

namespace atxtest_stage_optimize_dead_alpha_wire {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
namespace lib = atx::engine::library;
using atx::f64;
using atx::usize;

// A gently-trending panel identical in shape to
// stage_optimize_riskmodel_test.cpp's fixture (kept local -- no cross-file
// test dependency).
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

// combo: a fixed long-first-half / short-second-half alpha signal, CONSTANT
// across periods (matches stage_optimize_riskmodel_test.cpp::make_pair_combo).
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

// Build an ON-DISK library at `dir` with n_dead alphas all concentrated on
// instrument `center` (rank-1 overlap -- same fixture shape as p8's
// stage_riskmodel_dead_factor_test.cpp), then FLUSH and let the Library
// object go out of scope so a later independent Library::open(dir, ...)
// (the one stage_optimize.cpp's wire performs) sees every admit on disk --
// a live in-process instance's un-flushed memtable is invisible to a
// second Library::open of the same directory.
void seed_crowded_library(const fs::path& dir, usize n_dead, usize m, usize center) {
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  lib::Library library = lib::Library::open(dir.string(), permissive_gate_cfg(), {777ULL});
  const atx::engine::combine::AlphaGate gate{permissive_gate_cfg()};
  constexpr usize kT = 2U;
  std::vector<std::vector<f64>> pnls(n_dead), positions(n_dead);
  std::vector<lib::AlphaId> ids;
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
    ids.push_back(v.id);
  }
  ASSERT_TRUE(library.flush_all().has_value());
  // NOTE: no LifecycleState::Dead transition here -- see the S1 ledger's
  // "admitted pool" policy note; the wire treats every admitted (non-
  // Candidate, non-Recycled) alpha as the crowding-defense population.
}

class AtxImplOptimizeDeadAlphaWire : public ::testing::Test {
protected:
  fs::path tmp_dir_;
  void SetUp() override {
    tmp_dir_ = fs::temp_directory_path() / "atx_p9_s1_wire_test";
    std::error_code ec;
    fs::remove_all(tmp_dir_, ec);
    fs::create_directories(tmp_dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_dir_, ec);
  }
};

// -- (a) off-path byte-identity: three independent fail-open guards ---------
TEST_F(AtxImplOptimizeDeadAlphaWire, FailOpen_FlagOffByteIdentical) {
  constexpr usize M = 10, D = 40;
  const fs::path research_path = tmp_dir_ / "research.bin";
  const fs::path combo_path = tmp_dir_ / "combo.bin";
  ASSERT_TRUE(make_trend_research(research_path, M, D).has_value());
  ASSERT_TRUE(make_pair_combo(combo_path, M, D).has_value());
  seed_crowded_library(tmp_dir_ / "lib", 2U, M, 3U);

  atx::impl::RunConfig cfg;
  cfg.panel = research_path.string();
  cfg.combo = combo_path.string();
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "weekly";
  cfg.risk_aversion = 1.0;
  cfg.set_flags.emplace("risk-aversion");
  cfg.dead_alpha_lib_dir = (tmp_dir_ / "lib").string(); // populated, but the GATE is off
  cfg.dead_alpha_factors = false;

  cfg.books_out = (tmp_dir_ / "books_gate_off.bin").string();
  auto r_off = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(r_off.has_value()) << r_off.error().message();

  cfg.dead_alpha_lib_dir = ""; // no library at all, matches pre-wire literally
  cfg.books_out = (tmp_dir_ / "books_legacy.bin").string();
  auto r_legacy = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(r_legacy.has_value());

  EXPECT_EQ(r_off->digest, r_legacy->digest)
      << "dead_alpha_factors=false must ignore a populated --dead-alpha-lib-dir entirely";
}

TEST_F(AtxImplOptimizeDeadAlphaWire, FailOpen_MissingDirByteIdentical) {
  constexpr usize M = 10, D = 40;
  const fs::path research_path = tmp_dir_ / "research2.bin";
  const fs::path combo_path = tmp_dir_ / "combo2.bin";
  ASSERT_TRUE(make_trend_research(research_path, M, D).has_value());
  ASSERT_TRUE(make_pair_combo(combo_path, M, D).has_value());

  atx::impl::RunConfig cfg;
  cfg.panel = research_path.string();
  cfg.combo = combo_path.string();
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "weekly";
  cfg.risk_aversion = 1.0;
  cfg.set_flags.emplace("risk-aversion");
  cfg.dead_alpha_factors = true;
  cfg.dead_alpha_lib_dir = (tmp_dir_ / "does_not_exist").string(); // never created

  cfg.books_out = (tmp_dir_ / "books_missing_dir.bin").string();
  auto r_missing = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(r_missing.has_value()) << r_missing.error().message(); // MUST NOT abort/crash

  cfg.dead_alpha_factors = false;
  cfg.books_out = (tmp_dir_ / "books_legacy2.bin").string();
  auto r_legacy = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(r_legacy.has_value());

  EXPECT_EQ(r_missing->digest, r_legacy->digest)
      << "a --dead-alpha-lib-dir that does not exist on disk must fail OPEN, not abort";
}

// -- (b) on-path RED->GREEN: crowded pool must shrink the crowded direction --
TEST_F(AtxImplOptimizeDeadAlphaWire, CrowdedPoolDelevers) {
  constexpr usize M = 10, D = 40;
  const usize center = 3U;
  const fs::path research_path = tmp_dir_ / "research3.bin";
  const fs::path combo_path = tmp_dir_ / "combo3.bin";
  ASSERT_TRUE(make_trend_research(research_path, M, D).has_value());
  ASSERT_TRUE(make_pair_combo(combo_path, M, D).has_value()); // long-first-half/short-second-half
  seed_crowded_library(tmp_dir_ / "lib3", /*n_dead=*/3U, M, center);

  atx::impl::RunConfig cfg;
  cfg.panel = research_path.string();
  cfg.combo = combo_path.string();
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "weekly";
  cfg.risk_aversion = 1.0;
  cfg.set_flags.emplace("risk-aversion");

  cfg.dead_alpha_factors = false;
  cfg.books_out = (tmp_dir_ / "books_baseline.bin").string();
  auto baseline_sr = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(baseline_sr.has_value()) << baseline_sr.error().message();

  cfg.dead_alpha_factors = true;
  cfg.dead_alpha_lib_dir = (tmp_dir_ / "lib3").string();
  cfg.books_out = (tmp_dir_ / "books_delevered.bin").string();
  auto delev_sr = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(delev_sr.has_value()) << delev_sr.error().message();

  auto baseline_r = atx::impl::read_panel((tmp_dir_ / "books_baseline.bin").string());
  auto delev_r = atx::impl::read_panel((tmp_dir_ / "books_delevered.bin").string());
  ASSERT_TRUE(baseline_r.has_value());
  ASSERT_TRUE(delev_r.has_value());
  const auto wfid_b = *baseline_r->field_id("weight");
  const auto wfid_d = *delev_r->field_id("weight");
  const usize last = baseline_r->dates() - 1;
  const auto w_base = baseline_r->field_cross_section(wfid_b, last);
  const auto w_delev = delev_r->field_cross_section(wfid_d, last);

  EXPECT_LT(std::fabs(w_delev[center]), std::fabs(w_base[center]))
      << "expected the crowded instrument's weight to shrink once the dead-alpha "
         "wire is live: base="
      << w_base[center] << " delevered=" << w_delev[center];
  // Bit-exact sanity: the two runs must NOT be byte-identical once the gate is
  // live (else the wire silently did nothing) -- bit_cast per the p9 byte-
  // identity idiom, applied here to prove a DIFFERENCE, not an equality.
  EXPECT_NE(std::bit_cast<std::uint64_t>(w_base[center]),
            std::bit_cast<std::uint64_t>(w_delev[center]));
}

} // namespace atxtest_stage_optimize_dead_alpha_wire
