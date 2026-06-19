// atx::engine::factory — ResearchDriver integration (S4b-4, suite ResearchDriver).
// The continuous mine->admit->repeat engine over a FIXED research panel.
//
// ResearchDriver owns the persistent library::Library and drives a budget-bounded
// loop of Factory::mine_into runs (each a complete S4b-3 mine -> deflate ->
// library::admit) into that ONE library, growing the deduplicated persistent
// library across runs until either max_runs is hit or `patience` consecutive
// zero-admit runs signal novelty exhaustion. This is the across-run orchestration
// layer (it sits above Factory exactly as Factory sits above SearchDriver).
//
// These tests mirror factory_mine_into_test.cpp's Panel/sim/policy/gate fixtures
// (the real-signal momentum vs pure-noise panels + the seed grammar + the temp-dir
// library setup) so the engine's growth / patience / replay semantics are validated
// against the SAME planted-edge / pure-noise discrimination the S3/S4b suites prove.

#include <cstdint>
#include <filesystem>   // per-test temp directory (the library is rooted at a dir)
#include <string>
#include <system_error> // std::error_code (tmpdir's remove_all/create_directories)
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp" // alpha::Library (its definition site)
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/combine/gate.hpp" // combine::AlphaGate, GateConfig

#include "atx/engine/factory/factory.hpp" // factory::FactoryConfig (the per-run inner config)
#include "atx/engine/factory/research_driver.hpp" // factory::ResearchDriver, ResearchConfig, ResearchReport

#include "atx/engine/library/library.hpp"  // library::Library, rebuild_equals
#include "atx/engine/library/manifest.hpp" // library::LibraryManifest

namespace atxtest_factory_research_driver_test {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::GateConfig;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::ResearchConfig;
using atx::engine::factory::ResearchDriver;
using atx::engine::factory::ResearchReport;

namespace lib = atx::engine::library;

// ---- builders (mirrored from factory_mine_into_test.cpp) --------------------

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

[[nodiscard]] Panel make_panel(usize dates, usize insts, std::vector<std::string> fields,
                               std::vector<std::vector<f64>> cols) {
  auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), {});
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

// A tiny deterministic LCG (no RNG dep) -> uniform(-1, 1), the S3-4/S3-5 idiom.
struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

// REAL-SIGNAL close matrix: a persistent per-instrument drift => rank/ts_mean over
// close carry a genuine momentum edge the seed grammar captures (S3 fixture idiom).
[[nodiscard]] std::vector<f64> momentum_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.010 - 0.0040 * static_cast<f64>(j);
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.008 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

// PURE-NOISE close matrix: i.i.d. multiplicative noise, ZERO drift => no edge.
[[nodiscard]] std::vector<f64> noise_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + 0.008 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

// A two-field (close + 1-day reversal) panel over the supplied close matrix.
[[nodiscard]] Panel two_field_panel(usize dates, usize insts, std::vector<f64> close) {
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  return make_panel(dates, insts, {"close", "rev"}, {close, rev});
}

[[nodiscard]] Panel real_signal_panel() {
  return two_field_panel(120, 8, momentum_close(120, 8, 0xA11Cu));
}

[[nodiscard]] Panel pure_noise_panel() {
  return two_field_panel(120, 8, noise_close(120, 8, 0xBADC0FFEEu));
}

[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)", "rank(rev)",         "ts_mean(close, 5)", "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))", "delta(close, 2)"};
}

[[nodiscard]] std::vector<std::string> panel_fields() { return {"close", "rev"}; }

[[nodiscard]] GateConfig default_gate_cfg() { return GateConfig{}; }

constexpr f64 kMinDsr = 0.5;

// The persistent library's master seed (the corr-index rebuild seed, persisted into
// the manifest). FIXED so a reopen rebuilds the index identically (L7).
constexpr u64 kLibSeed = 0xC0FFEEu;

// The per-run inner mine config (mirrors factory_mine_into_test.cpp::base_cfg).
[[nodiscard]] FactoryConfig per_run_cfg(usize pop, usize gens) {
  FactoryConfig cfg;
  cfg.search.master_seed = 0; // OVERWRITTEN per run by detail::seed_for_run
  cfg.search.population = pop;
  cfg.search.generations = gens;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.search.enable_behavioral_novelty = true;
  cfg.search.fitness.trial_count = 4;
  cfg.seed_exprs = seed_exprs();
  cfg.panel_fields = panel_fields();
  cfg.min_dsr = kMinDsr;
  return cfg;
}

// A real-signal research config: a small per-run budget, several runs.
[[nodiscard]] ResearchConfig real_signal_research_cfg(u64 master_seed, usize max_runs,
                                                      usize patience) {
  ResearchConfig cfg;
  cfg.per_run = per_run_cfg(16, 4);
  cfg.max_runs = max_runs;
  cfg.patience = patience;
  cfg.master_seed = master_seed;
  return cfg;
}

// A LARGE-budget per-run config over pure noise: the deflation N drives even the
// in-sample-best noise candidate below the dsr bar, so EVERY run admits 0 -> the
// patience stop fires before max_runs.
[[nodiscard]] ResearchConfig noise_research_cfg(u64 master_seed, usize max_runs, usize patience) {
  ResearchConfig cfg;
  cfg.per_run = per_run_cfg(24, 6);
  cfg.per_run.search.fitness.trial_count = 64;
  cfg.max_runs = max_runs;
  cfg.patience = patience;
  cfg.master_seed = master_seed;
  return cfg;
}

// A per-test temp directory the library is rooted at (mirrors the S4b mine_into helper).
[[nodiscard]] std::string tmpdir(const std::string &tag = "") {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S4b") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_s4b_research" / base;
  // Wipe + recreate the per-test dir. The unique (suite_name + test_name + tag) path
  // already prevents cross-test collisions, so a pre-existing artifact (a prior run's
  // leftovers) is tolerated: the remove_all/create_directories failures are non-fatal
  // (ec is intentionally inspected-then-discarded — the open below would surface any
  // genuine unwritable-dir fault).
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  (void)ec;
  std::filesystem::create_directories(dir, ec);
  (void)ec;
  return dir.string();
}

// A reusable fixture bundle (one DSL Library / Panel / policy / sim). The DSL Library
// here is the run-wide alpha::Library the genomes' op pointers alias (NOT the
// persistent library::Library, which the engine admits into).
struct Fixture {
  Library dsl{};
  Panel panel;
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  explicit Fixture(Panel p) : panel{std::move(p)} {}
};

// =============================================================================
//  GrowsLibraryAcrossRuns — the engine drives multiple mine->admit runs into ONE
//  persistent library over a real-signal panel and grows the deduplicated library.
//  The report's accounting reconciles with library::n_alphas() / the manifest.
// =============================================================================
TEST(ResearchDriver, GrowsLibraryAcrossRuns) {
  Fixture fx{real_signal_panel()};
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {kLibSeed});
  AlphaGate gate{default_gate_cfg()};
  ResearchDriver engine{library, fx.dsl, fx.panel, fx.sim, fx.policy, gate};

  const ResearchReport rep = engine.run(real_signal_research_cfg(/*seed*/ 11, /*max_runs*/ 3,
                                                                 /*patience*/ 0));

  EXPECT_GT(rep.total_admitted, 0u);       // a real-signal panel admits survivors
  EXPECT_GE(rep.runs, 1u);                 // at least one run executed
  EXPECT_LE(rep.runs, 3u);                 // never exceeds max_runs
  EXPECT_EQ(rep.library_size, library.n_alphas()); // report reconciles with the library
  EXPECT_EQ(rep.seed, 11u);                // the engine seed is surfaced
  // The histogram is correctly populated FROM THE MANIFEST: every alpha is bucketed
  // exactly once, so the bucket counts sum to the library size. This tests S4b-4's own
  // report-building wiring (manifest entries -> histogram) without re-asserting S4-4's
  // admit->Admitted lifecycle contract (which bucket each alpha lands in is library law).
  usize hist_sum = 0;
  for (const usize c : rep.lifecycle_histogram) {
    hist_sum += c;
  }
  EXPECT_EQ(hist_sum, rep.library_size);
}

// =============================================================================
//  StopsOnPatienceWhenNoveltyExhausted — over pure noise the deflation bar rejects
//  every candidate, so every run admits 0; the patience counter trips and the
//  engine STOPS strictly before max_runs (non-vacuous: runs < max_runs is asserted).
// =============================================================================
TEST(ResearchDriver, StopsOnPatienceWhenNoveltyExhausted) {
  Fixture fx{pure_noise_panel()};
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {kLibSeed});
  AlphaGate gate{default_gate_cfg()};
  ResearchDriver engine{library, fx.dsl, fx.panel, fx.sim, fx.policy, gate};

  constexpr usize kPatience = 2;
  constexpr usize kMaxRuns = 10;
  const ResearchReport rep =
      engine.run(noise_research_cfg(/*seed*/ 7, kMaxRuns, kPatience));

  EXPECT_EQ(rep.total_admitted, 0u);       // noise admits nothing
  EXPECT_LT(rep.runs, kMaxRuns);           // EARLY STOP fired (non-vacuous)
  EXPECT_EQ(rep.runs, kPatience);          // stopped exactly after `patience` dry runs
  EXPECT_EQ(library.n_alphas(), 0u);       // nothing inserted
  EXPECT_EQ(rep.library_size, 0u);
}

// =============================================================================
//  CheckpointResumeReplaysIdentically — F1: the persistent library reopened from
//  the SAME dir (same cfg + seeds) recomputes a byte-identical manifest (version_id
//  + every segment_crc). rep.manifest_version_id == the snapshot's version_id.
// =============================================================================
TEST(ResearchDriver, CheckpointResumeReplaysIdentically) {
  const std::string dir = tmpdir();
  Fixture fx{real_signal_panel()};
  AlphaGate gate{default_gate_cfg()};
  const GateConfig gate_cfg = default_gate_cfg();
  const std::vector<u64> seeds{kLibSeed};

  lib::LibraryManifest snap;
  {
    lib::Library library = lib::Library::open(dir, gate_cfg, seeds);
    ResearchDriver engine{library, fx.dsl, fx.panel, fx.sim, fx.policy, gate};
    const ResearchReport rep =
        engine.run(real_signal_research_cfg(/*seed*/ 13, /*max_runs*/ 2, /*patience*/ 0));
    snap = library.snapshot();
    EXPECT_EQ(rep.manifest_version_id, snap.version_id); // the report carries the address
    EXPECT_GT(rep.total_admitted, 0u);                   // need content to make the replay non-trivial
  } // library closed (segments sealed on disk)

  // Reopen the SAME dir: the recomputed snapshot must be byte-identical (L6/L7).
  EXPECT_TRUE(lib::rebuild_equals(snap, dir, gate_cfg, seeds));
}

// =============================================================================
//  SeededEngineDigestIsDeterministic — F1: two ResearchDriver runs with the SAME
//  ResearchConfig into FRESH temp-dirs replay to an equal engine digest AND an equal
//  manifest version_id (the per-run FactoryReport digests folded in run order).
// =============================================================================
TEST(ResearchDriver, SeededEngineDigestIsDeterministic) {
  Fixture fx1{real_signal_panel()};
  Fixture fx2{real_signal_panel()};
  AlphaGate gate{default_gate_cfg()};
  lib::Library lib1 = lib::Library::open(tmpdir("a"), default_gate_cfg(), {kLibSeed});
  lib::Library lib2 = lib::Library::open(tmpdir("b"), default_gate_cfg(), {kLibSeed});
  ResearchDriver e1{lib1, fx1.dsl, fx1.panel, fx1.sim, fx1.policy, gate};
  ResearchDriver e2{lib2, fx2.dsl, fx2.panel, fx2.sim, fx2.policy, gate};

  const ResearchReport a = e1.run(real_signal_research_cfg(/*seed*/ 21, /*max_runs*/ 3,
                                                          /*patience*/ 0));
  const ResearchReport b = e2.run(real_signal_research_cfg(/*seed*/ 21, /*max_runs*/ 3,
                                                          /*patience*/ 0));

  EXPECT_EQ(a.digest, b.digest);                       // byte-identical engine fingerprint
  EXPECT_EQ(a.manifest_version_id, b.manifest_version_id); // identical content-address
  EXPECT_EQ(a.total_admitted, b.total_admitted);       // identical mine+admit across runs
  EXPECT_EQ(a.runs, b.runs);
}


}  // namespace atxtest_factory_research_driver_test
