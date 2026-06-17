// atx::engine — guarded end-to-end smoke test: ORATS history Panel through the
// unchanged p2 RobustResearchDriver (p3 S3-6).
//
// Suite: OratsE2ESmoke
//
// This test probes for the on-disk ORATS partition (data/orats_history_1d).  In
// the committed CI state the partition is gitignored/absent, so the test
// COMPILES, LINKS, and SKIPS cleanly.  When an operator runs the S3-2 data-build
// the partition is present and the test drives the UNCHANGED p2 pipeline over the
// real Panel — proving that factory::RobustResearchDriver accepts a
// build_history_panel result without modification to any production source.
//
// Field-name alignment note:
//   The existing robust_pipeline_e2e_test.cpp panel uses fields {"close", "rev"}.
//   "rev" does NOT exist in the history Panel.  The history Panel exposes exactly:
//     close, raw_close, volume, high, low, open, market_cap, sector
//   (kHistField* constants in history_panel.hpp).
//   This smoke therefore authors its own alpha DSL referencing ONLY "close" and
//   "volume" — both confirmed present in the history Panel — with momentum and
//   reversal expressions.  The library's panel_fields is set to {"close", "volume"}
//   to match.

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"    // alpha::Library (DSL)
#include "atx/engine/combine/gate.hpp"      // combine::AlphaGate, GateConfig
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/factory.hpp"         // factory::FactoryConfig
#include "atx/engine/factory/robust_pipeline.hpp" // factory::RobustResearchDriver, RobustPipelineConfig
#include "atx/engine/library/library.hpp"         // library::Library
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/data/history_panel.hpp" // data::build_history_panel, HistoryDataConfig
#include "atx/engine/data/orats_history.hpp" // data::detail::date_to_nanos

namespace atxtest_orats_e2e_smoke {

namespace fs = std::filesystem;

using atx::f64;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
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
using atx::engine::factory::RobustPipelineConfig;
using atx::engine::factory::RobustReport;
using atx::engine::factory::RobustResearchDriver;

namespace lib = atx::engine::library;

namespace {

// ---------------------------------------------------------------------------
// ORATS partition probe — mirrors data_real_panel_e2e_test.cpp:find_data_root().
// Probes for "data/orats_history_1d" instead of the databento/master pair.
// ---------------------------------------------------------------------------
constexpr const char *kOratsProbe = "data/orats_history_1d";

[[nodiscard]] std::optional<fs::path> probe_path(const fs::path &root, const char *rel) {
  std::error_code ec;
  const fs::path candidate = root / rel;
  if (fs::exists(candidate, ec)) {
    return candidate;
  }
  return std::nullopt;
}

// Read a worktree's .git file to get the main repo root (parent^3 of the
// gitdir).  Returns std::nullopt if `ancestor/.git` is not a file or is
// malformed.
[[nodiscard]] std::optional<fs::path> main_root_from_worktree(const fs::path &ancestor) {
  std::error_code ec;
  const fs::path git_marker = ancestor / ".git";
  if (!fs::is_regular_file(git_marker, ec)) {
    return std::nullopt;
  }
  std::ifstream in(git_marker);
  std::string line;
  if (!std::getline(in, line)) {
    return std::nullopt;
  }
  const std::string prefix = "gitdir:";
  if (line.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  std::string gitdir = line.substr(prefix.size());
  while (!gitdir.empty() && (gitdir.front() == ' ' || gitdir.front() == '\t')) {
    gitdir.erase(gitdir.begin());
  }
  // gitdir = <mainroot>/.git/worktrees/<name>  ->  mainroot = parent^3.
  fs::path p(gitdir);
  return p.parent_path().parent_path().parent_path();
}

[[nodiscard]] bool read_env(const char *name, std::string &out) {
  char *buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) {
    return false;
  }
  out.assign(buf);
  std::free(buf);
  return true;
}

// Resolve the repo root (the dir CONTAINING "data/").  Tries ATX_DATA_DIR's
// parent, then walks up from __FILE__ (including the worktree main root), then
// the cwd — looking for "data/orats_history_1d".
[[nodiscard]] std::optional<fs::path> find_orats_root() {
  if (std::string env; read_env("ATX_DATA_DIR", env)) {
    const fs::path root = fs::path(env).parent_path();
    if (probe_path(root, kOratsProbe)) {
      return root;
    }
  }
  for (fs::path dir = fs::path(__FILE__).parent_path(); !dir.empty();
       dir = dir.parent_path()) {
    if (probe_path(dir, kOratsProbe)) {
      return dir;
    }
    if (auto root = main_root_from_worktree(dir)) {
      if (probe_path(*root, kOratsProbe)) {
        return *root;
      }
    }
    if (dir == dir.root_path()) {
      break;
    }
  }
  std::error_code ec;
  for (fs::path dir = fs::current_path(ec); !dir.empty(); dir = dir.parent_path()) {
    if (probe_path(dir, kOratsProbe)) {
      return dir;
    }
    if (dir == dir.root_path()) {
      break;
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Shared builders (mirrored from robust_pipeline_e2e_test.cpp).
// ---------------------------------------------------------------------------

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

[[nodiscard]] GateConfig default_gate_cfg() { return GateConfig{}; }

constexpr f64 kMinDsr = 0.80;
constexpr u64 kLibSeed = 0xC0FFEEu;

// Alpha expressions that reference ONLY "close" and "volume" — both confirmed
// present in the history Panel (kHistFieldClose, kHistFieldVolume in
// history_panel.hpp).  Simple momentum / reversal / volume-scaled signals.
[[nodiscard]] std::vector<std::string> orats_seed_exprs() {
  return {
      "rank(close)",
      "ts_mean(close, 5)",
      "delta(close, 2)",
      "rank(ts_mean(close, 10))",
      "rank(volume)",
      "ts_mean(volume, 5)",
  };
}

// Panel fields visible to the GA — MUST match fields in the history Panel.
[[nodiscard]] std::vector<std::string> orats_panel_fields() {
  return {"close", "volume"};
}

// Per-run factory config with a small budget for a time-boxed smoke.
[[nodiscard]] FactoryConfig smoke_per_run_cfg() {
  FactoryConfig cfg;
  cfg.search.master_seed = 0; // overwritten per-run by detail::seed_for_run
  cfg.search.population = 8;
  cfg.search.generations = 2;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.search.novelty_w = 0.0;         // OFF for the smoke
  cfg.search.fitness.trial_count = 2; // minimal
  cfg.seed_exprs = orats_seed_exprs();
  cfg.panel_fields = orats_panel_fields();
  cfg.min_dsr = kMinDsr;
  return cfg;
}

// Collapsed (ScalarRaw, novelty off, cost off, robustness gate off) pipeline
// config with a tiny budget — the same "collapsed" shape used by
// CollapsedPipelinePinsToPlainDriver in the reference test.
[[nodiscard]] RobustPipelineConfig smoke_pipeline_cfg() {
  RobustPipelineConfig cfg;
  cfg.research.per_run = smoke_per_run_cfg();
  cfg.research.per_run.search.objective_mode =
      atx::engine::factory::ObjectiveMode::ScalarRaw;
  cfg.research.per_run.search.novelty_w = 0.0;
  cfg.research.per_run.search.fitness.target_aum = 0.0;
  cfg.research.per_run.search.n_workers = 1;
  cfg.research.max_runs = 2;
  cfg.research.patience = 0;
  cfg.research.master_seed = 0xDEADBEEFu;
  cfg.research.robustness_gate = false; // gate OFF (collapsed / smoke path)
  cfg.lockbox_frac = 0.20;
  cfg.embargo_len = 0;
  cfg.book.single.gross_leverage = 1.0;
  cfg.book.single.max_iters = 16;
  cfg.book.trade_rate = 1.0;
  cfg.cost.kappa = 0.0;
  cfg.cost.round_trip_cost_bps = 0.0;
  cfg.cost.capacity_gross = 1e9;
  return cfg;
}

// A per-test temp directory for the persistent library.
[[nodiscard]] std::string smoke_tmpdir() {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_orats_e2e_smoke";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  (void)ec;
  std::filesystem::create_directories(dir, ec);
  (void)ec;
  return dir.string();
}

} // namespace

// =============================================================================
//  OratsE2ESmoke — GUARDED real-data smoke.
//
//  SKIPS when data/orats_history_1d is absent (the committed CI state).
//  When the partition is present, assembles a HistoryPanel over a small
//  2020-Q1 window and drives the UNCHANGED RobustResearchDriver over it.
// =============================================================================
TEST(OratsE2ESmoke, RealPartitionRunsUnchangedRobustPipeline) {
  // ---- probe for the partition -----------------------------------------------
  auto root = find_orats_root();
  if (!root) {
    GTEST_SKIP() << "ORATS partition (data/orats_history_1d) not built; "
                    "skipping (operator data-build step S3-2 required)";
  }
  const std::string seg_dir = (*root / "data" / "orats_history_1d").string();

  // ---- assemble the history Panel (S3-5) -------------------------------------
  // 2020-Q1 window — small enough for a time-boxed smoke.  Bounds come from the
  // loader's own date_to_nanos helper (midnight-UTC) so they stay self-documenting.
  const auto start_ns = atx::engine::data::detail::date_to_nanos("2020-01-02");
  const auto end_ns   = atx::engine::data::detail::date_to_nanos("2020-04-01");
  ASSERT_TRUE(start_ns.has_value()) << "date_to_nanos(2020-01-02) failed";
  ASSERT_TRUE(end_ns.has_value())   << "date_to_nanos(2020-04-01) failed";

  atx::engine::data::HistoryDataConfig hcfg;
  hcfg.seg_dir = seg_dir;
  hcfg.window.start_nanos = *start_ns;
  hcfg.window.end_nanos   = *end_ns;
  hcfg.universe.min_adv_usd     = 5.0e6; // liquid subset
  hcfg.universe.top_n_by_adv    = 200;   // cap for time-boxing

  auto hp = atx::engine::data::build_history_panel(hcfg);
  ASSERT_TRUE(hp.has_value()) << "build_history_panel failed: " << hp.error().to_string();
  EXPECT_GT(hp->panel.instruments(), 0u) << "history Panel must contain at least one instrument";
  EXPECT_GT(hp->panel.dates(), 0u) << "history Panel must contain at least one date";

  // Confirm the fields this smoke's alpha expressions depend on are present.
  EXPECT_TRUE(hp->panel.field_id(atx::engine::data::kHistFieldClose).has_value())
      << "Panel missing 'close' field";
  EXPECT_TRUE(hp->panel.field_id(atx::engine::data::kHistFieldVolume).has_value())
      << "Panel missing 'volume' field";

  // ---- drive the UNCHANGED p2 RobustResearchDriver ---------------------------
  // Construction mirrors robust_pipeline_e2e_test.cpp verbatim.  The ONLY
  // substitution: hp->panel (the real history Panel) instead of a synthetic panel.
  Library dsl{};
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  AlphaGate gate{default_gate_cfg()};
  lib::Library library = lib::Library::open(smoke_tmpdir(), default_gate_cfg(), {kLibSeed});

  RobustResearchDriver driver{library, dsl, hp->panel, sim, policy, gate};

  const RobustPipelineConfig pcfg = smoke_pipeline_cfg();
  auto report = driver.run(pcfg);
  ASSERT_TRUE(report.has_value()) << "RobustResearchDriver::run failed: "
                                   << report.error().to_string();

  // The pipeline ran end-to-end.  A non-zero digest confirms the inner
  // ResearchDriver actually executed its mine/gate/admit loop.
  EXPECT_NE(report->research.digest, 0u) << "pipeline returned a zero digest (did not run?)";

  // Log a brief summary for the operator's ledger record.  GTEST_LOG_(INFO)
  // keeps test output pristine (a tagged info line, not raw stdout) when an
  // operator runs this with the real partition present.
  GTEST_LOG_(INFO) << "[OratsE2ESmoke] instruments=" << hp->panel.instruments()
                   << " dates=" << hp->panel.dates()
                   << " admitted=" << report->research.total_admitted
                   << " robust_size=" << report->robust_size
                   << " book.ran=" << report->book.ran
                   << " digest=0x" << std::hex << report->research.digest << std::dec;
}

} // namespace atxtest_orats_e2e_smoke
