// atx::engine::parallel — S7.5d Factory::mine_into over the PROCESS boundary proof.
//
// THE SPRINT CAPSTONE (R1/§0.5/§0.9): Factory::mine_into runs REAL over the process
// boundary with a BYTE-IDENTICAL FactoryReport::digest (the search digest folded with
// every admission decision, incl. the library version_id) across ProcessExecutor@{1,N},
// ThreadExecutor@{1,N}, and the sequential single-process path. The admit fold stays
// SEQUENTIAL in the parent (it is stateful — AlphaId in admission order, segment_crc
// folds base_alpha_id; §0.4 partition-merge is UNSOUND, §0.9); ONLY the pure per-genome
// scoring map (compile+eval+extract_streams + pool-aware fitness vs. the run-start pool
// snapshot) crosses the seam via WorkloadId::Mine. This suite proves:
//
//   1. DIGEST + version_id IDENTITY — mine_into(sequential) == ThreadExecutor@{1,8} ==
//      ProcessExecutor@{1,4}, byte for byte on BOTH report.digest AND the resulting
//      library version_id. ALL legs asserted against one `want`.
//   2. GENOME ROUND-TRIP — serialize -> parse a genome yields the SAME compile+eval
//      streams AND the same pool-aware (dsr, raw) as the original (op-index remap +
//      re-analyze fidelity).
//   3. WORKER-COUNT INVARIANCE — Process@1 == Process@4 (digest + version_id).
//   4. PARSE REJECTION — truncated / bad-magic / op-index-OOR / dimension-overflow /
//      malformed-offset-table buffers are each rejected with Err (no crash, no OOB).
//
// The worker exe (atx-shm-worker) registers mine_shard in its own main; the test process
// only constructs ProcessExecutor (which spawns that exe).

#include <cstddef>
#include <cstdint>
#include <cstring> // std::memcpy (crafted-header overflow test)
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/store.hpp" // combine::AlphaStore (empty-pool oracle)

#include "atx/engine/factory/factory.hpp"
#include "atx/engine/factory/fitness.hpp"
#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/pool_view.hpp" // factory::AlphaStorePool (empty-pool oracle)

#include "atx/engine/library/library.hpp"

#include "atx/engine/parallel/executor.hpp"
#include "atx/engine/parallel/process_executor.hpp"
#include "atx/engine/parallel/thread_executor.hpp"
#include "atx/engine/parallel/workload_mine.hpp"

namespace {

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
using atx::engine::factory::Factory;
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::FactoryReport;
using atx::engine::factory::FitnessCfg;
using atx::engine::factory::Genome;
using atx::engine::factory::pool_aware_fitness;
using atx::engine::parallel::ExecutorConfig;
using atx::engine::parallel::InputView;
using atx::engine::parallel::kMineHeaderBytes;
using atx::engine::parallel::kMineMagic;
using atx::engine::parallel::MineGenomeResult;
using atx::engine::parallel::MineInputView;
using atx::engine::parallel::MineWorkItem;
using atx::engine::parallel::ProcessExecutor;
using atx::engine::parallel::score_one_genome;
using atx::engine::parallel::serialize_mine_input;
using atx::engine::parallel::ThreadExecutor;

namespace lib = atx::engine::library;
namespace factory = atx::engine::factory;
namespace alpha = atx::engine::alpha;

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

[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)",
          "rank(rev)",
          "ts_mean(close, 5)",
          "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))",
          "delta(close, 2)"};
}

[[nodiscard]] std::vector<std::string> panel_fields() { return {"close", "rev"}; }

[[nodiscard]] GateConfig default_gate_cfg() { return GateConfig{}; }

constexpr f64 kMinDsr = 0.5;

[[nodiscard]] FactoryConfig real_signal_cfg(u64 seed) {
  FactoryConfig cfg;
  cfg.search.master_seed = seed;
  cfg.search.population = 16;
  cfg.search.generations = 4;
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

[[nodiscard]] std::string tmpdir(const std::string &tag) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S75d") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s75d_mine" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

// The run-wide DSL library the genomes' op pointers alias (NOT the persistent library).
struct Fixture {
  Library lib{};
  Panel panel;
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  explicit Fixture(Panel p) : panel{std::move(p)} {}
  [[nodiscard]] Factory factory() { return Factory{lib, panel, sim, policy}; }
};

// The library version_id after a mine_into run (the L6/L7 content-address that folds
// every admitted alpha's canon_hash + segment_crc in admission order).
[[nodiscard]] u64 version_id_of(lib::Library &library) { return library.snapshot().version_id; }

} // namespace

// ===========================================================================
//  1. MINE — process@{1,4} == thread@{1,8} == sequential (digest + version_id).
// ===========================================================================
TEST(ParallelWorkloadMineProcess, MineReportDigestProcessEqualsThreadEqualsSequential) {
  // The sequential single-process oracle.
  u64 want_digest = 0;
  u64 want_version = 0;
  usize want_admitted = 0;
  {
    Fixture fx{real_signal_panel()};
    lib::Library library = lib::Library::open(tmpdir("seq"), default_gate_cfg(), {0xC0FFEEu});
    AlphaGate gate{default_gate_cfg()};
    Factory f = fx.factory();
    const FactoryReport rep = f.mine_into(real_signal_cfg(/*seed*/ 7), library, gate);
    want_digest = rep.digest;
    want_admitted = rep.admitted;
    want_version = version_id_of(library);
  }
  ASSERT_GT(want_admitted, 0u) << "the real-signal fixture must admit at least one alpha";

  // ThreadExecutor @ 1 and @ 8 (in-process substrate).
  for (const usize w : {usize{1}, usize{8}}) {
    Fixture fx{real_signal_panel()};
    lib::Library library =
        lib::Library::open(tmpdir("thr" + std::to_string(w)), default_gate_cfg(), {0xC0FFEEu});
    AlphaGate gate{default_gate_cfg()};
    Factory f = fx.factory();
    ThreadExecutor te{ExecutorConfig{w}};
    const FactoryReport rep = f.mine_into(real_signal_cfg(/*seed*/ 7), library, gate, te);
    EXPECT_EQ(rep.digest, want_digest) << "ThreadExecutor@" << w << " digest diverged";
    EXPECT_EQ(rep.admitted, want_admitted) << "ThreadExecutor@" << w << " admitted diverged";
    EXPECT_EQ(version_id_of(library), want_version)
        << "ThreadExecutor@" << w << " library version_id diverged";
  }

  // ProcessExecutor @ 1 and @ 4 (the cross-process legs — the whole point).
  for (const usize w : {usize{1}, usize{4}}) {
    Fixture fx{real_signal_panel()};
    lib::Library library =
        lib::Library::open(tmpdir("proc" + std::to_string(w)), default_gate_cfg(), {0xC0FFEEu});
    AlphaGate gate{default_gate_cfg()};
    Factory f = fx.factory();
    ProcessExecutor pe{ExecutorConfig{w, false}};
    const FactoryReport rep = f.mine_into(real_signal_cfg(/*seed*/ 7), library, gate, pe);
    EXPECT_EQ(rep.digest, want_digest) << "ProcessExecutor@" << w << " digest diverged";
    EXPECT_EQ(rep.admitted, want_admitted) << "ProcessExecutor@" << w << " admitted diverged";
    EXPECT_EQ(version_id_of(library), want_version)
        << "ProcessExecutor@" << w << " library version_id diverged";
  }
}

// ===========================================================================
//  3. WORKER-COUNT INVARIANCE — Process@1 == Process@4 (digest + version_id).
// ===========================================================================
TEST(ParallelWorkloadMineProcess, MineProcessWorkerCountInvariant) {
  Fixture fx1{real_signal_panel()};
  Fixture fx4{real_signal_panel()};
  lib::Library lib1 = lib::Library::open(tmpdir("w1"), default_gate_cfg(), {0xC0FFEEu});
  lib::Library lib4 = lib::Library::open(tmpdir("w4"), default_gate_cfg(), {0xC0FFEEu});
  AlphaGate gate{default_gate_cfg()};
  Factory f1 = fx1.factory();
  Factory f4 = fx4.factory();

  ProcessExecutor pe1{ExecutorConfig{1, false}};
  ProcessExecutor pe4{ExecutorConfig{4, false}};
  const FactoryReport a = f1.mine_into(real_signal_cfg(/*seed*/ 11), lib1, gate, pe1);
  const FactoryReport b = f4.mine_into(real_signal_cfg(/*seed*/ 11), lib4, gate, pe4);

  EXPECT_EQ(a.digest, b.digest) << "ProcessExecutor mine digest must be invariant 1 vs N workers";
  EXPECT_EQ(a.admitted, b.admitted);
  EXPECT_EQ(version_id_of(lib1), version_id_of(lib4))
      << "ProcessExecutor mine version_id must be invariant 1 vs N workers";
}

// ===========================================================================
//  5. OOS MINE — the Task-5 binding invariant. With oos_fraction > 0 the parallel
//     OOS path (Factory::mine_into_oos_parallel, two gather_mine_scores submits: one
//     panel=train for ranking, one panel=holdout for admission) MUST reproduce the
//     serial mine_into_oos BYTE-IDENTICALLY: report.digest, report.admitted, AND the
//     library version_id, across {sequential, ProcessExecutor@1, ProcessExecutor@4}.
//     If a digest/admit-count shifts, the parallel path is not reproducing the serial
//     eval — that is a BUG, not a golden to re-baseline.
// ===========================================================================
namespace {

// A real-signal config with the holdout branch ON (terminal 25% of dates held out).
// The default embargo + a 120-date panel leave a non-empty train AND holdout, so the
// search + admission have a real domain and the run admits at least one alpha.
[[nodiscard]] FactoryConfig oos_signal_cfg(u64 seed) {
  FactoryConfig cfg = real_signal_cfg(seed);
  cfg.oos_fraction = 0.25; // terminal holdout window (eval::reserve_lockbox geometry)
  return cfg;
}

} // namespace

TEST(ParallelWorkloadMineProcess, OosMineReportDigestProcessEqualsSequential) {
  // The sequential single-process OOS oracle (Factory::mine_into_oos via the no-executor
  // mine_into overload when oos_fraction > 0).
  u64 want_digest = 0;
  u64 want_version = 0;
  usize want_admitted = 0;
  {
    Fixture fx{real_signal_panel()};
    lib::Library library = lib::Library::open(tmpdir("oos_seq"), default_gate_cfg(), {0xC0FFEEu});
    AlphaGate gate{default_gate_cfg()};
    Factory f = fx.factory();
    const FactoryReport rep = f.mine_into(oos_signal_cfg(/*seed*/ 7), library, gate);
    want_digest = rep.digest;
    want_admitted = rep.admitted;
    want_version = version_id_of(library);
  }
  ASSERT_GT(want_admitted, 0u) << "the OOS real-signal fixture must admit at least one alpha";

  // ProcessExecutor @ 1 and @ 4 — the parallel two-submit OOS path. Each MUST reproduce
  // the serial OOS digest / admitted / version_id byte-for-byte (the binding invariant).
  for (const usize w : {usize{1}, usize{4}}) {
    Fixture fx{real_signal_panel()};
    lib::Library library =
        lib::Library::open(tmpdir("oos_proc" + std::to_string(w)), default_gate_cfg(), {0xC0FFEEu});
    AlphaGate gate{default_gate_cfg()};
    Factory f = fx.factory();
    ProcessExecutor pe{ExecutorConfig{w, false}};
    const FactoryReport rep = f.mine_into(oos_signal_cfg(/*seed*/ 7), library, gate, pe);
    EXPECT_EQ(rep.digest, want_digest) << "OOS ProcessExecutor@" << w << " digest diverged";
    EXPECT_EQ(rep.admitted, want_admitted) << "OOS ProcessExecutor@" << w << " admitted diverged";
    EXPECT_EQ(version_id_of(library), want_version)
        << "OOS ProcessExecutor@" << w << " library version_id diverged";
  }
}

// Worker-count invariance for the OOS path (Process@1 == Process@4), independent seed.
TEST(ParallelWorkloadMineProcess, OosMineProcessWorkerCountInvariant) {
  Fixture fx1{real_signal_panel()};
  Fixture fx4{real_signal_panel()};
  lib::Library lib1 = lib::Library::open(tmpdir("oos_w1"), default_gate_cfg(), {0xC0FFEEu});
  lib::Library lib4 = lib::Library::open(tmpdir("oos_w4"), default_gate_cfg(), {0xC0FFEEu});
  AlphaGate gate{default_gate_cfg()};
  Factory f1 = fx1.factory();
  Factory f4 = fx4.factory();

  ProcessExecutor pe1{ExecutorConfig{1, false}};
  ProcessExecutor pe4{ExecutorConfig{4, false}};
  const FactoryReport a = f1.mine_into(oos_signal_cfg(/*seed*/ 11), lib1, gate, pe1);
  const FactoryReport b = f4.mine_into(oos_signal_cfg(/*seed*/ 11), lib4, gate, pe4);

  EXPECT_EQ(a.digest, b.digest)
      << "OOS ProcessExecutor mine digest must be invariant 1 vs N workers";
  EXPECT_EQ(a.admitted, b.admitted);
  EXPECT_EQ(version_id_of(lib1), version_id_of(lib4))
      << "OOS ProcessExecutor mine version_id must be invariant 1 vs N workers";
}

// ===========================================================================
//  2. GENOME ROUND-TRIP — serialize -> parse a genome yields the SAME compile+eval
//     streams AND the same pool-aware (dsr, raw) as the original (op-index remap +
//     re-analyze fidelity proof). Exercised through score_one_genome with an EMPTY
//     run-start pool (worst_corr == 0), so the (dsr, raw) are well-defined.
// ===========================================================================
TEST(ParallelWorkloadMineProcess, GenomeRoundTripsThroughSerializeParse) {
  Fixture fx{real_signal_panel()};

  // Build a handful of distinct in-grammar genomes (parse + analyze) the same way the
  // search produces them; each carries Call ops (rank / ts_mean / delta) so the op-name
  // remap is exercised, plus literals + field leaves.
  const std::vector<std::string> exprs = {"rank(close)", "ts_mean(close, 5)",
                                          "rank(rev) - rank(close)", "delta(close, 2) + 1.5"};
  std::vector<Genome> genomes;
  for (const std::string &e : exprs) {
    auto ast = parse_expr(e, fx.lib);
    ASSERT_TRUE(ast.has_value()) << e << ": " << (ast ? "" : ast.error().message());
    auto ana = analyze(*ast);
    ASSERT_TRUE(ana.has_value()) << e << ": " << (ana ? "" : ana.error().message());
    genomes.push_back(Genome{std::move(*ast), std::move(*ana), 0});
  }

  // Empty run-start pool snapshot (worst_corr == 0 for every candidate).
  MineWorkItem pool;
  pool.pool_n_alphas = 0;
  pool.n_periods = 0;
  pool.pool_seed = 0xC0FFEEu;

  FitnessCfg fit;
  fit.trial_count = 4;
  fit.book_size = 1.0;

  const std::vector<std::byte> buf = serialize_mine_input(std::span<const Genome>{genomes}, pool,
                                                          fx.panel, fit, fx.policy, fx.sim);
  const auto parsed = MineInputView::parse(InputView{std::span<const std::byte>{buf}});
  ASSERT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  const MineInputView &v = parsed.value();
  ASSERT_EQ(v.n_genomes(), genomes.size());

  for (usize k = 0; k < genomes.size(); ++k) {
    // (a) the rebuilt genome compiles+evals to the SAME streams as the in-process oracle.
    // detail_eval_streams is private, so reproduce its streams via the public in-process
    // score (the same built-in Library resolves the serialized op names).
    const auto scored = score_one_genome(v, k, fx.lib);
    ASSERT_TRUE(scored.has_value())
        << "genome " << k << ": " << (scored ? "" : scored.error().message());
    const MineGenomeResult &res = scored.value();
    ASSERT_EQ(res.ok, 1u) << "genome " << k << " must compile+score";

    // Oracle streams via a direct compile+eval+extract over the ORIGINAL genome.
    const Genome &orig = genomes[k];
    // pool_aware_fitness over an empty pool gives the oracle (dsr, raw); the rebuilt
    // genome must reproduce them bit-for-bit (re-analyze + op-remap fidelity).
    auto empty_pool = factory::AlphaStorePool{atx::engine::combine::AlphaStore{}};
    auto oracle_fit = pool_aware_fitness(orig, empty_pool, fx.panel, fx.policy, fx.sim, fit);
    ASSERT_TRUE(oracle_fit.has_value()) << "genome " << k << " oracle fitness";
    EXPECT_EQ(res.dsr, oracle_fit->dsr) << "genome " << k << " dsr must round-trip exactly";
    EXPECT_EQ(res.raw, oracle_fit->raw) << "genome " << k << " raw must round-trip exactly";

    // The streams themselves: compile+eval the ORIGINAL and compare PnL + positions.
    auto rebuilt = v.genome(k, fx.lib);
    ASSERT_TRUE(rebuilt.has_value()) << "genome " << k << " must re-parse";
    // Same canon_hash carried verbatim.
    EXPECT_EQ(rebuilt->canon_hash, orig.canon_hash) << "genome " << k << " canon_hash verbatim";
    // Realized streams equal byte-for-byte (the gathered slot decode produces these).
    const atx::engine::alpha::AlphaStreams &got = res.streams;
    // Recompute the oracle streams from the original genome directly.
    {
      using atx::engine::alpha::compile;
      using atx::engine::alpha::Engine;
      using atx::engine::alpha::extract_streams;
      auto prog = compile(orig.ast, orig.analysis);
      ASSERT_TRUE(prog.has_value());
      Engine engine{fx.panel};
      auto ss = engine.evaluate(*prog);
      ASSERT_TRUE(ss.has_value());
      auto strm = extract_streams(*ss, fx.policy, fx.panel, fx.sim);
      ASSERT_TRUE(strm.has_value());
      ASSERT_EQ(got.n_periods(), strm->n_periods());
      ASSERT_EQ(got.n_instruments(), strm->n_instruments());
      const std::span<const f64> a = got.pnl(0);
      const std::span<const f64> b = strm->pnl(0);
      ASSERT_EQ(a.size(), b.size());
      for (usize t = 0; t < a.size(); ++t) {
        EXPECT_EQ(a[t], b[t]) << "genome " << k << " pnl[" << t << "] diverged";
      }
    }
  }
}

// ===========================================================================
//  4. PARSE REJECTION — untrusted bytes -> Err, no crash / OOB.
// ===========================================================================
namespace {

// Build a valid serialized mine buffer (one genome, empty pool) to mutate.
[[nodiscard]] std::vector<std::byte> valid_mine_buffer() {
  Library dsl;
  auto ast = parse_expr("rank(close)", dsl);
  EXPECT_TRUE(ast.has_value());
  auto ana = analyze(*ast);
  EXPECT_TRUE(ana.has_value());
  std::vector<Genome> genomes;
  genomes.push_back(Genome{std::move(*ast), std::move(*ana), 0});

  const Panel panel = two_field_panel(20, 4, momentum_close(20, 4, 0x1234u));
  WeightPolicy policy;
  ExecutionSimulator sim = frictionless_sim();
  MineWorkItem pool;
  pool.pool_n_alphas = 0;
  pool.n_periods = 0;
  pool.pool_seed = 1;
  FitnessCfg fit;
  fit.trial_count = 2;
  return serialize_mine_input(std::span<const Genome>{genomes}, pool, panel, fit, policy, sim);
}

} // namespace

TEST(ParallelWorkloadMineProcess, ParseRejectsTruncatedBuffer) {
  const std::vector<std::byte> ok = valid_mine_buffer();
  ASSERT_FALSE(ok.empty());
  // A buffer shorter than the header is rejected (no OOB header read).
  std::vector<std::byte> trunc(ok.begin(), ok.begin() + (kMineHeaderBytes / 2));
  EXPECT_FALSE(MineInputView::parse(InputView{std::span<const std::byte>{trunc}}).has_value());
  // A buffer cut mid-genome (header + a few bytes) is rejected too.
  std::vector<std::byte> mid(ok.begin(), ok.begin() + (kMineHeaderBytes + 8));
  EXPECT_FALSE(MineInputView::parse(InputView{std::span<const std::byte>{mid}}).has_value());
}

TEST(ParallelWorkloadMineProcess, ParseRejectsBadMagic) {
  std::vector<std::byte> bad = valid_mine_buffer();
  ASSERT_GE(bad.size(), 4u);
  const atx::u32 wrong = kMineMagic ^ 0xFFFFFFFFU;
  std::memcpy(bad.data(), &wrong, sizeof(wrong));
  EXPECT_FALSE(MineInputView::parse(InputView{std::span<const std::byte>{bad}}).has_value());
}

TEST(ParallelWorkloadMineProcess, ParseRejectsDimensionOverflow) {
  std::vector<std::byte> bad = valid_mine_buffer();
  // Set pool_n_alphas + n_periods to values whose product overflows usize (checked_mul
  // must reject the pool snapshot region rather than form a wrapped-small alias).
  const atx::u32 huge = 0xFFFFFFFFU;
  std::memcpy(bad.data() + 8, &huge, sizeof(huge));  // n_periods
  std::memcpy(bad.data() + 12, &huge, sizeof(huge)); // pool_n_alphas
  EXPECT_FALSE(MineInputView::parse(InputView{std::span<const std::byte>{bad}}).has_value());
}

TEST(ParallelWorkloadMineProcess, ParseRejectsGenomeCountOverflow) {
  std::vector<std::byte> bad = valid_mine_buffer();
  // A bogus n_genomes makes the offset-table region overrun the buffer -> Err (no OOB).
  const atx::u32 huge = 0x10000000U;                // (n+1)*8 overruns any realistic buffer
  std::memcpy(bad.data() + 4, &huge, sizeof(huge)); // n_genomes
  EXPECT_FALSE(MineInputView::parse(InputView{std::span<const std::byte>{bad}}).has_value());
}

TEST(ParallelWorkloadMineProcess, ParseRejectsUnknownOpName) {
  // A genome whose serialized Call op-name is not in the worker's Library must be rejected
  // by genome() (untrusted bytes -> the lib.find == nullptr branch), not crash. The valid
  // buffer's single genome is `rank(close)`: its op-name dictionary is the 4-byte-prefixed
  // ASCII string "rank" near the buffer tail. Corrupt those bytes to a non-existent op
  // ("XXXX") and assert genome() Errs; the parse itself (structure-only) still succeeds.
  std::vector<std::byte> bad = valid_mine_buffer();
  const auto parsed = MineInputView::parse(InputView{std::span<const std::byte>{bad}});
  ASSERT_TRUE(parsed.has_value());
  Library dsl;
  ASSERT_TRUE(parsed->genome(0, dsl).has_value()); // positive: "rank" resolves

  // Find the ASCII "rank" op-name bytes and clobber them to a non-existent op.
  const char *needle = "rank";
  bool clobbered = false;
  for (std::size_t i = 0; i + 4 <= bad.size(); ++i) {
    if (std::memcmp(bad.data() + i, needle, 4) == 0) {
      const char repl[4] = {'X', 'X', 'X', 'X'};
      std::memcpy(bad.data() + i, repl, 4);
      clobbered = true;
      break;
    }
  }
  ASSERT_TRUE(clobbered) << "expected the op-name 'rank' in the serialized buffer";
  const auto reparsed = MineInputView::parse(InputView{std::span<const std::byte>{bad}});
  ASSERT_TRUE(reparsed.has_value()); // structure still parses (the op-name is genome-local)
  // genome() now hits lib.find("XXXX") == nullptr -> Err (no crash, no OOB).
  EXPECT_FALSE(reparsed->genome(0, dsl).has_value());
  // An out-of-range genome index is also rejected.
  EXPECT_FALSE(reparsed->genome(99, dsl).has_value());
}
