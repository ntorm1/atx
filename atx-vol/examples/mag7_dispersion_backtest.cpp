// mag7_dispersion_backtest.cpp — the acceptance example: MAG7-vs-SPY
// dispersion-strangle backtest, driven entirely by a SurfaceDb. Gate test:
// Mag7DispersionBacktest (tests/mag7_dispersion_backtest_test.cpp). OFF by
// default (ATX_BUILD_EXAMPLES).
//
//   mag7_dispersion_backtest --db DIR [--out DIR]
//       [--names AAPL,MSFT,GOOGL,AMZN,NVDA,META,TSLA] [--index SPY]
//       [--theta-per-name 10.0] [--delta 0.40] [--tenor-days 90]
//       [--close-dte 10] [--min-names 4] [--frictions] [--threads N]
//
// Flow: open db -> Clock::from_surface_db -> make_dispersion_strangle_spec ->
// DeclarativeStrategy -> run_timed (the spine: timed run_backtest + tearsheet +
// EngineRunStats) -> emit the four pinned CSVs under --out, plus the conditional
// populate_stats.csv byte copy -> print a console summary. The Python renderer
// (a later work item) reads --out verbatim, so the file names, the shared
// `# key=value` meta block, and the CLI defaults below are a BINDING contract.
// Exit codes: 2 bad args, 1 runtime error.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/backtest/backtest.hpp"            // Clock, RunConfig, SnapshotCache
#include "atx/vol/research/backtest_driver.hpp"     // run_timed (the timed-run + tearsheet + stats spine)
#include "storage/track_key.hpp"  // kBacktestEconomicsRev
#include "atx/vol/api/backtest/dispersion.hpp"          // MissingNamePolicy, MissingNameSpec
#include "atx/vol/api/backtest/dispersion_strangle.hpp" // DispersionStrangleConfig, make_dispersion_strangle_spec
#include "atx/vol/tools/run_report.hpp"          // MetaKv, write_* emitters, EngineRunStats
#include "atx/vol/api/backtest/strategy.hpp"            // DeclarativeStrategy
#include "atx/vol/api/storage/surface_db.hpp"          // SurfaceDb
#include "atx/vol/tools/tearsheet.hpp"           // TearSheet
#include "atx/vol/api/core/types.hpp"               // Result, Status
#include "dispersion_realism_flags.hpp"    // Task E1: friction/financing/policy CLI flags

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::vector<std::string> split_csv_list(std::string_view csv) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t end = csv.find(',', start);
    const std::string_view field =
        csv.substr(start, end == std::string_view::npos ? csv.size() - start : end - start);
    if (!field.empty()) {
      out.emplace_back(field);
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return out;
}

[[nodiscard]] std::string join_csv_list(const std::vector<std::string> &v) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += v[i];
  }
  return out;
}

// Metric-value formatting discipline (%.10g), matching run_report.cpp.
[[nodiscard]] std::string fmt_num(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  return buf;
}

[[nodiscard]] std::string missing_policy_str(MissingNamePolicy p) {
  return (p == MissingNamePolicy::DropRenormalize) ? "drop_renormalize" : "error";
}

struct Args {
  std::string db;
  std::string out;
  std::vector<std::string> names{"AAPL", "MSFT", "GOOGL", "AMZN", "NVDA", "META", "TSLA"};
  std::string index_symbol{"SPY"};
  double theta_per_name{10.0};
  double delta{0.40};
  double tenor_days{90.0};
  double close_dte{10.0};
  std::size_t min_names{4};
  bool frictions{false};
  unsigned threads{0}; // 0 = RunConfig default (all cores)
  RealismArgs realism; // Task E1: financing/friction/policy overrides
};

void print_usage() {
  std::fprintf(stderr,
               "usage: mag7_dispersion_backtest --db DIR [--out DIR] "
               "[--names AAPL,MSFT,GOOGL,AMZN,NVDA,META,TSLA] [--index SPY] "
               "[--theta-per-name 10.0] [--delta 0.40] [--tenor-days 90] "
               "[--close-dte 10] [--min-names 4] [--frictions] [--threads N]%s\n",
               std::string(kRealismUsage).c_str());
}

// Parse argv into `a`. False (unknown flag / missing required --db) -> caller
// prints usage and exits 2.
[[nodiscard]] bool parse_args(int argc, char **argv, Args &a) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto nv = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : ""; };
    if (arg == "--db") {
      a.db = nv();
    } else if (arg == "--out") {
      a.out = nv();
    } else if (arg == "--names") {
      a.names = split_csv_list(nv());
    } else if (arg == "--index") {
      a.index_symbol = nv();
    } else if (arg == "--theta-per-name") {
      a.theta_per_name = std::strtod(nv(), nullptr);
    } else if (arg == "--delta") {
      a.delta = std::strtod(nv(), nullptr);
    } else if (arg == "--tenor-days") {
      a.tenor_days = std::strtod(nv(), nullptr);
    } else if (arg == "--close-dte") {
      a.close_dte = std::strtod(nv(), nullptr);
    } else if (arg == "--min-names") {
      a.min_names = static_cast<std::size_t>(std::strtoul(nv(), nullptr, 10));
    } else if (arg == "--frictions") {
      a.frictions = true;
    } else if (arg == "--threads") {
      a.threads = static_cast<unsigned>(std::strtoul(nv(), nullptr, 10));
    } else if (parse_realism_flag(arg, nv, a.realism)) {
      // Task E1: financing/friction/policy flag, recognized and consumed above.
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return false;
    }
  }
  if (a.db.empty()) {
    std::fprintf(stderr, "--db is required\n");
    return false;
  }
  if (a.names.empty()) {
    std::fprintf(stderr, "--names must not be empty\n");
    return false;
  }
  if (a.out.empty()) {
    a.out = (fs::temp_directory_path() / "atx-mag7-dispersion").string();
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    print_usage();
    return 2;
  }

  auto db = SurfaceDb::open(args.db);
  if (!db) {
    std::fprintf(stderr, "SurfaceDb::open(%s): %s\n", args.db.c_str(), db.error().to_string().c_str());
    return 1;
  }

  auto clock = Clock::from_surface_db(*db);
  if (!clock) {
    std::fprintf(stderr, "Clock::from_surface_db: %s\n", clock.error().to_string().c_str());
    return 1;
  }

  DispersionStrangleConfig cfg;
  cfg.names = args.names;
  cfg.index_symbol = args.index_symbol;
  cfg.target_abs_delta = args.delta;
  cfg.tenor_days = args.tenor_days;
  cfg.close_dte_days = args.close_dte;
  cfg.theta_per_name_daily = args.theta_per_name;
  cfg.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, args.min_names};

  auto spec = make_dispersion_strangle_spec(cfg);
  if (!spec) {
    std::fprintf(stderr, "make_dispersion_strangle_spec: %s\n", spec.error().to_string().c_str());
    return 1;
  }

  DeclarativeStrategy strat(*spec);
  RunConfig rc;
  rc.snapshot_cache = std::make_shared<SnapshotCache>();
  // Task E1: this used to force UnpricedLotPolicy::ExcludeAndReport here,
  // overriding RunConfig{}'s own (already-Error, WS-F F1(c)) default -- the
  // acceptance driver silently truncated NAV across any missing board instead
  // of failing loud. Deleting the override lets the engine's production
  // default stand; `--unpriced exclude` opts back into the lenient behavior
  // below.
  rc.price.n_threads = args.threads;
  if (args.frictions) {
    // Simple nonzero default: a modest bid/ask (5 bps half-spread on mark) +
    // a flat per-contract commission. Trivial by design (--frictions just
    // needs to move the needle off the frictionless baseline).
    rc.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
    rc.frictions.half_spread_bps = 5.0;
    rc.frictions.per_contract_cost = 0.65;
  }
  // Task E1: explicit flags refine (or replace) the --frictions preset above,
  // same "preset first, explicit keys refine" order dispersion_run.cpp's own
  // typed config builder uses for its friction_preset spec key.
  {
    std::string realism_err;
    if (!apply_realism_args(args.realism, rc, realism_err)) {
      std::fprintf(stderr, "%s\n", realism_err.c_str());
      return 2;
    }
  }

  // The spine (Wave C): time the engine call, fold the tearsheet, capture the
  // stats. `wall_clock_ms` still brackets ONLY `run_backtest` — the fold is not
  // inside the interval — so `engine_metrics.csv`'s timing rows keep their
  // meaning. `stats.cache` is the spine reading `rc.snapshot_cache->stats()`;
  // this driver always supplies a shared cache (:189), so it takes the non-null
  // branch and the four deterministic `n_steps`/`cache_*` rows do not move.
  auto outcome = run_timed(*clock, strat, rc);
  if (!outcome) {
    std::fprintf(stderr, "run_backtest: %s\n", outcome.error().to_string().c_str());
    return 1;
  }
  const BacktestResult &r = outcome->result;
  const TearSheet ts = outcome->sheet;
  const EngineRunStats &stats = outcome->stats;
  const double wall_ms = stats.wall_clock_ms; // still needed by the console summary

  std::error_code ec;
  fs::create_directories(args.out, ec);

  // Shared meta block, written verbatim into every emitted file. Keys +
  // order are the BINDING contract the Python renderer reads.
  const MetaKv meta = {
      {"strategy", "mag7_dispersion_strangle"},
      {"names", join_csv_list(cfg.names)},
      {"index_symbol", cfg.index_symbol},
      {"data_source", "surface_db"},
      {"db_root", db->root()},
      {"db_generation", std::to_string(db->generation())},
      {"window_start", clock->refs().front().date},
      {"window_end", clock->refs().back().date},
      {"n_steps", std::to_string(r.size())},
      {"delta_target", fmt_num(cfg.target_abs_delta)},
      {"tenor_days", fmt_num(cfg.tenor_days)},
      {"close_dte_days", fmt_num(cfg.close_dte_days)},
      {"theta_per_name_daily", fmt_num(cfg.theta_per_name_daily)},
      {"entry_every_n_days", std::to_string(cfg.entry_every_n_days)},
      {"multiplier", "100"},
      {"frictions", args.frictions ? "on" : "off"},
      {"missing_policy", missing_policy_str(cfg.missing.policy)},
      {"min_names", std::to_string(cfg.missing.min_names)},
      // Task E1: every emitted artifact now names its own pricing assumption
      // (A3/B1's `friction_regime`, classified from `RunConfig::frictions`
      // alone -- see `BacktestResult::friction_regime`'s own comment) and the
      // engine economics revision (D1's `kBacktestEconomicsRev`) that produced
      // it, so a reader of series.csv/strategy_metrics.csv/engine_metrics.csv/
      // db_stats.csv never has to cross-reference the RunConfig that made them.
      {"friction_regime", std::string(to_string(r.friction_regime))},
      {"economics_rev", std::to_string(kBacktestEconomicsRev)},
  };

  const std::string series_path = (fs::path(args.out) / "series.csv").string();
  Status st = write_backtest_series_csv(r, meta, series_path);
  if (!st) {
    std::fprintf(stderr, "write_backtest_series_csv: %s\n", st.error().to_string().c_str());
    return 1;
  }

  MetaKv strat_rows = strategy_metrics(ts);
  const MetaKv summary_rows = result_summary_metrics(r);
  strat_rows.insert(strat_rows.end(), summary_rows.begin(), summary_rows.end());
  const std::string strat_path = (fs::path(args.out) / "strategy_metrics.csv").string();
  st = write_metrics_csv(meta, strat_rows, strat_path);
  if (!st) {
    std::fprintf(stderr, "write_metrics_csv(strategy_metrics): %s\n", st.error().to_string().c_str());
    return 1;
  }

  const std::string engine_path = (fs::path(args.out) / "engine_metrics.csv").string();
  st = write_metrics_csv(meta, engine_metrics(stats), engine_path);
  if (!st) {
    std::fprintf(stderr, "write_metrics_csv(engine_metrics): %s\n", st.error().to_string().c_str());
    return 1;
  }

  const std::string db_stats_path = (fs::path(args.out) / "db_stats.csv").string();
  st = write_surface_db_stats_csv(*db, meta, db_stats_path);
  if (!st) {
    std::fprintf(stderr, "write_surface_db_stats_csv: %s\n", st.error().to_string().c_str());
    return 1;
  }

  // populate_stats.csv is a byte copy from the db root, only when present
  // (a db populated straight from write_partition in tests has none).
  const fs::path populate_src = fs::path(args.db) / "populate_stats.csv";
  if (fs::exists(populate_src, ec)) {
    fs::copy_file(populate_src, fs::path(args.out) / "populate_stats.csv",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
      std::fprintf(stderr, "copy populate_stats.csv: %s\n", ec.message().c_str());
      return 1;
    }
  }

  const double steps_per_s = (wall_ms > 0.0) ? 1000.0 * static_cast<double>(r.size()) / wall_ms : 0.0;
  const double peak_lots = *std::max_element(r.n_open_lots.begin(), r.n_open_lots.end());
  std::printf("=== MAG7 dispersion-strangle backtest ===\n"
              "db: %s (generation %llu) | window: %s .. %s (%zu steps)\n"
              "names: %s vs index %s | delta=%.2f tenor=%.0fd close=%.1fd theta/name=$%.2f/day "
              "min_names=%zu frictions=%s\n"
              "[timing] run_backtest: %.1f ms over %zu steps (%.1f steps/s)\n"
              "[economics] friction_regime=%s economics_rev=%d\n"
              "[tearsheet] total_return=%.2f ann_return=%.2f ann_vol=%.2f sharpe=%.3f "
              "max_drawdown=%.2f hit_rate=%.3f\n"
              "[book] avg_gross_vega=%.1f avg_gross_gamma=%.4f peak_open_lots=%.0f\n"
              "[wrote] %s\n[wrote] %s\n[wrote] %s\n[wrote] %s\n",
              db->root().c_str(), static_cast<unsigned long long>(db->generation()),
              clock->refs().front().date.c_str(), clock->refs().back().date.c_str(), r.size(),
              join_csv_list(cfg.names).c_str(), cfg.index_symbol.c_str(), cfg.target_abs_delta,
              cfg.tenor_days, cfg.close_dte_days, cfg.theta_per_name_daily, cfg.missing.min_names,
              args.frictions ? "on" : "off", wall_ms, r.size(), steps_per_s,
              std::string(to_string(r.friction_regime)).c_str(), kBacktestEconomicsRev,
              ts.total_return, ts.ann_return, ts.ann_vol, ts.sharpe, ts.max_drawdown, ts.hit_rate,
              ts.avg_gross_vega, ts.avg_gross_gamma, peak_lots, series_path.c_str(),
              strat_path.c_str(), engine_path.c_str(), db_stats_path.c_str());
  return 0;
}
