// atx::engine::parallel — S7.6 (A) the consolidated FIVE-PATH digest-invariance
// CAPSTONE. The sprint's single explicit pin that, for ALL FOUR real workloads,
// the determinism digest is BYTE-IDENTICAL across the five execution paths
//
//   { sequential, ThreadExecutor@1, ThreadExecutor@N, ProcessExecutor@1, ProcessExecutor@N }
//
// (N = 4, with an additional larger thread leg @ 8 to prove the thread substrate
// is invariant well past the {1,4} the process legs exercise). Per-workload digest
// identity was already proven in S7.5b/c/d; THIS file consolidates those into ONE
// place so a regression in ANY single path — a thread-count drift, a serialization
// bug across the process boundary, a non-deterministic admit fold — is caught by a
// single failing assertion that names the exact (workload, path) that diverged.
//
//   * eval       -> signal_set_digest(parallel_evaluate(progs, panel, exec))
//   * backtests  -> result_table_digest(parallel_backtests(streams, book, exec))
//   * cpcv       -> result_table_digest(parallel_cpcv(folds, streams, alpha, book, exec))
//   * mine       -> FactoryReport::digest AND the resulting library version_id
//                   from mine_into(cfg, lib, gate, exec)
//
// For each workload we compute ONE `want` (the sequential / in-process oracle) and
// EXPECT_EQ every one of the five legs to it. The five paths are a TABLE/LOOP
// (kSubstratePlan below): adding a 6th substrate later is one row, not a new test.
//
// NON-VACUITY: a comparison of two zero digests is meaningless, so every workload
// asserts its oracle is non-trivial (eval/backtests/cpcv digests != 0; mine admits
// > 0 alphas) BEFORE comparing the legs — a five-way equality of nothing cannot
// pass by accident.
//
// The builders (panel / streams / genome library / factory fixture) are the SAME
// shapes the S7.5b/c/d process tests use, lifted minimally per the sprint's
// duplicate-or-share guidance (each existing process test owns its builders too;
// the capstone's contribution is the five-path assertion, not the fixtures).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp" // ATX_CHECK — builders fail loudly (never a vacuous degraded test)
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/combine/gate.hpp"

#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/factory/factory.hpp"
#include "atx/engine/factory/genome.hpp"

#include "atx/engine/eval/cpcv.hpp"

#include "atx/engine/library/library.hpp"

#include "atx/engine/parallel/batch_eval.hpp"
#include "atx/engine/parallel/digest.hpp"
#include "atx/engine/parallel/executor.hpp"
#include "atx/engine/parallel/parallel_run.hpp"
#include "atx/engine/parallel/process_executor.hpp"
#include "atx/engine/parallel/thread_executor.hpp"

namespace {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::analyze;
using atx::engine::alpha::AlphaStreams;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::GateConfig;
using atx::engine::eval::CpcvConfig;
using atx::engine::eval::CpcvFold;
using atx::engine::eval::LabelSpan;
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
using atx::engine::factory::Genome;
using atx::engine::parallel::ExecutorConfig;
using atx::engine::parallel::FoldResult;
using atx::engine::parallel::IExecutor;
using atx::engine::parallel::parallel_backtests;
using atx::engine::parallel::parallel_cpcv;
using atx::engine::parallel::parallel_evaluate;
using atx::engine::parallel::ProcessExecutor;
using atx::engine::parallel::result_table_digest;
using atx::engine::parallel::run_full_backtest;
using atx::engine::parallel::run_one_fold;
using atx::engine::parallel::signal_set_digest;
using atx::engine::parallel::Substrate;
using atx::engine::parallel::ThreadExecutor;

namespace lib = atx::engine::library;

constexpr f64 kBook = 1.0e6;

// ===========================================================================
//  THE FIVE-PATH TABLE — one row per substrate leg, driven by a uniform loop.
//
//  Each row knows how to MAKE its executor (a fresh, owned IExecutor per leg —
//  five-path comparisons over a SHARED substrate would be vacuous; a fresh one
//  per leg is what makes the worker-count + transport the only moving part) and
//  carries a human label for the failing-assertion message. The four sequential
//  oracles are computed separately (no executor); these are the four NON-sequential
//  legs. Adding a 6th substrate is one row here, not a new test.
// ===========================================================================
enum class Substr : std::uint8_t { Thread, Process };

struct SubstratePlan {
  std::string_view label; // names the leg in a failing EXPECT_EQ
  Substr kind;            // which IExecutor to construct
  usize workers;          // worker count for this leg
};

// {ThreadExecutor@1, ThreadExecutor@4, ThreadExecutor@8, ProcessExecutor@1,
//  ProcessExecutor@4}. The thread substrate gets the extra @8 leg (the brief's
// "larger thread N") to prove invariance past the {1,4} the process legs run.
constexpr SubstratePlan kSubstratePlan[] = {
    {"ThreadExecutor@1", Substr::Thread, 1},
    {"ThreadExecutor@4", Substr::Thread, 4},
    {"ThreadExecutor@8", Substr::Thread, 8},
    {"ProcessExecutor@1", Substr::Process, 1},
    {"ProcessExecutor@4", Substr::Process, 4},
};

// Construct a FRESH executor for one plan row. unique_ptr<IExecutor> so the loop
// is substrate-agnostic above the seam (exactly the IExecutor contract). pin=false
// matches every existing S7.5b/c/d process test (CPU pinning is a no-op anyway).
[[nodiscard]] std::unique_ptr<IExecutor> make_executor(const SubstratePlan &p) {
  switch (p.kind) {
    case Substr::Thread:
      return std::make_unique<ThreadExecutor>(ExecutorConfig{p.workers, false});
    case Substr::Process:
      return std::make_unique<ProcessExecutor>(ExecutorConfig{p.workers, false});
  }
  return nullptr; // unreachable: the enum is exhaustively switched above
}

// ---- EVAL builders (trimmed mirror of parallel_workload_eval_process_test) ----

// Process-lifetime DSL Library: parsed Asts borrow OpSig pointers from it.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] Panel make_eval_panel(usize dates, usize instruments, std::uint64_t seed) {
  const usize cells = dates * instruments;
  std::vector<std::string> names = {"open", "high", "low", "close", "volume", "vwap", "returns"};
  std::vector<std::vector<f64>> cols(names.size(), std::vector<f64>(cells));
  std::uint64_t state = seed | 1ULL;
  auto next = [&state]() noexcept -> f64 {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) / static_cast<f64>(1ULL << 53);
  };
  for (usize i = 0; i < cells; ++i) {
    const f64 base = 10.0 + next() * 190.0;
    const f64 spread = next() * 5.0;
    const f64 hi = base + spread;
    const f64 lo = base - spread;
    cols[0][i] = base;
    cols[1][i] = hi;
    cols[2][i] = lo;
    cols[3][i] = lo + (hi - lo) * 0.5;
    cols[4][i] = 1.0e4 + next() * 9.9e5;
    cols[5][i] = (hi + lo + cols[3][i]) / 3.0;
    cols[6][i] = next() * 0.1 - 0.05;
  }
  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});
  auto p = Panel::create(dates, instruments, names, cols, universe);
  // ALWAYS-ON: a silently-degraded empty panel would make every five-path leg agree on a
  // degenerate digest (a VACUOUS pass), so abort loudly rather than EXPECT_TRUE + value_or.
  ATX_CHECK(p.has_value() && "make_eval_panel: Panel::create failed");
  return std::move(p).value();
}

[[nodiscard]] Program compile_named(usize k, std::string_view src) {
  const std::string text = "a" + std::to_string(k) + " = " + std::string(src);
  auto ast = parse_program(text, shared_lib());
  // ALWAYS-ON: a degraded empty Program would make the eval legs agree vacuously, so abort
  // loudly on any compile-pipeline failure rather than EXPECT_TRUE + value_or(Program{}).
  ATX_CHECK(ast.has_value() && "compile_named: parse_program failed");
  auto ana = analyze(ast.value());
  ATX_CHECK(ana.has_value() && "compile_named: analyze failed");
  auto prog = compile(ast.value(), ana.value());
  ATX_CHECK(prog.has_value() && "compile_named: compile failed");
  return std::move(prog).value();
}

[[nodiscard]] std::vector<Program> compile_fan() {
  const std::vector<std::string_view> battery = {
      "close - open",       "(high + low) / 2 - vwap", "close / open - 1",
      "high - low",         "(high - low) / close",    "(high + low) / 2",
      "volume * close",     "vwap - close",            "returns * volume",
      "high * 2 - low",     "open * high - low * close"};
  std::vector<Program> progs;
  progs.reserve(battery.size());
  for (usize k = 0; k < battery.size(); ++k) {
    progs.push_back(compile_named(k, battery[k]));
  }
  return progs;
}

// ---- BACKTESTS / CPCV builders (mirror parallel_workload_process_test) --------

[[nodiscard]] AlphaStreams make_streams(usize n_alphas, usize n_periods, usize n_inst,
                                        std::uint64_t seed) {
  AlphaStreams s;
  s.n_alphas_ = n_alphas;
  s.n_periods_ = n_periods;
  s.n_instruments_ = n_inst;
  s.pnl_flat.resize(n_alphas * n_periods);
  s.pos_flat.resize(n_alphas * n_periods * n_inst);
  std::uint64_t st = seed | 1ULL;
  auto nx = [&st] {
    st = st * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(st >> 11) / static_cast<double>(1ULL << 53);
  };
  for (auto &v : s.pnl_flat) {
    v = nx() * 0.04 - 0.02;
  }
  for (auto &v : s.pos_flat) {
    v = nx() * 2.0 - 1.0;
  }
  return s;
}

[[nodiscard]] std::vector<CpcvFold> make_cpcv_folds(usize n_periods) {
  std::vector<LabelSpan> spans;
  spans.reserve(n_periods);
  for (usize i = 0; i < n_periods; ++i) {
    spans.push_back({i, i + 1});
  }
  return atx::engine::eval::cpcv_folds(spans, CpcvConfig{6, 2, 0.0});
}

[[nodiscard]] u64 backtests_sequential_digest(const AlphaStreams &streams) {
  std::vector<FoldResult> seq(streams.n_alphas());
  for (usize a = 0; a < streams.n_alphas(); ++a) {
    seq[a] = run_full_backtest(streams, a, kBook);
  }
  return result_table_digest(seq);
}

[[nodiscard]] u64 cpcv_sequential_digest(const AlphaStreams &streams, usize alpha_id,
                                         std::span<const CpcvFold> folds) {
  std::vector<FoldResult> seq(folds.size());
  for (usize f = 0; f < folds.size(); ++f) {
    seq[f] = run_one_fold(streams, alpha_id, f, folds[f], kBook);
  }
  return result_table_digest(seq);
}

// ---- MINE builders (mirror parallel_workload_mine_process_test) --------------

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
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

[[nodiscard]] Panel mine_panel() {
  const usize dates = 120;
  const usize insts = 8;
  std::vector<f64> close = momentum_close(dates, insts, 0xA11Cu);
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  auto p = Panel::create(dates, insts, {"close", "rev"}, {close, rev}, {});
  EXPECT_TRUE(p.has_value());
  return std::move(p.value());
}

[[nodiscard]] GateConfig default_gate_cfg() { return GateConfig{}; }

[[nodiscard]] FactoryConfig mine_cfg(u64 seed) {
  FactoryConfig cfg;
  cfg.search.master_seed = seed;
  cfg.search.population = 16;
  cfg.search.generations = 4;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.search.enable_behavioral_novelty = true;
  cfg.search.fitness.trial_count = 4;
  cfg.seed_exprs = {"rank(close)",  "rank(rev)",
                    "ts_mean(close, 5)", "ts_mean(rev, 3)",
                    "rank(ts_mean(close, 10))", "delta(close, 2)"};
  cfg.panel_fields = {"close", "rev"};
  cfg.min_dsr = 0.5;
  return cfg;
}

// A per-leg scratch dir for the persistent library (each leg opens a FRESH library
// at an identical empty starting state — that identical start is what makes the
// version_id comparison meaningful, not a same-handle artifact).
[[nodiscard]] std::string tmpdir(std::string_view tag) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->name() : "cap") + "_" + std::string(tag);
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_s76_capstone" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

// The run-wide DSL library the genomes' op pointers alias (NOT the persistent lib).
struct MineFixture {
  Library lib{};
  Panel panel;
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  explicit MineFixture(Panel p) : panel{std::move(p)} {}
  [[nodiscard]] Factory factory() { return Factory{lib, panel, sim, policy}; }
};

[[nodiscard]] u64 version_id_of(lib::Library &library) { return library.snapshot().version_id; }

} // namespace

// ===========================================================================
//  (A.1) EVAL — five paths, one want. signal_set_digest invariant across all.
// ===========================================================================
TEST(ParallelFivePathCapstone, EvalDigestIdenticalAcrossFivePaths) {
  const Panel panel = make_eval_panel(24, 6, 0xABCDEF01ULL);
  const std::vector<Program> progs = compile_fan();

  // PATH 1 — the sequential / in-process oracle (ThreadExecutor@1 is pinned to the
  // single-thread batch in S7.5a, so it IS the sequential reference here).
  ThreadExecutor oracle_exec{ExecutorConfig{1, false}};
  const auto oracle = parallel_evaluate(progs, panel, oracle_exec);
  ASSERT_TRUE(oracle.has_value()) << (oracle ? "" : oracle.error().message());
  const u64 want = signal_set_digest(oracle.value());
  ASSERT_NE(want, u64{0}) << "non-vacuity: the eval oracle digest must be non-trivial";

  // PATHS 2..5 — every substrate leg from the table must match `want`.
  for (const SubstratePlan &p : kSubstratePlan) {
    std::unique_ptr<IExecutor> exec = make_executor(p);
    ASSERT_NE(exec, nullptr) << p.label;
    const auto got = parallel_evaluate(progs, panel, *exec);
    ASSERT_TRUE(got.has_value()) << p.label << ": " << (got ? "" : got.error().message());
    EXPECT_EQ(signal_set_digest(got.value()), want)
        << "FIVE-PATH BREAK [eval]: " << p.label << " diverged from the sequential oracle";
  }
}

// ===========================================================================
//  (A.2) BACKTESTS — five paths, one want. result_table_digest invariant.
// ===========================================================================
TEST(ParallelFivePathCapstone, BacktestsDigestIdenticalAcrossFivePaths) {
  const AlphaStreams streams = make_streams(20, 48, 4, 0x1234ULL);

  // PATH 1 — the sequential oracle (run_full_backtest per alpha in canonical order).
  const u64 want = backtests_sequential_digest(streams);
  ASSERT_NE(want, u64{0}) << "non-vacuity: the backtests oracle digest must be non-trivial";

  // PATHS 2..5 — every substrate leg from the table must match `want`.
  for (const SubstratePlan &p : kSubstratePlan) {
    std::unique_ptr<IExecutor> exec = make_executor(p);
    ASSERT_NE(exec, nullptr) << p.label;
    const u64 got = result_table_digest(parallel_backtests(streams, kBook, *exec));
    EXPECT_EQ(got, want)
        << "FIVE-PATH BREAK [backtests]: " << p.label << " diverged from the sequential oracle";
  }
}

// ===========================================================================
//  (A.3) CPCV — five paths, one want. result_table_digest invariant.
// ===========================================================================
TEST(ParallelFivePathCapstone, CpcvDigestIdenticalAcrossFivePaths) {
  const AlphaStreams streams = make_streams(1, 64, 5, 0xC0FFEEULL);
  const std::vector<CpcvFold> folds = make_cpcv_folds(64);

  // PATH 1 — the sequential oracle (run_one_fold per fold in canonical order).
  const u64 want = cpcv_sequential_digest(streams, 0, folds);
  ASSERT_NE(want, u64{0}) << "non-vacuity: the cpcv oracle digest must be non-trivial";

  // PATHS 2..5 — every substrate leg from the table must match `want`.
  for (const SubstratePlan &p : kSubstratePlan) {
    std::unique_ptr<IExecutor> exec = make_executor(p);
    ASSERT_NE(exec, nullptr) << p.label;
    const u64 got = result_table_digest(parallel_cpcv(folds, streams, 0, kBook, *exec));
    EXPECT_EQ(got, want)
        << "FIVE-PATH BREAK [cpcv]: " << p.label << " diverged from the sequential oracle";
  }
}

// ===========================================================================
//  (A.4) MINE — five paths, one want on BOTH report.digest AND library version_id.
//
//  Each leg builds a FRESH factory fixture + a FRESH persistent library opened at
//  an identical empty start (same dir-content state, same seeds), so the only
//  moving part is the substrate + worker count. The admit fold runs SEQUENTIALLY in
//  the parent on every substrate (§0.9), so digest + version_id are invariant BY
//  CONSTRUCTION — this pins that they actually are.
// ===========================================================================
TEST(ParallelFivePathCapstone, MineReportDigestAndVersionIdIdenticalAcrossFivePaths) {
  // PATH 1 — the sequential single-process oracle (mine_into without an executor).
  u64 want_digest = 0;
  u64 want_version = 0;
  usize want_admitted = 0;
  {
    MineFixture fx{mine_panel()};
    lib::Library library = lib::Library::open(tmpdir("seq"), default_gate_cfg(), {0xC0FFEEu});
    AlphaGate gate{default_gate_cfg()};
    Factory f = fx.factory();
    const FactoryReport rep = f.mine_into(mine_cfg(/*seed*/ 7), library, gate).value();
    want_digest = rep.digest;
    want_admitted = rep.admitted;
    want_version = version_id_of(library);
  }
  // NON-VACUITY: a five-way equality of "admitted nothing" is meaningless. The
  // real-signal fixture must admit at least one alpha for the comparison to bite.
  ASSERT_GT(want_admitted, 0u) << "non-vacuity: the mine oracle must admit >0 alphas";

  // PATHS 2..5 — every substrate leg from the table must match `want` on BOTH the
  // report digest AND the resulting library version_id.
  for (const SubstratePlan &p : kSubstratePlan) {
    MineFixture fx{mine_panel()};
    lib::Library library =
        lib::Library::open(tmpdir(std::string(p.label)), default_gate_cfg(), {0xC0FFEEu});
    AlphaGate gate{default_gate_cfg()};
    Factory f = fx.factory();
    std::unique_ptr<IExecutor> exec = make_executor(p);
    ASSERT_NE(exec, nullptr) << p.label;
    const FactoryReport rep = f.mine_into(mine_cfg(/*seed*/ 7), library, gate, *exec).value();
    EXPECT_EQ(rep.digest, want_digest)
        << "FIVE-PATH BREAK [mine.digest]: " << p.label << " diverged from the sequential oracle";
    EXPECT_EQ(rep.admitted, want_admitted)
        << "FIVE-PATH BREAK [mine.admitted]: " << p.label;
    EXPECT_EQ(version_id_of(library), want_version)
        << "FIVE-PATH BREAK [mine.version_id]: " << p.label << " diverged from the oracle";
  }
}
