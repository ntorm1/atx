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
#include "atx/engine/data/orats_history.hpp" // data::detail::date_to_nanos, kOratsFields

#include "atx/tsdb/load_parquet.hpp" // atx::tsdb::build_from_long, LongColumns

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

// ---------------------------------------------------------------------------
// Synthetic partition helper — mirrors data_history_panel_test.cpp::write_day.
// Writes one date's .seg file with all 16 ORATS fields for `n_instr` symbols
// in `dir`.  Only indices 3 (close), 6 (volume), 7 (shares), and 10
// (cumReturnFactor) are filled; the rest are zero (NaN would prevent market-cap
// computation so we use 0.0 for unused fields).
//
// Symbols are "10001".."10000+n_instr" (numeric securityID-style strings).
// close[i]   = base_close + i (monotone, different per instrument).
// cumret[i]  = 1.0 (so TRI = close — no split adjustment needed).
// shares     = 2e8 (200M shares; market_cap = close*shares >> $1M ADV floor).
// volume     = 1e6 (1M shares/day; dollar_volume = close*volume >> $1M ADV).
// ---------------------------------------------------------------------------
void write_seg_day(const fs::path &dir, const std::string &name, atx::i64 dn,
                   int n_instr, atx::f64 base_close) {
  const auto r = static_cast<atx::usize>(n_instr);
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(atx::engine::data::kOratsFields.begin(),
                           atx::engine::data::kOratsFields.end());
  cols.times.assign(r, dn);
  cols.symbols.reserve(r);
  for (int i = 0; i < n_instr; ++i) {
    cols.symbols.push_back(std::to_string(10001 + i));
  }
  cols.values.assign(atx::engine::data::kOratsFields.size(),
                      std::vector<atx::f64>(r, 0.0));
  // idx 3 = close
  for (int i = 0; i < n_instr; ++i) {
    cols.values[3][static_cast<atx::usize>(i)] = base_close + static_cast<atx::f64>(i);
  }
  // idx 6 = volume (1M shares/day -> dollar_volume >> ADV floor)
  cols.values[6].assign(r, 1.0e6);
  // idx 7 = shares (200M -> market_cap >> floor)
  cols.values[7].assign(r, 2.0e8);
  // idx 10 = cumReturnFactor = 1.0 (TRI = raw close)
  cols.values[10].assign(r, 1.0);
  const auto ok = atx::tsdb::build_from_long(cols, (dir / name).string(), 0);
  ASSERT_TRUE(ok.has_value()) << "write_seg_day failed for " << name;
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

// =============================================================================
//  OratsE2ESmoke — SYNTHETIC always-run integration test.
//
//  Writes a fully synthetic per-date .seg partition to a temp dir (60 dates x
//  30 instruments), builds a HistoryPanel, then drives the UNCHANGED
//  RobustResearchDriver over it.  ALWAYS runs in CI (no data dependency).
//
//  The purpose of this test is to prove the build_history_panel -> RobustResearch
//  Driver::run driver path executes end-to-end on a Panel assembled by the ORATS
//  segment-builder pipeline, without modifying any production source.
//
//  Synthetic partition parameters chosen to clear the pipeline's minimum-size
//  requirements:
//    * 60 trading dates  — covers the longest ts_mean(close, 10) lookback (10d)
//      plus the lockbox (20% = 12d) and leaves >= 48 visible dates.
//    * 30 instruments    — exceeds the population (8) and provides meaningful
//      cross-sectional rank signal.
//    * close paths: monotone (base+i), cumReturnFactor=1.0 -> TRI = raw close.
//    * volume = 1e6 shares/day, shares = 2e8 -> dollar_volume >> $1M ADV floor
//      so build_universe admits all 30 instruments.
//    * min_adv_usd lowered to 0 to guarantee all instruments pass the screen
//      regardless of the exact causal ADV window fill.
// =============================================================================
TEST(OratsE2ESmoke, SyntheticPartitionRunsUnchangedRobustPipeline) {
  // ---- build synthetic temp partition ----------------------------------------
  const fs::path seg_dir = fs::temp_directory_path() / "atx_orats_e2e_synth_seg";
  {
    std::error_code ec;
    fs::remove_all(seg_dir, ec);
    fs::create_directories(seg_dir, ec);
  }

  // 60 trading dates starting from 2020-01-02 (day 18263 in unix-day offset).
  // Each date advances by 1 day (synthetic calendar, no weekend gaps needed).
  constexpr int kDates = 60;
  constexpr int kInstr = 30;
  const atx::i64 kDayNanos = 86400LL * 1'000'000'000LL;
  const atx::i64 kDay0 = 18263LL * kDayNanos; // 2020-01-02 midnight UTC

  for (int d = 0; d < kDates; ++d) {
    const atx::i64 dn = kDay0 + static_cast<atx::i64>(d) * kDayNanos;
    // Encode date as "YYYY-MM-DD" via date_to_nanos inverse — we synthesize
    // numeric names directly.  Use a fixed-format string from an offset we know:
    // 2020-01-02 + d days.  The filename only needs to parse via build_from_long;
    // use a simple incrementing label.
    // day_str: synthesize "2020-01-DD" style — but build_history_panel reads the
    // .seg's embedded timestamp, not the filename, for date ordering.  Filename
    // just needs to be unique; use "seg_NNN.seg".
    const std::string fname = "seg_" + std::to_string(1000 + d) + ".seg";
    // close[i] = 50 + i + d * 0.1  (slowly monotone upward — non-trivial signal)
    const atx::f64 base = 50.0 + static_cast<atx::f64>(d) * 0.1;
    write_seg_day(seg_dir, fname, dn, kInstr, base);
  }

  // ---- build history Panel ---------------------------------------------------
  atx::engine::data::HistoryDataConfig hcfg;
  hcfg.seg_dir = seg_dir.string();
  // Default TimeWindow{} == [min, max) == all dates — no restriction needed.
  hcfg.universe.min_adv_usd  = 0.0;  // admit all synthetic instruments
  hcfg.universe.adv_window   = 5;    // short enough for a 60-date panel
  hcfg.universe.top_n_by_adv = 0;    // no count cap

  auto hp = atx::engine::data::build_history_panel(hcfg);
  ASSERT_TRUE(hp.has_value()) << "build_history_panel (synthetic) failed: "
                               << hp.error().to_string();
  EXPECT_GT(hp->panel.instruments(), 0u)
      << "synthetic Panel must contain at least one instrument";
  EXPECT_GT(hp->panel.dates(), 0u)
      << "synthetic Panel must contain at least one date";

  // ---- drive the UNCHANGED p2 RobustResearchDriver ---------------------------
  const fs::path lib_dir = fs::temp_directory_path() / "atx_orats_e2e_synth_lib";
  {
    std::error_code ec;
    fs::remove_all(lib_dir, ec);
    fs::create_directories(lib_dir, ec);
  }

  Library dsl{};
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  AlphaGate gate{default_gate_cfg()};
  lib::Library library = lib::Library::open(lib_dir.string(), default_gate_cfg(), {kLibSeed});

  RobustResearchDriver driver{library, dsl, hp->panel, sim, policy, gate};
  const RobustPipelineConfig pcfg = smoke_pipeline_cfg();
  auto report = driver.run(pcfg);
  ASSERT_TRUE(report.has_value()) << "RobustResearchDriver::run (synthetic) failed: "
                                   << report.error().to_string();

  // A non-zero digest confirms the inner ResearchDriver actually executed the
  // mine/gate/admit loop (not just that run() returned an empty result).
  EXPECT_NE(report->research.digest, 0u)
      << "synthetic pipeline returned a zero digest (inner loop did not execute?)";

  GTEST_LOG_(INFO) << "[OratsE2ESmoke/Synthetic] instruments=" << hp->panel.instruments()
                   << " dates=" << hp->panel.dates()
                   << " admitted=" << report->research.total_admitted
                   << " robust_size=" << report->robust_size
                   << " book.ran=" << report->book.ran
                   << " digest=0x" << std::hex << report->research.digest << std::dec;

  // ---- clean up temp dirs ---------------------------------------------------
  {
    std::error_code ec;
    fs::remove_all(seg_dir, ec);
    fs::remove_all(lib_dir, ec);
  }
}

// =============================================================================
//  OratsE2ESmoke — OPERATOR test for load_orats_history.
//
//  Guarded by the ATX_ORATS_ZIP env var.  SKIPS cleanly when unset (the CI
//  state — the 11GB zip is never present in CI).  When set, loads the zip,
//  writes the per-date .seg partition, then runs build_history_panel + the
//  UNCHANGED RobustResearchDriver over a small window, and logs OratsLoadStats +
//  the smoke digest via GTEST_LOG_(INFO).
//
//  Usage (operator, one env var):
//    ATX_ORATS_ZIP=/path/to/tbltickerhistory3_10y.zip ctest -R OratsE2ESmoke
// =============================================================================
TEST(OratsE2ESmoke, OperatorOratsZip) {
  std::string zip_path;
  if (!read_env("ATX_ORATS_ZIP", zip_path) || zip_path.empty()) {
    GTEST_SKIP() << "ATX_ORATS_ZIP not set; skipping operator ORATS load "
                    "(set env var to the zip path to run)";
  }

  {
    std::error_code ec;
    if (!fs::exists(zip_path, ec)) {
      GTEST_SKIP() << "ATX_ORATS_ZIP=" << zip_path << " does not exist; skipping";
    }
  }

  // Write to a temp partition dir (or the operator's data dir if ATX_DATA_DIR is set).
  fs::path out_dir;
  if (std::string data_dir; read_env("ATX_DATA_DIR", data_dir) && !data_dir.empty()) {
    out_dir = fs::path(data_dir) / "orats_history_1d";
  } else {
    out_dir = fs::temp_directory_path() / "atx_orats_zip_out";
  }

  const auto min_date = atx::engine::data::detail::date_to_nanos("2020-01-01");
  ASSERT_TRUE(min_date.has_value()) << "date_to_nanos(2020-01-01) failed";

  atx::engine::data::OratsLoadConfig load_cfg;
  load_cfg.zip_path         = zip_path;
  load_cfg.out_dir          = out_dir.string();
  load_cfg.min_date_nanos   = *min_date;
  load_cfg.created_at_nanos = 0;

  auto stats_res = atx::engine::data::load_orats_history(load_cfg);
  ASSERT_TRUE(stats_res.has_value()) << "load_orats_history failed: "
                                      << stats_res.error().to_string();
  const auto &stats = *stats_res;

  GTEST_LOG_(INFO) << "[OperatorOratsZip] rows_read=" << stats.rows_read
                   << " rows_kept=" << stats.rows_kept
                   << " rows_filtered=" << stats.rows_filtered
                   << " rows_malformed=" << stats.rows_malformed
                   << " dates_written=" << stats.dates_written
                   << " distinct_securities=" << stats.distinct_securities;

  // Build the history Panel over a small 2020-Q1 window.
  const auto start_ns = atx::engine::data::detail::date_to_nanos("2020-01-02");
  const auto end_ns   = atx::engine::data::detail::date_to_nanos("2020-04-01");
  ASSERT_TRUE(start_ns.has_value()) << "date_to_nanos(2020-01-02) failed";
  ASSERT_TRUE(end_ns.has_value())   << "date_to_nanos(2020-04-01) failed";

  atx::engine::data::HistoryDataConfig hcfg;
  hcfg.seg_dir = out_dir.string();
  hcfg.window.start_nanos   = *start_ns;
  hcfg.window.end_nanos     = *end_ns;
  hcfg.universe.min_adv_usd  = 5.0e6;
  hcfg.universe.top_n_by_adv = 200;

  auto hp = atx::engine::data::build_history_panel(hcfg);
  ASSERT_TRUE(hp.has_value()) << "build_history_panel (operator) failed: "
                               << hp.error().to_string();
  EXPECT_GT(hp->panel.instruments(), 0u);
  EXPECT_GT(hp->panel.dates(), 0u);

  // Drive the UNCHANGED RobustResearchDriver.
  Library dsl{};
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  AlphaGate gate{default_gate_cfg()};
  lib::Library library =
      lib::Library::open(smoke_tmpdir(), default_gate_cfg(), {kLibSeed});

  RobustResearchDriver driver{library, dsl, hp->panel, sim, policy, gate};
  const RobustPipelineConfig pcfg = smoke_pipeline_cfg();
  auto report = driver.run(pcfg);
  ASSERT_TRUE(report.has_value()) << "RobustResearchDriver::run (operator) failed: "
                                   << report.error().to_string();

  GTEST_LOG_(INFO) << "[OperatorOratsZip/Driver] instruments=" << hp->panel.instruments()
                   << " dates=" << hp->panel.dates()
                   << " admitted=" << report->research.total_admitted
                   << " robust_size=" << report->robust_size
                   << " book.ran=" << report->book.ran
                   << " digest=0x" << std::hex << report->research.digest << std::dec;
}

} // namespace atxtest_orats_e2e_smoke
