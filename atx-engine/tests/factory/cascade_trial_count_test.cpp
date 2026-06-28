// cascade_trial_count_test.cpp — p7 Sprint 1 (S1-4): thread the realized trial
// count N into the cascade pre-gate bound (cascade_gate_passes).
//
// The pre-gate is a CONSERVATIVE TRUE UPPER BOUND: it skips the expensive holdout
// eval for a candidate whose train Sharpe is provably too weak to clear min_dsr,
// but it MUST NEVER skip a candidate the gate-off run would have admitted (else the
// admitted set / digest / reject_histogram diverge — the binding AdmittedSetUnchanged
// proof). S1-4 folds the expected-maximum-Sharpe benchmark SR*_N into the keep side:
//   keep iff  sr_tr * cascade_gate_factor + SR*_N >= min_dsr
// with SR*_N = expected_max_sharpe(N, 1/T) >= 0, monotone non-decreasing in N, 0 at
// N<=1. Adding the non-negative SR*_N can only RELAX the keep test, so the bound
// monotone-LOOSENS in N (the strictly safe direction) and is byte-identical at N<=1
// / min_dsr<=0 / factor<=0.
//
// cascade_gate_passes is a translation-unit-local helper (anonymous namespace in
// factory.cpp), so it is not directly callable here. This file therefore:
//   (1) MIRRORS the exact bound arithmetic (using the SAME public eval::expected_max_sharpe
//       and combine::kAnnualizationDays the implementation uses) to unit-test the inert /
//       monotone / safe math directly; and
//   (2) EXERCISES the real predicate end-to-end through mine_into_oos (serial) and
//       mine_into_oos_parallel (ProcessExecutor), with the realized trial_count > 1, to
//       prove byte-identity (gate ON == OFF), seq==parallel, twice-run, and that the gate fires.

#include <cmath>    // std::sqrt
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp" // alpha::Library (op-signature registry)
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/combine/gate.hpp"     // GateConfig, AlphaGate
#include "atx/engine/combine/metrics.hpp"  // combine::kAnnualizationDays

#include "atx/engine/eval/deflated_sharpe.hpp" // eval::expected_max_sharpe (the bound term)

#include "atx/engine/factory/factory.hpp"

#include "atx/engine/library/library.hpp"

#include "atx/engine/parallel/process_executor.hpp"

namespace atxtest_cascade_trial_count {

using atx::f64;
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
using atx::engine::parallel::ExecutorConfig;
using atx::engine::parallel::ProcessExecutor;

namespace lib = atx::engine::library;
namespace eval = atx::engine::eval;
namespace combine = atx::engine::combine;

// ===========================================================================
//  Part 1 — direct math mirror of cascade_gate_passes (TU-local helper is
//  anonymous in factory.cpp, so we re-state the EXACT arithmetic here using the
//  same public building blocks the implementation uses).
// ===========================================================================

// VERBATIM mirror of cascade_gate_passes' decision (annualized train Sharpe in,
// keep/skip out). Kept in lock-step with factory.cpp by construction (same
// eval::expected_max_sharpe, same kAnnualizationDays, same variance proxy 1/T).
[[nodiscard]] bool cascade_keep(f64 train_sharpe_annualized, f64 cascade_gate_factor, f64 min_dsr,
                                usize trial_count) noexcept {
  if (!(cascade_gate_factor > 0.0)) {
    return true;
  }
  if (!(min_dsr > 0.0)) {
    return true;
  }
  const f64 sr_tr = train_sharpe_annualized / std::sqrt(combine::kAnnualizationDays);
  if (!std::isfinite(sr_tr)) {
    return true;
  }
  const f64 sr_star_n = (trial_count > 1U)
                            ? eval::expected_max_sharpe(trial_count, 1.0 / combine::kAnnualizationDays)
                            : 0.0;
  return sr_tr * cascade_gate_factor + sr_star_n >= min_dsr;
}

constexpr f64 kFactor = 3.0;
constexpr f64 kMinDsr = 0.5;

// cascade_trial_count_inert_n1: trial_count=1 reproduces the old void-path bound.
// Sharpe=0.10 annualized -> sr_tr ~ 0.0063, *3.0 ~ 0.0189 < 0.5 -> SKIP (keep=false).
TEST(CascadeTrialCount, InertN1MatchesOldBound) {
  EXPECT_FALSE(cascade_keep(/*sharpe*/ 0.10, kFactor, kMinDsr, /*N*/ 1));
  // The N=1 result must equal the bound with SR*_N forced to 0 (the old formula).
  const f64 sr_tr = 0.10 / std::sqrt(combine::kAnnualizationDays);
  EXPECT_EQ(cascade_keep(0.10, kFactor, kMinDsr, 1), (sr_tr * kFactor >= kMinDsr));
}

// cascade_trial_count_inert_min_dsr_zero: min_dsr=0 -> always keep (gate off),
// for any trial_count -> byte-identical pre-S1-4 behavior.
TEST(CascadeTrialCount, InertMinDsrZeroAlwaysKeeps) {
  for (const usize n : {usize{1}, usize{10}, usize{50}, usize{100}}) {
    EXPECT_TRUE(cascade_keep(/*sharpe*/ 0.0, kFactor, /*min_dsr*/ 0.0, n));
    EXPECT_TRUE(cascade_keep(/*sharpe*/ -5.0, kFactor, /*min_dsr*/ 0.0, n));
  }
}

// cascade_trial_count_factor_off: factor<=0 -> always keep (gate inert).
TEST(CascadeTrialCount, InertFactorOffAlwaysKeeps) {
  EXPECT_TRUE(cascade_keep(0.10, /*factor*/ 0.0, kMinDsr, 50));
}

// cascade_trial_count_n50_still_skips: a hopeless candidate (Sharpe=0.10) stays
// skipped even at N=50 (the small SR*_N addition cannot rescue it).
TEST(CascadeTrialCount, HopelessStaysSkippedAtLargeN) {
  EXPECT_FALSE(cascade_keep(/*sharpe*/ 0.10, kFactor, kMinDsr, /*N*/ 50));
  EXPECT_FALSE(cascade_keep(/*sharpe*/ 0.10, kFactor, kMinDsr, /*N*/ 100));
}

// cascade_trial_count_monotone: SR*_N is monotone non-decreasing in N and is added
// to the keep side, so the bound monotone-LOOSENS: a candidate KEPT at low N is also
// KEPT at high N, i.e. skip@N=high => skip@N=low (the plan's concrete monotone assertion).
TEST(CascadeTrialCount, MonotoneLoosensWithN) {
  // SR*_N rises with N.
  const f64 s10 = eval::expected_max_sharpe(10, 1.0 / combine::kAnnualizationDays);
  const f64 s50 = eval::expected_max_sharpe(50, 1.0 / combine::kAnnualizationDays);
  const f64 s100 = eval::expected_max_sharpe(100, 1.0 / combine::kAnnualizationDays);
  EXPECT_GE(s50, s10);
  EXPECT_GE(s100, s50);

  // Across a sweep of train Sharpes, keep is monotone non-decreasing in N (never
  // flips keep->skip as N grows). Hence skip@high => skip@low.
  for (const f64 sharpe : {0.0, 0.5, 1.0, 2.0, 4.0, 8.0}) {
    const bool k1 = cascade_keep(sharpe, kFactor, kMinDsr, 1);
    const bool k10 = cascade_keep(sharpe, kFactor, kMinDsr, 10);
    const bool k100 = cascade_keep(sharpe, kFactor, kMinDsr, 100);
    EXPECT_TRUE(k10 || !k1) << "keep must not flip true->false from N=1 to N=10 (sharpe=" << sharpe
                            << ")";
    EXPECT_TRUE(k100 || !k10) << "keep must not flip true->false from N=10 to N=100 (sharpe="
                              << sharpe << ")";
  }
}

// cascade_trial_count_safe: a strong candidate (high train Sharpe) that clears the
// bound at N=1 must NEVER be skipped at any larger N (the true-upper-bound contract).
TEST(CascadeTrialCount, KeeperNeverSkippedAtAnyN) {
  const f64 strong_sharpe = 3.0; // sr_tr ~ 0.189, *3.0 ~ 0.567 >= 0.5 -> keep at N=1
  ASSERT_TRUE(cascade_keep(strong_sharpe, kFactor, kMinDsr, 1));
  for (const usize n : {usize{10}, usize{50}, usize{100}, usize{1000}}) {
    EXPECT_TRUE(cascade_keep(strong_sharpe, kFactor, kMinDsr, n))
        << "a keeper at N=1 must remain a keeper at N=" << n;
  }
}

// twice-run on the pure bound: same inputs => same result (no state).
TEST(CascadeTrialCount, TwiceRunSameResult) {
  const bool a = cascade_keep(0.10, kFactor, kMinDsr, 50);
  const bool b = cascade_keep(0.10, kFactor, kMinDsr, 50);
  EXPECT_EQ(a, b);
}

// ===========================================================================
//  Part 2 — end-to-end exercise of the REAL predicate through the mine paths.
//  Fixtures mirror factory_oos_test.cpp (self-contained: each *_test.cpp is its
//  own TU).
// ===========================================================================

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

[[nodiscard]] Panel real_signal_panel() {
  const usize dates = 120;
  const usize insts = 8;
  std::vector<f64> close = momentum_close(dates, insts, 0xA11Cu);
  std::vector<f64> rev = reversal_of(close, dates, insts);
  return make_panel(dates, insts, {"close", "rev"}, {close, rev});
}

[[nodiscard]] GateConfig default_gate_cfg() { return GateConfig{}; }

[[nodiscard]] FactoryConfig real_signal_cfg(atx::u64 seed) {
  FactoryConfig cfg;
  cfg.search.master_seed = seed;
  cfg.search.population = 16;
  cfg.search.generations = 4;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.search.enable_behavioral_novelty = true;
  cfg.search.fitness.trial_count = 4;
  cfg.seed_exprs = {"rank(close)",
                    "rank(rev)",
                    "ts_mean(close, 5)",
                    "ts_mean(rev, 3)",
                    "rank(ts_mean(close, 10))",
                    "delta(close, 2)"};
  cfg.panel_fields = {"close", "rev"};
  cfg.min_dsr = kMinDsr;
  return cfg;
}

[[nodiscard]] std::string tmpdir(const std::string &tag = "") {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S1_4") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s1_4_cascade" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

struct Fixture {
  Library lib{};                      // alpha::Library: the op-signature registry the Factory borrows
  Panel panel;
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  explicit Fixture(Panel p) : panel{std::move(p)} {}
  [[nodiscard]] Factory factory() { return Factory{lib, panel, sim, policy}; }
};

// cascade_trial_count_real_n_gt_1: with the gate ON and a realized trial_count > 1,
// the admitted SET, digest, version_id, and reject_histogram are byte-identical to
// the gate-OFF run (the true-upper-bound proof holds at the real N), and the gate
// actually fires (n_cascade_skipped > 0).
TEST(CascadeTrialCount, RealRunAdmittedSetUnchangedAtRealN) {
  AlphaGate gate{default_gate_cfg()};

  // Run A: gate OFF.
  FactoryConfig cfg_off = real_signal_cfg(/*seed*/ 17);
  cfg_off.oos_fraction = 0.20;
  ASSERT_EQ(cfg_off.cascade_gate_factor, 0.0);
  Fixture fxA{real_signal_panel()};
  lib::Library libA = lib::Library::open(tmpdir("off"), default_gate_cfg(), {0xC0FFEEu});
  Factory fA = fxA.factory();
  const FactoryReport repA = fA.mine_into(cfg_off, libA, gate).value();
  ASSERT_GT(repA.admitted, 0u) << "the stationary edge must admit >= 1 (else the proof is vacuous)";
  ASSERT_EQ(repA.n_cascade_skipped, 0u);

  // Run B: gate ON. The realized trial_count (res.trial_count) is > 1 for this
  // multi-generation run, so the S1-4 SR*_N term is live.
  FactoryConfig cfg_on = cfg_off;
  cfg_on.cascade_gate_factor = kFactor;
  Fixture fxB{real_signal_panel()};
  lib::Library libB = lib::Library::open(tmpdir("on"), default_gate_cfg(), {0xC0FFEEu});
  Factory fB = fxB.factory();
  const FactoryReport repB = fB.mine_into(cfg_on, libB, gate).value();

  EXPECT_GT(repB.trials, 1u) << "realized trial_count must be > 1 so SR*_N is exercised";
  EXPECT_GT(repB.n_cascade_skipped, 0u) << "the gate must fire (else the test is vacuous)";
  EXPECT_EQ(repB.digest, repA.digest) << "S1-4 bound skipped an admitting candidate (too tight)";
  EXPECT_EQ(repB.admitted, repA.admitted);
  EXPECT_EQ(libB.snapshot().version_id, libA.snapshot().version_id);
  EXPECT_EQ(repB.reject_histogram, repA.reject_histogram);
}

// cascade_trial_count_seq_eq_parallel: serial (mine_into_oos) == parallel
// (mine_into_oos_parallel, ProcessExecutor) with the gate ON and trial_count > 1.
TEST(CascadeTrialCount, SeqEqualsParallelAtRealN) {
  AlphaGate gate{default_gate_cfg()};

  FactoryConfig cfg = real_signal_cfg(/*seed*/ 13);
  cfg.oos_fraction = 0.20;
  cfg.cascade_gate_factor = kFactor;
  cfg.search.seed_from_grammar = false;

  Fixture fxSerial{real_signal_panel()};
  lib::Library libSerial = lib::Library::open(tmpdir("seq"), default_gate_cfg(), {0xC0FFEEu});
  Factory fSerial = fxSerial.factory();
  const FactoryReport repSerial = fSerial.mine_into(cfg, libSerial, gate).value();

  Fixture fxPar{real_signal_panel()};
  lib::Library libPar = lib::Library::open(tmpdir("par"), default_gate_cfg(), {0xC0FFEEu});
  Factory fPar = fxPar.factory();
  ProcessExecutor execPar{ExecutorConfig{2, false}};
  const FactoryReport repPar = fPar.mine_into(cfg, libPar, gate, execPar).value();

  EXPECT_GT(repSerial.trials, 1u);
  EXPECT_EQ(repSerial.digest, repPar.digest);
  EXPECT_EQ(repSerial.admitted, repPar.admitted);
  EXPECT_EQ(libSerial.snapshot().version_id, libPar.snapshot().version_id);
  EXPECT_EQ(repSerial.reject_histogram, repPar.reject_histogram);
  EXPECT_EQ(repSerial.n_cascade_skipped, repPar.n_cascade_skipped);
}

// cascade_trial_count_twice_run: the same config replays byte-identically (no state).
TEST(CascadeTrialCount, RealRunTwiceIdentical) {
  AlphaGate gate{default_gate_cfg()};
  FactoryConfig cfg = real_signal_cfg(/*seed*/ 21);
  cfg.oos_fraction = 0.20;
  cfg.cascade_gate_factor = kFactor;

  Fixture fx1{real_signal_panel()};
  lib::Library lib1 = lib::Library::open(tmpdir("r1"), default_gate_cfg(), {0xC0FFEEu});
  Factory f1 = fx1.factory();
  const FactoryReport r1 = f1.mine_into(cfg, lib1, gate).value();

  Fixture fx2{real_signal_panel()};
  lib::Library lib2 = lib::Library::open(tmpdir("r2"), default_gate_cfg(), {0xC0FFEEu});
  Factory f2 = fx2.factory();
  const FactoryReport r2 = f2.mine_into(cfg, lib2, gate).value();

  EXPECT_EQ(r1.digest, r2.digest);
  EXPECT_EQ(r1.admitted, r2.admitted);
  EXPECT_EQ(r1.n_cascade_skipped, r2.n_cascade_skipped);
  EXPECT_EQ(lib1.snapshot().version_id, lib2.snapshot().version_id);
}

} // namespace atxtest_cascade_trial_count
