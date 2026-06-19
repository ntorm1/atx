// atx::engine::factory — Factory::mine_into integration (S4b-3, suite
// FactoryMineInto). The factory->library admit bridge.
//
// mine_into is the REAL admit path: it mines the search space exactly as mine()
// does (same seeded SearchDriver), but admits each deflated survivor into the
// PERSISTENT library::Library (library-wide dedup -> O(neighbors) corr -> P4 gate
// floors -> segmented store + PIT lifecycle + manifest) instead of an ephemeral
// combine::AlphaStore. The deflation bar stays FACTORY-side (dsr >= cfg.min_dsr)
// BEFORE library::admit is even consulted, so noise that the library alone might
// pass is still rejected.
//
// These tests mirror factory_integration_test.cpp's Panel/sim/policy/gate
// fixtures (the noise vs real-signal momentum panels + the seed grammar) so the
// admit semantics are validated against the SAME planted-edge / pure-noise
// discrimination the S3 suite proves for mine().

#include <cstdint>
#include <filesystem>   // per-test temp directory (the library is rooted at a dir)
#include <string>
#include <system_error> // std::error_code (tmpdir's remove_all/create_directories)
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"   // alpha::parse_expr (round-trip re-parse)
#include "atx/engine/alpha/registry.hpp" // alpha::Library (its definition site)
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/combine/gate.hpp" // combine::AlphaGate, GateConfig

#include "atx/engine/factory/canonical.hpp" // factory::canonical_hash (round-trip key)
#include "atx/engine/factory/factory.hpp"   // factory::Factory, FactoryConfig, FactoryReport
#include "atx/engine/factory/genome.hpp"    // factory::Genome (round-trip re-parse)

#include "atx/engine/library/library.hpp" // library::Library, AlphaCandidate, AdmitKind
#include "atx/engine/library/record.hpp"  // library::Provenance

namespace atxtest_factory_mine_into_test {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
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
using atx::engine::factory::canonical_hash;
using atx::engine::factory::Factory;
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::FactoryReport;
using atx::engine::factory::Genome;

namespace lib = atx::engine::library;

// ---- builders (mirrored from factory_integration_test.cpp) ------------------

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

// The shared P4 gate config (the BRAIN gold-standard floors) — same as the S3 suite.
[[nodiscard]] GateConfig default_gate_cfg() { return GateConfig{}; }

// The shared min_dsr deflation bar (S1, F4). The same bar gates both panels.
constexpr f64 kMinDsr = 0.5;

[[nodiscard]] FactoryConfig base_cfg(atx::u64 seed, usize pop, usize gens) {
  FactoryConfig cfg;
  cfg.search.master_seed = seed;
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

[[nodiscard]] FactoryConfig real_signal_cfg(atx::u64 seed) { return base_cfg(seed, 16, 4); }

// A LARGE-budget config: many trials -> a high multiple-testing N -> the deflation
// drives even the in-sample-best NOISE candidate's dsr below the bar (F4).
[[nodiscard]] FactoryConfig large_budget_cfg(atx::u64 seed) {
  FactoryConfig cfg = base_cfg(seed, 24, 6);
  cfg.search.fitness.trial_count = 64;
  return cfg;
}

// A per-test temp directory the library is rooted at (mirrors the S4 tmpdir helper).
[[nodiscard]] std::string tmpdir(const std::string &tag = "") {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S4b") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s4b_mine" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

// A reusable fixture bundle (one Library/Panel/policy/sim). The DSL Library here is
// the run-wide alpha::Library the genomes' op pointers alias (NOT the persistent
// library::Library, which mine_into admits into).
struct Fixture {
  Library lib{};
  Panel panel;
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  explicit Fixture(Panel p) : panel{std::move(p)} {}

  [[nodiscard]] Factory factory() { return Factory{lib, panel, sim, policy}; }
};

// The canonical hash the round-trip test re-derives from an admitted alpha's
// stored expr_source (parse_expr against a vanilla DSL library + analyze).
[[nodiscard]] u64 reparse_canonical_hash(const std::string &expr_source) {
  Library dsl;
  auto parsed = parse_expr(expr_source, dsl);
  EXPECT_TRUE(parsed.has_value())
      << "stored expr_source must re-parse: " << expr_source
      << (parsed ? "" : (" : " + parsed.error().message()));
  if (!parsed) {
    return 0;
  }
  auto info = analyze(*parsed);
  EXPECT_TRUE(info.has_value()) << (info ? "" : info.error().message());
  if (!info) {
    return 0;
  }
  Genome g{std::move(*parsed), std::move(*info), 0};
  return canonical_hash(g.ast, g.ast.roots().front().root);
}

// =============================================================================
//  MinesAndAdmitsIntoPersistentLibrary — the bridge admits survivors into the
//  PERSISTENT library (not an ephemeral AlphaStore), and the report's library
//  accounting reconciles with library::n_alphas().
// =============================================================================
TEST(FactoryMineInto, MinesAndAdmitsIntoPersistentLibrary) {
  Fixture fx{real_signal_panel()};
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {0xC0FFEEu});
  AlphaGate gate{default_gate_cfg()};
  Factory f = fx.factory();

  const FactoryReport rep = f.mine_into(real_signal_cfg(/*seed*/ 1), library, gate);

  EXPECT_GT(rep.admitted, 0u);                            // admits survivors on a real-signal panel
  EXPECT_EQ(library.n_alphas(), rep.library_n_alphas_after);
  EXPECT_EQ(rep.library_n_alphas_after - rep.library_n_alphas_before,
            static_cast<u64>(rep.admitted)); // every admit grew the library by one
  EXPECT_EQ(rep.library_n_alphas_before, 0u); // a fresh temp-dir starts empty
}

// =============================================================================
//  EveryAdmittedAlphaHasRoundTrippableFormula — S4b-1 round-trip: each admitted
//  alpha's stored provenance expr_source re-parses to the SAME canonical_hash as
//  the stored canon_hash key (the F6 dedup key the library deduped on).
// =============================================================================
TEST(FactoryMineInto, EveryAdmittedAlphaHasRoundTrippableFormula) {
  Fixture fx{real_signal_panel()};
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {0xC0FFEEu});
  AlphaGate gate{default_gate_cfg()};
  Factory f = fx.factory();

  const FactoryReport rep = f.mine_into(real_signal_cfg(/*seed*/ 3), library, gate);
  ASSERT_GT(rep.admitted, 0u) << "need at least one admitted alpha to round-trip";

  const u64 n = library.n_alphas();
  ASSERT_EQ(n, static_cast<u64>(rep.admitted));
  for (u64 a = 0; a < n; ++a) {
    const auto rec = library.get(lib::AlphaId{static_cast<atx::u32>(a)});
    const lib::Provenance &prov = rec.provenance;
    EXPECT_FALSE(prov.expr_source.empty()) << "admitted alpha " << a << " has no expr_source";
    EXPECT_EQ(reparse_canonical_hash(prov.expr_source), rec.canon_hash)
        << "alpha " << a << " expr_source '" << prov.expr_source
        << "' does not round-trip to its stored canon_hash";
  }
}

// =============================================================================
//  DeflationBarStaysFactorySide — F4: a pure-noise panel admits NOTHING. The dsr
//  bar rejects the entire noise population BEFORE library::admit is consulted, so
//  even if library::admit alone would pass a candidate the deflation kills it.
// =============================================================================
TEST(FactoryMineInto, DeflationBarStaysFactorySide) {
  Fixture fx{pure_noise_panel()};
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {0xC0FFEEu});
  AlphaGate gate{default_gate_cfg()};
  Factory f = fx.factory();

  const FactoryReport rep = f.mine_into(large_budget_cfg(/*seed*/ 2), library, gate);

  EXPECT_EQ(rep.admitted, 0u);      // deflation + gates kill the entire noise population
  EXPECT_EQ(library.n_alphas(), 0u); // nothing inserted into the persistent library
  EXPECT_EQ(rep.library_n_alphas_after, 0u);
}

// =============================================================================
//  SeededRunFoldsAdmissionsIntoDigest — F1/F2: two mine_into runs with the same
//  seed into fresh temp-dirs replay to a byte-identical digest (the search digest
//  FOLDED with every admission decision).
// =============================================================================
TEST(FactoryMineInto, SeededRunFoldsAdmissionsIntoDigest) {
  Fixture fx1{real_signal_panel()};
  Fixture fx2{real_signal_panel()};
  AlphaGate gate{default_gate_cfg()};
  lib::Library lib1 = lib::Library::open(tmpdir("a"), default_gate_cfg(), {0xC0FFEEu});
  lib::Library lib2 = lib::Library::open(tmpdir("b"), default_gate_cfg(), {0xC0FFEEu});
  Factory f1 = fx1.factory();
  Factory f2 = fx2.factory();

  const FactoryReport a = f1.mine_into(real_signal_cfg(/*seed*/ 5), lib1, gate);
  const FactoryReport b = f2.mine_into(real_signal_cfg(/*seed*/ 5), lib2, gate);

  EXPECT_EQ(a.digest, b.digest);
  EXPECT_EQ(a.admitted, b.admitted); // identical mine+admit -> identical outcome
}


}  // namespace atxtest_factory_mine_into_test
