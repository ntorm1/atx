// blocking_pbo_test.cpp — p8 Sprint 5 (S5-2, sub-seam 2): make the run-level PBO
// gate BLOCKING under --blocking-pbo, distinct from --pbo-hard-block (which only
// escalates the STAGE's exit code, in stage_discover.cpp, while Factory::mine_into
// itself still returns Ok). finalize_run_pbo's verdict (rep.pbo_gate_passed) has
// always been ADVISORY-but-RECORDED; FactoryConfig::blocking_pbo (S5-2) makes a
// breach FAIL the Factory call itself (Err instead of Ok(rep)) — a fail-closed
// escalation checked AFTER the admit loop (the library may already have persisted
// this run's admits, the same documented caveat --pbo-hard-block already carries).

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/combine/gate.hpp"

#include "atx/engine/factory/factory.hpp"

#include "atx/engine/library/library.hpp"

namespace atxtest_blocking_pbo {

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

namespace lib = atx::engine::library;

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

[[nodiscard]] FactoryConfig permissive_cfg(atx::u64 seed) {
  FactoryConfig cfg;
  cfg.search.master_seed = seed;
  cfg.search.population = 16;
  cfg.search.generations = 4;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.search.enable_behavioral_novelty = true;
  cfg.seed_exprs = {"rank(close)",
                    "rank(rev)",
                    "ts_mean(close, 5)",
                    "ts_mean(rev, 3)",
                    "rank(ts_mean(close, 10))",
                    "delta(close, 2)"};
  cfg.panel_fields = {"close", "rev"};
  cfg.min_dsr = 0.0; // permissive: maximize admitted count so the PBO cross-section is feasible
  cfg.oos_fraction = 0.20;
  return cfg;
}

[[nodiscard]] GateConfig permissive_gate_cfg() {
  GateConfig gc;
  gc.min_sharpe = 0.0;
  gc.min_fitness = 0.0;
  gc.max_turnover = 10.0;
  gc.max_pool_corr = 1.0; // allow fully correlated — maximize admitted count
  return gc;
}

[[nodiscard]] std::string tmpdir(const std::string &tag) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S5_2") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s5_2_blocking_pbo" / base;
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

// ---------------------------------------------------------------------------
// UnadmitsOnBreach — RED before the wire (blocking_pbo did not exist; a breach
// was always advisory-only — finalize_run_pbo's own doc: "never un-persists an
// alpha or alters admission"). GREEN: check_blocking_pbo (called by mine_into
// AFTER finalize_run_pbo, at every persistent-library admit site) fails the run
// CLOSED (Err) when blocking_pbo is set and the run-level gate is breached;
// WITHOUT it (advisory-only, the default), the SAME breach leaves the caller Ok —
// the distinguishing behavior --pbo-hard-block (a stage_discover.cpp-level,
// exit-code-only escalation that never touches Factory::mine_into's return value
// at all) does not provide.
//
// admitted_pnls is HAND-BUILT i.i.d. noise (no genuine edge) — the doorway
// finalize_run_pbo's OWN header doc invites ("Declared so a unit test can verify
// the verdict on hand-built admitted_pnls"). Pure noise is exactly the case
// Bailey/Lopez de Prado's CSCV-PBO targets (an in-sample "best" that is actually
// OOS noise) and reliably yields pbo > 0 — unlike this file's/cascade's
// real_signal_panel, which is a deliberately-STATIONARY persistent edge with
// pbo ~ 0 by construction (confirmed empirically: FactoryOos's own R3b test has
// to special-case a pbo==0 fallback for that exact fixture).
// ---------------------------------------------------------------------------
TEST(BlockingPbo, UnadmitsOnBreach) {
  namespace factory = atx::engine::factory;

  Lcg rng{0xBEEF01ULL};
  constexpr usize kCands = 6;
  constexpr usize kRawT = 9; // raw_t=9 -> periods=8 after dropping the structural index-0 zero
  std::vector<std::vector<f64>> admitted_pnls(kCands, std::vector<f64>(kRawT, 0.0));
  for (usize c = 0; c < kCands; ++c) {
    for (usize t = 1; t < kRawT; ++t) {
      admitted_pnls[c][t] = 0.01 * rng.next(); // pure noise: no persistent cross-split edge
    }
  }

  // Observe the noise fixture's OWN pbo (always_compute=true forces the compute
  // even at the max_pbo=1.0 default; the gate still fail-opens at that default).
  FactoryReport rep_observe;
  factory::detail::finalize_run_pbo(rep_observe, admitted_pnls, /*max_pbo=*/1.0,
                                    /*always_compute=*/true);
  ASSERT_TRUE(std::isfinite(rep_observe.pbo)) << "pbo must be computed (>=2 candidates, feasible matrix)";
  ASSERT_GT(rep_observe.pbo, 0.0) << "pure noise must show SOME overfit signal (else this fixture "
                                     "is degenerate — widen kCands/kRawT or reseed)";
  EXPECT_TRUE(rep_observe.pbo_gate_passed) << "max_pbo=1.0 (off) must fail-open regardless of pbo";

  // A threshold strictly BELOW the observed pbo guarantees a real breach (the
  // FactoryOos.R3b_OosPboEqualsRunPboAndGateCapable technique, applied directly).
  const f64 breach_max_pbo = std::nextafter(rep_observe.pbo, 0.0);
  FactoryReport rep_breach;
  factory::detail::finalize_run_pbo(rep_breach, admitted_pnls, breach_max_pbo);
  ASSERT_FALSE(rep_breach.pbo_gate_passed) << "the chosen threshold must breach by construction";

  // (b) advisory-only (blocking_pbo=false, the default): check_blocking_pbo must
  // NOT fail the run on this breach — today's behavior, unchanged.
  FactoryConfig cfg_advisory;
  cfg_advisory.max_pbo = breach_max_pbo;
  ASSERT_FALSE(cfg_advisory.blocking_pbo) << "blocking_pbo must default false (inert)";
  const auto verdict_advisory = factory::detail::check_blocking_pbo(cfg_advisory, rep_breach);
  EXPECT_TRUE(verdict_advisory.has_value())
      << "advisory-only (blocking_pbo=false) must NOT fail closed on a breach";

  // (c) SAME breach, --blocking-pbo set: check_blocking_pbo now fails CLOSED.
  FactoryConfig cfg_blocking = cfg_advisory;
  cfg_blocking.blocking_pbo = true;
  const auto verdict_blocking = factory::detail::check_blocking_pbo(cfg_blocking, rep_breach);
  EXPECT_FALSE(verdict_blocking.has_value())
      << "--blocking-pbo must fail the run CLOSED (Err) on the same breach";
}

// ---------------------------------------------------------------------------
// InertAtMaxPboOff — blocking_pbo=true but max_pbo left at its 1.0 (off) default:
// finalize_run_pbo's own contract fail-opens (pbo_gate_passed stays true)
// regardless of always_compute, so mine_into must still return Ok.
// ---------------------------------------------------------------------------
TEST(BlockingPbo, InertAtMaxPboOffDefault) {
  AlphaGate gate{permissive_gate_cfg()};
  FactoryConfig cfg = permissive_cfg(/*seed*/ 11);
  cfg.blocking_pbo = true;
  ASSERT_DOUBLE_EQ(cfg.max_pbo, 1.0) << "max_pbo must default to the OFF sentinel";

  Fixture fx{real_signal_panel()};
  lib::Library library = lib::Library::open(tmpdir("inert_max_pbo_off"), permissive_gate_cfg(), {0xC0FFEEu});
  Factory f = fx.factory();
  const auto rep_r = f.mine_into(cfg, library, gate);
  ASSERT_TRUE(rep_r.has_value())
      << "blocking_pbo=true with max_pbo=1.0 (off) must be a no-op (byte-identical Ok path)";
  EXPECT_TRUE(rep_r->pbo_gate_passed);
}

} // namespace atxtest_blocking_pbo
