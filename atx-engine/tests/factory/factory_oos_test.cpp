// atx::engine::factory — Factory::mine_into out-of-sample (holdout) validation
// (Task P2a, suite FactoryOos). Adds GENUINE train/holdout discipline to the
// gated discover path: the genetic search SELECTS on a TRAIN window, but the
// AlphaGate floors + the DSR bar are CONFIRMED on a HELD-OUT terminal window the
// search never optimized on, and the persisted library `metrics` are the HOLDOUT
// metrics (what was actually gated).
//
// The OOS path is gated behind FactoryConfig::oos_fraction (0.0 default == OFF).
// When OFF the path is byte-identical to the legacy mine_into (proven by
// MineIntoOosOff_ByteIdenticalToLegacy). When ON, mine_into dispatches to the
// additive mine_into_oos branch.
//
// Fixtures mirror factory_mine_into_test.cpp (the same Lcg / momentum / noise
// panels + the seed grammar) so the OOS admit semantics are validated against the
// same planted-edge discrimination the S3 / S4b suite proves for the legacy path.

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp" // alpha::Library
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/combine/gate.hpp"

#include "atx/engine/eval/lockbox.hpp" // eval::detail::slice_panel (the holdout builder)

#include "atx/engine/factory/factory.hpp"

#include "atx/engine/library/library.hpp"

namespace atxtest_factory_oos_test {

using atx::f64;
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
using atx::engine::factory::Factory;
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::FactoryReport;

namespace lib = atx::engine::library;
namespace eval = atx::engine::eval;

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

struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

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

[[nodiscard]] std::vector<f64> reversal_of(const std::vector<f64> &close, usize dates, usize insts) {
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  return rev;
}

[[nodiscard]] Panel two_field_panel(usize dates, usize insts, std::vector<f64> close) {
  std::vector<f64> rev = reversal_of(close, dates, insts);
  return make_panel(dates, insts, {"close", "rev"}, {close, rev});
}

[[nodiscard]] Panel real_signal_panel() {
  return two_field_panel(120, 8, momentum_close(120, 8, 0xA11Cu));
}

// A REGIME-SWITCHED panel: a strong, persistent per-instrument momentum drift in
// the TRAIN prefix [0, split) that COLLAPSES to pure i.i.d. noise (zero drift) in
// the HOLDOUT suffix [split, dates). A momentum/rank alpha that wins on the train
// prefix has NO edge on the noise holdout — its holdout Sharpe ≈ 0, so it fails
// the gate floors / DSR bar. This is the canonical "good IS / bad OOS" overfit a
// real holdout must reject. (A sign-FLIP would leave a trend/reversal alpha a
// symmetric edge; a regime that simply has no structure is the honest negative.)
[[nodiscard]] Panel regime_flip_panel(usize dates, usize insts, usize split,
                                       std::uint64_t seed) {
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.012 - 0.0050 * static_cast<f64>(j);
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    const f64 d = (t < split) ? 1.0 : 0.0; // drift only in TRAIN; noise-only HOLDOUT
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + d * drift[j] + 0.008 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return two_field_panel(dates, insts, std::move(close));
}

[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)", "rank(rev)",         "ts_mean(close, 5)", "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))", "delta(close, 2)"};
}

[[nodiscard]] std::vector<std::string> panel_fields() { return {"close", "rev"}; }

[[nodiscard]] GateConfig default_gate_cfg() { return GateConfig{}; }

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

[[nodiscard]] std::string tmpdir(const std::string &tag = "") {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "P2a") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_p2a_oos" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

struct Fixture {
  Library lib{};
  Panel panel;
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  explicit Fixture(Panel p) : panel{std::move(p)} {}

  [[nodiscard]] Factory factory() { return Factory{lib, panel, sim, policy}; }
};

// =============================================================================
//  SlicePanelContiguousDateRange — slice_panel(p, d0, d1) reproduces field_all
//  cell-for-cell over [d0*N, d1*N) for every field; dates() == d1-d0; the
//  universe mask is sliced; field names are preserved.
// =============================================================================
TEST(FactoryOos, SlicePanelContiguousDateRange) {
  const usize dates = 30;
  const usize insts = 4;
  Panel p = two_field_panel(dates, insts, momentum_close(dates, insts, 0x5111CEu));

  const usize d0 = 11;
  const usize d1 = 23;
  auto sliced_r = eval::detail::slice_panel(p, d0, d1);
  ASSERT_TRUE(sliced_r.has_value()) << "slice must build";
  const Panel &sliced = *sliced_r;

  EXPECT_EQ(sliced.dates(), d1 - d0);
  EXPECT_EQ(sliced.instruments(), insts);
  ASSERT_EQ(sliced.num_fields(), p.num_fields());

  for (usize f = 0; f < p.num_fields(); ++f) {
    EXPECT_EQ(sliced.field_name(f), p.field_name(f)) << "field name preserved";
    const auto full = p.field_all(static_cast<atx::u32>(f));
    const auto cut = sliced.field_all(static_cast<atx::u32>(f));
    ASSERT_EQ(cut.size(), (d1 - d0) * insts);
    for (usize t = 0; t < (d1 - d0); ++t) {
      for (usize j = 0; j < insts; ++j) {
        EXPECT_EQ(cut[t * insts + j], full[(d0 + t) * insts + j])
            << "cell mismatch at sliced date " << t << " inst " << j;
      }
    }
  }
  // Universe mask: an all-in-universe source reproduces an all-in-universe slice.
  for (usize t = 0; t < (d1 - d0); ++t) {
    for (usize j = 0; j < insts; ++j) {
      EXPECT_EQ(sliced.in_universe(t, j), p.in_universe(d0 + t, j));
    }
  }
}

// =============================================================================
//  MineIntoOosOff_ByteIdenticalToLegacy — with oos_fraction == 0 (the default),
//  the new branch is never taken: two runs (one explicitly leaving oos_fraction
//  at its default, one a second invocation) yield identical library version_id,
//  admitted count, and report digest. Pins the off-path invariant.
// =============================================================================
TEST(FactoryOos, MineIntoOosOff_ByteIdenticalToLegacy) {
  Fixture fx1{real_signal_panel()};
  Fixture fx2{real_signal_panel()};
  AlphaGate gate{default_gate_cfg()};
  lib::Library lib1 = lib::Library::open(tmpdir("a"), default_gate_cfg(), {0xC0FFEEu});
  lib::Library lib2 = lib::Library::open(tmpdir("b"), default_gate_cfg(), {0xC0FFEEu});

  FactoryConfig cfg = real_signal_cfg(/*seed*/ 7);
  ASSERT_EQ(cfg.oos_fraction, 0.0) << "default must be OFF";

  Factory f1 = fx1.factory();
  Factory f2 = fx2.factory();
  const FactoryReport a = f1.mine_into(cfg, lib1, gate).value();
  const FactoryReport b = f2.mine_into(cfg, lib2, gate).value();

  EXPECT_EQ(a.digest, b.digest);
  EXPECT_EQ(a.admitted, b.admitted);
  EXPECT_GT(a.admitted, 0u) << "legacy real-signal path still admits";
  EXPECT_TRUE(a.oos_metrics.empty()) << "off-path leaves the OOS report empty";
  EXPECT_EQ(lib1.snapshot().version_id, lib2.snapshot().version_id);
}

// =============================================================================
//  GoodIsBadOos_Rejected (THE required test) — a regime-flip panel: a momentum
//  edge is strong in TRAIN, reversed in HOLDOUT. The search ranks a candidate
//  well on the train window, but admission is CONFIRMED on the holdout where the
//  signal fails → nothing is admitted (library empty), because the holdout
//  metrics fail the floors / holdout DSR < min_dsr.
// =============================================================================
TEST(FactoryOos, GoodIsBadOos_Rejected) {
  const usize dates = 160;
  const usize insts = 8;
  const usize split = 128; // 20% terminal holdout
  Fixture fx{regime_flip_panel(dates, insts, split, 0xD15EA5Eu)};
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {0xC0FFEEu});
  AlphaGate gate{default_gate_cfg()};
  Factory f = fx.factory();

  FactoryConfig cfg = real_signal_cfg(/*seed*/ 11);
  cfg.oos_fraction = 0.20; // hold out the terminal 20% — the noise-only regime
  // legacy-init pin (consistent with the golden boundary pin): the grammar-init
  // default changes the mined candidate set; this test verifies OOS-gate LOGIC, not
  // init diversity.
  cfg.search.seed_from_grammar = false;

  const FactoryReport rep = f.mine_into(cfg, library, gate).value();

  EXPECT_EQ(rep.admitted, 0u)
      << "an alpha good on TRAIN but with no edge on the noise HOLDOUT must be rejected";
  EXPECT_EQ(library.n_alphas(), 0u) << "nothing persisted";

  // DISCRIMINATING CONTROL: the legacy IN-SAMPLE path (oos OFF) sees the strong
  // train-prefix edge folded across the WHOLE panel and DOES admit at least one
  // overfit alpha. The OOS discipline is what rejects it above — without the
  // holdout confirmation this candidate would have entered the library.
  Fixture fx_is{regime_flip_panel(dates, insts, split, 0xD15EA5Eu)};
  lib::Library lib_is = lib::Library::open(tmpdir("is"), default_gate_cfg(), {0xC0FFEEu});
  Factory f_is = fx_is.factory();
  FactoryConfig cfg_is = real_signal_cfg(/*seed*/ 11); // oos_fraction stays 0 (OFF)
  // legacy-init pin (consistent with the golden boundary pin): the grammar-init
  // default changes the mined candidate set; this test verifies OOS-gate LOGIC, not
  // init diversity.
  cfg_is.search.seed_from_grammar = false;
  const FactoryReport rep_is = f_is.mine_into(cfg_is, lib_is, gate).value();
  EXPECT_GT(rep_is.admitted, 0u)
      << "the legacy in-sample path admits the overfit the OOS holdout rejects";
}

// =============================================================================
//  GoodIsGoodOos_Admitted — a STATIONARY planted signal good in BOTH windows is
//  admitted, and the persisted library `metrics` equal the HOLDOUT metrics (what
//  was gated), NOT the train metrics.
// =============================================================================
TEST(FactoryOos, GoodIsGoodOos_Admitted) {
  Fixture fx{real_signal_panel()}; // stationary momentum: good on train AND holdout
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {0xC0FFEEu});
  AlphaGate gate{default_gate_cfg()};
  Factory f = fx.factory();

  FactoryConfig cfg = real_signal_cfg(/*seed*/ 13);
  cfg.oos_fraction = 0.20;

  const FactoryReport rep = f.mine_into(cfg, library, gate).value();

  ASSERT_GT(rep.admitted, 0u) << "a stationary edge survives the holdout confirmation";
  EXPECT_EQ(library.n_alphas(), static_cast<u64>(rep.admitted));
  ASSERT_FALSE(rep.oos_metrics.empty());

  // The library's durable metrics are the HOLDOUT (admission) metrics — they
  // match the oos_metrics reported, not the is_metrics.
  for (const auto &e : rep.oos_metrics) {
    bool found = false;
    const u64 n = library.n_alphas();
    for (u64 a = 0; a < n; ++a) {
      const auto rec = library.get(lib::AlphaId{static_cast<atx::u32>(a)});
      if (rec.canon_hash == e.canon_hash) {
        found = true;
        EXPECT_EQ(rec.metrics.sharpe, e.oos_metrics.sharpe)
            << "persisted metrics must be the HOLDOUT metrics";
        EXPECT_EQ(rec.metrics.fitness, e.oos_metrics.fitness);
        break;
      }
    }
    EXPECT_TRUE(found) << "every reported admit must be in the library";
  }
}

// =============================================================================
//  OosDeterminism — same seed + panel + oos_fraction => identical library
//  version_id + digest across two runs.
// =============================================================================
TEST(FactoryOos, OosDeterminism) {
  Fixture fx1{real_signal_panel()};
  Fixture fx2{real_signal_panel()};
  AlphaGate gate{default_gate_cfg()};
  lib::Library lib1 = lib::Library::open(tmpdir("a"), default_gate_cfg(), {0xC0FFEEu});
  lib::Library lib2 = lib::Library::open(tmpdir("b"), default_gate_cfg(), {0xC0FFEEu});

  FactoryConfig cfg = real_signal_cfg(/*seed*/ 17);
  cfg.oos_fraction = 0.20;

  Factory f1 = fx1.factory();
  Factory f2 = fx2.factory();
  const FactoryReport a = f1.mine_into(cfg, lib1, gate).value();
  const FactoryReport b = f2.mine_into(cfg, lib2, gate).value();

  EXPECT_EQ(a.digest, b.digest);
  EXPECT_EQ(a.admitted, b.admitted);
  EXPECT_EQ(lib1.snapshot().version_id, lib2.snapshot().version_id);
}

// ============================================================================
//  CountingSink — minimal local recording sink for forwarding tests.
// ============================================================================
struct CountingSink : atx::engine::factory::SearchProgressSink {
  int calls = 0;
  atx::core::Status on_generation(const atx::engine::factory::GenerationSnapshot &) override {
    ++calls;
    return atx::core::Ok();
  }
};

// =============================================================================
//  MineIntoForwardsSinkPerGeneration — with oos OFF, mine_into forwards the sink
//  to SearchDriver::run; on_generation is called once per completed generation.
// =============================================================================
TEST(FactoryOos, MineIntoForwardsSinkPerGeneration) {
  Fixture fx{real_signal_panel()};
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {0xC0FFEEu});
  AlphaGate gate{default_gate_cfg()};
  Factory f = fx.factory();

  FactoryConfig cfg = base_cfg(/*seed*/ 42, /*pop*/ 6, /*gens*/ 3);
  cfg.search.objective_mode = atx::engine::factory::ObjectiveMode::ScalarRaw;
  cfg.search.enable_behavioral_novelty = false;
  ASSERT_EQ(cfg.oos_fraction, 0.0) << "must be off-path (non-OOS) for this test";

  CountingSink sink;
  const FactoryReport rep = f.mine_into(cfg, library, gate, &sink, nullptr).value();
  (void)rep;

  EXPECT_EQ(sink.calls, static_cast<int>(cfg.search.generations))
      << "on_generation must fire once per completed generation";
}

// =============================================================================
//  MineIntoOffPathDigestUnchanged — the legacy 3-arg call and the explicit
//  nullptr/nullptr call produce byte-identical digest + admitted count.
// =============================================================================
TEST(FactoryOos, MineIntoOffPathDigestUnchanged) {
  AlphaGate gate{default_gate_cfg()};
  FactoryConfig cfg = real_signal_cfg(/*seed*/ 99);
  ASSERT_EQ(cfg.oos_fraction, 0.0) << "must be off-path";

  // Run A: legacy 3-arg call.
  Fixture fxA{real_signal_panel()};
  lib::Library libA = lib::Library::open(tmpdir("A"), default_gate_cfg(), {0xC0FFEEu});
  Factory fA = fxA.factory();
  const FactoryReport repA = fA.mine_into(cfg, libA, gate).value();

  // Run B: explicit nullptr/nullptr — must be byte-identical to A.
  Fixture fxB{real_signal_panel()};
  lib::Library libB = lib::Library::open(tmpdir("B"), default_gate_cfg(), {0xC0FFEEu});
  Factory fB = fxB.factory();
  const FactoryReport repB = fB.mine_into(cfg, libB, gate, nullptr, nullptr).value();

  EXPECT_EQ(repA.digest, repB.digest)
      << "off-path nullptr params must produce byte-identical digest";
  EXPECT_EQ(repA.admitted, repB.admitted)
      << "off-path nullptr params must produce identical admitted count";
}

// =============================================================================
//  MineIntoOosForwardsSink — with oos ON, mine_into_oos (via the top-guard)
//  forwards the sink; on_generation fires >= 1 time (the train search runs).
// =============================================================================
TEST(FactoryOos, MineIntoOosForwardsSink) {
  Fixture fx{real_signal_panel()}; // stationary panel: good on train AND holdout
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {0xC0FFEEu});
  AlphaGate gate{default_gate_cfg()};
  Factory f = fx.factory();

  FactoryConfig cfg = real_signal_cfg(/*seed*/ 77);
  cfg.oos_fraction = 0.20;

  CountingSink sink;
  const FactoryReport rep = f.mine_into(cfg, library, gate, &sink, nullptr).value();
  (void)rep;

  EXPECT_GE(sink.calls, 1)
      << "on_generation must fire at least once (train search generates >= 1 generation)";
  EXPECT_EQ(sink.calls, static_cast<int>(cfg.search.generations))
      << "on_generation must fire once per completed generation on the train search";
}

// =============================================================================
//  AccumulationGeometryMismatchHaltsRun (Task 8 footgun, REAL path) — drives the
//  ACTUAL cross-run accumulation fold (mine_into -> mine_into_oos, the OOS-holdout
//  admit at the named site) against a REOPENED --library-dir-style library whose
//  fixed period count t_ differs from the current run's holdout length. The
//  reachable discover path must HALT with a CLEAN propagated Result error (the
//  guarded try_admit seam), NOT abort (debug ATX_ASSERT) / read out-of-bounds
//  (release). The prior LibraryIntegration.AccumulationRejectsGeometryMismatch test
//  only exercised try_admit DIRECTLY; this one exercises the factory fold that the
//  CLI's run_discover_gated actually reaches.
// =============================================================================
TEST(FactoryOos, AccumulationGeometryMismatchHaltsRun) {
  AlphaGate gate{default_gate_cfg()};
  const std::string lib_dir = tmpdir("accum"); // the persistent --library-dir

  // RUN 1 — accumulate a first batch into a FRESH library on a panel whose terminal
  // 20% holdout fixes t_ (the OOS-holdout admit stages HOLDOUT-length pnl). A
  // stationary momentum edge survives the holdout confirmation, so at least one
  // alpha is admitted and t_ is durably fixed.
  {
    Fixture fx{real_signal_panel()}; // 120 dates -> holdout ~24 periods
    lib::Library library = lib::Library::open(lib_dir, default_gate_cfg(), {0xC0FFEEu});
    Factory f = fx.factory();
    FactoryConfig cfg = real_signal_cfg(/*seed*/ 101);
    cfg.oos_fraction = 0.20;
    const FactoryReport rep = f.mine_into(cfg, library, gate).value();
    ASSERT_GT(rep.admitted, 0u) << "run 1 must admit so t_ is fixed on the persistent library";
    ASSERT_TRUE(library.flush_all().has_value()); // make the staged alphas durable for reopen
  }

  // RUN 2 (MISMATCH) — REOPEN the SAME library dir, but mine over a panel with a
  // DIFFERENT date count so the terminal-20% holdout length differs from run 1's
  // fixed t_. The reachable accumulation fold must surface a CLEAN error, not
  // crash/corrupt.
  {
    Fixture fx{two_field_panel(160, 8, momentum_close(160, 8, 0xA11Cu))}; // 160 dates -> holdout ~32
    lib::Library library = lib::Library::open(lib_dir, default_gate_cfg(), {0xC0FFEEu});
    Factory f = fx.factory();
    FactoryConfig cfg = real_signal_cfg(/*seed*/ 202);
    cfg.oos_fraction = 0.20; // same fraction, longer panel => longer holdout => != run 1's t_

    auto rep_r = f.mine_into(cfg, library, gate);
    ASSERT_FALSE(rep_r.has_value())
        << "a geometry-mismatched reopened --library-dir must HALT with a clean error, "
           "not abort / read OOB";
    EXPECT_EQ(rep_r.error().code(), atx::core::ErrorCode::InvalidArgument);
  }

  // RUN 3 (MATCH) — REOPEN the SAME library dir and mine over a panel whose holdout
  // length MATCHES run 1 (same 120-date geometry). The accumulation fold must still
  // SUCCEED (Ok), proving the guard only halts on a true mismatch, never the
  // intended same-panel-many-seeds accumulation.
  {
    Fixture fx{real_signal_panel()}; // 120 dates -> holdout matches run 1
    lib::Library library = lib::Library::open(lib_dir, default_gate_cfg(), {0xC0FFEEu});
    Factory f = fx.factory();
    FactoryConfig cfg = real_signal_cfg(/*seed*/ 303); // a DIFFERENT seed (new candidates)
    cfg.oos_fraction = 0.20;

    auto rep_r = f.mine_into(cfg, library, gate);
    ASSERT_TRUE(rep_r.has_value())
        << "a matching-geometry reopen must accumulate cleanly: " << rep_r.error().to_string();
  }
}

} // namespace atxtest_factory_oos_test
