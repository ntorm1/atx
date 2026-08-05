// spy_leaps_strangle_backtest.cpp — long-dated SPY strangle acceptance driver.
//
// One 40-delta SPY strangle entered at a ~2-year tenor, rolled every calendar
// month (close + re-strike at the fresh tenor), delta-hedged to zero every
// session, zero frictions. The timeline spans MULTIPLE per-year SurfaceDb
// roots (spy-2019 .. spy-2026), which no single `Clock::from_surface_db` can
// produce — so the driver assembles one `CorpusManifest` across the roots and
// takes the cross-year clock from `Clock::from_manifest`. Emits the
// self-describing PnL-track TSV plus the full per-step series TSV.
//
//   atx-vol-spy-leaps-strangle --out DIR
//       [--db-prefix C:/atx-data/surface-db-r2/spy] [--year-lo 2019] [--year-hi 2026]
//       [--from YYYY-MM-DD] [--to YYYY-MM-DD]
//       [--delta 0.40] [--tenor-years 2.0] [--roll-months 1] [--contracts 1]
//       [--sign +1|-1]
//
// Flow: parse -> open each per-year root -> manifest + session-ts grid ->
// windowed Clock -> DeclarativeStrategy (Strangle/Delta/RollAtHorizon/
// DeltaToZero-daily) -> run_backtest -> track.tsv + series.tsv -> summary.
// Exit codes: 2 bad args, 1 runtime error. OFF by default (ATX_BUILD_EXAMPLES);
// acceptance driver, not a shipped operator CLI.
//
// The monthly roll is expressed through the DSL's residual-tenor trigger:
// `roll_at_T = tenor_years - roll_months/12`, so the single RollAtHorizon
// cohort is closed at marks and re-opened at the fresh 2y strikes as soon as
// one calendar month of tenor has decayed. Contracts, strikes and expiries are
// synthetic surface coordinates (frictionless model book), with expiries
// snapped onto the run's own session grid so a cohort's anchor is always an
// observable session.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/backtest.hpp"        // Clock, RunConfig, SnapshotCache, run_backtest
#include "atx/vol/corpus.hpp"          // CorpusManifest, CorpusEntry
#include "atx/vol/strategy.hpp"        // StrategySpec, DeclarativeStrategy
#include "atx/vol/surface_db.hpp"      // SurfaceDb
#include "atx/vol/tools/tearsheet.hpp" // TearSheet, tearsheet, write_backtest_* TSV
#include "atx/vol/types.hpp"           // Result, Status

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

using Meta = std::vector<std::pair<std::string, std::string>>;

struct Args {
  std::string db_prefix{"C:/atx-data/surface-db-r2/spy"};
  int year_lo{2019};
  int year_hi{2026};
  std::string from;
  std::string to;
  std::string out;
  double delta{0.40};
  double tenor_years{2.0};
  double roll_months{1.0};
  double contracts{1.0};
  double sign{+1.0};
  std::string mark_domain{"extrapolate"};
};

void usage() {
  std::fprintf(
      stderr,
      "usage: atx-vol-spy-leaps-strangle --out DIR [--db-prefix P] [--year-lo Y] "
      "[--year-hi Y]\n    [--from D] [--to D] [--delta X] [--tenor-years X] "
      "[--roll-months X] [--contracts X] [--sign +1|-1]\n    "
      "[--mark-domain extrapolate|carry|error]\n");
}

bool parse_args(int argc, char **argv, Args &args) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag{argv[i]};
    auto next = [&](std::string &dst) {
      if (i + 1 >= argc) return false;
      dst = argv[++i];
      return true;
    };
    std::string v;
    if (flag == "--out" && next(v)) {
      args.out = v;
    } else if (flag == "--db-prefix" && next(v)) {
      args.db_prefix = v;
    } else if (flag == "--year-lo" && next(v)) {
      args.year_lo = std::atoi(v.c_str());
    } else if (flag == "--year-hi" && next(v)) {
      args.year_hi = std::atoi(v.c_str());
    } else if (flag == "--from" && next(v)) {
      args.from = v;
    } else if (flag == "--to" && next(v)) {
      args.to = v;
    } else if (flag == "--delta" && next(v)) {
      args.delta = std::atof(v.c_str());
    } else if (flag == "--tenor-years" && next(v)) {
      args.tenor_years = std::atof(v.c_str());
    } else if (flag == "--roll-months" && next(v)) {
      args.roll_months = std::atof(v.c_str());
    } else if (flag == "--contracts" && next(v)) {
      args.contracts = std::atof(v.c_str());
    } else if (flag == "--sign" && next(v)) {
      args.sign = std::atof(v.c_str());
    } else if (flag == "--mark-domain" && next(v)) {
      args.mark_domain = v;
    } else {
      std::fprintf(stderr, "unknown/incomplete flag: %.*s\n", static_cast<int>(flag.size()),
                   flag.data());
      return false;
    }
  }
  if (args.out.empty()) {
    std::fprintf(stderr, "--out is required\n");
    return false;
  }
  if (args.year_lo > args.year_hi || args.delta <= 0.0 || args.delta >= 1.0 ||
      args.tenor_years <= 0.0 || args.roll_months <= 0.0 ||
      args.roll_months / 12.0 >= args.tenor_years || args.contracts <= 0.0 ||
      (args.sign != 1.0 && args.sign != -1.0)) {
    std::fprintf(stderr, "invalid argument values\n");
    return false;
  }
  if (args.mark_domain != "extrapolate" && args.mark_domain != "carry" &&
      args.mark_domain != "error") {
    std::fprintf(stderr, "--mark-domain must be extrapolate|carry|error\n");
    return false;
  }
  return true;
}

char num_buf[64];
std::string fmt_num(double v) {
  std::snprintf(num_buf, sizeof num_buf, "%.10g", v);
  return num_buf;
}

} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    usage();
    return 2;
  }

  // ── Cross-root manifest + session-timestamp grid ─────────────────────────
  CorpusManifest manifest;
  std::map<std::string, std::int64_t> ts_by_date; // partition key -> session ts
  std::vector<std::string> roots_used;
  for (int year = args.year_lo; year <= args.year_hi; ++year) {
    const std::string root = args.db_prefix + "-" + std::to_string(year);
    if (!fs::exists(root)) continue;
    auto db = SurfaceDb::open(root);
    if (!db) {
      std::fprintf(stderr, "SurfaceDb::open(%s): %s\n", root.c_str(),
                   db.error().to_string().c_str());
      return 1;
    }
    for (const DbPartitionInfo &p : db->partitions()) {
      auto ts = db->session_ts(p.key);
      if (!ts) {
        std::fprintf(stderr, "session_ts(%s @ %s): %s\n", p.key.c_str(), root.c_str(),
                     ts.error().to_string().c_str());
        return 1;
      }
      CorpusEntry entry;
      entry.date = p.key;
      entry.symbol = "SPY";
      entry.status = CorpusFitStatus::Ok;
      entry.archive_path = root + "/partitions/" + p.key + ".atxvsa";
      manifest.entries.push_back(std::move(entry));
      manifest.dates.push_back(p.key);
      ts_by_date[p.key] = *ts;
    }
    roots_used.push_back(root);
  }
  if (manifest.entries.empty()) {
    std::fprintf(stderr, "no partitions found under %s-{%d..%d}\n", args.db_prefix.c_str(),
                 args.year_lo, args.year_hi);
    return 1;
  }
  std::sort(manifest.dates.begin(), manifest.dates.end());
  manifest.dates.erase(std::unique(manifest.dates.begin(), manifest.dates.end()),
                       manifest.dates.end());
  std::sort(manifest.entries.begin(), manifest.entries.end(),
            [](const CorpusEntry &a, const CorpusEntry &b) {
              return a.date != b.date ? a.date < b.date : a.symbol < b.symbol;
            });
  manifest.n_boards = static_cast<std::uint32_t>(manifest.entries.size());
  manifest.n_ok = manifest.n_boards;

  auto full = Clock::from_manifest(manifest);
  if (!full) {
    std::fprintf(stderr, "Clock::from_manifest: %s\n", full.error().to_string().c_str());
    return 1;
  }
  const std::string lo = args.from.empty() ? full->refs().front().date : args.from;
  const std::string hi = args.to.empty() ? full->refs().back().date : args.to;
  auto clock = full->between(lo, hi);
  if (!clock) {
    std::fprintf(stderr, "Clock::between: %s\n", clock.error().to_string().c_str());
    return 1;
  }

  std::vector<std::int64_t> session_ts;
  session_ts.reserve(clock->size());
  for (const SnapshotRef &ref : clock->refs()) session_ts.push_back(ts_by_date.at(ref.date));

  // ── Strategy spec ────────────────────────────────────────────────────────
  StrategySpec spec;
  spec.name = "spy_leaps_40d_strangle_monthly_roll";
  LegSpec leg;
  leg.symbol = "SPY";
  leg.tenor.target_T = args.tenor_years;
  leg.tenor.snap_to_sessions = true;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, args.delta};
  leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, args.delta};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, args.contracts, args.sign};
  spec.legs.push_back(std::move(leg));
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.roll_at_T = args.tenor_years - args.roll_months / 12.0;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};
  spec.session_ts = session_ts;

  DeclarativeStrategy strat(spec);
  RunConfig rc;
  rc.snapshot_cache = std::make_shared<SnapshotCache>();
  // Frictionless by construction: FrictionModel/FinancingConfig defaults are
  // all-zero. NAV is audited against the cash/share/book ledgers every row.
  rc.reconcile_nav = true;
  rc.mark_domain = args.mark_domain == "carry"  ? MarkDomainPolicy::CarryLastMark
                   : args.mark_domain == "error" ? MarkDomainPolicy::Error
                                                 : MarkDomainPolicy::Extrapolate;

  auto run = run_backtest(*clock, strat, rc);
  if (!run) {
    std::fprintf(stderr, "run_backtest: %s\n", run.error().to_string().c_str());
    return 1;
  }
  BacktestResult &r = *run;

  const TearSheet ts = tearsheet(r);
  const double final_nav = r.nav.empty() ? 0.0 : r.nav.back();
  const double final_liq = r.nav_liquidation.empty() ? final_nav : r.nav_liquidation.back();

  std::error_code ec;
  fs::create_directories(args.out, ec);
  if (ec) {
    std::fprintf(stderr, "cannot create --out %s: %s\n", args.out.c_str(), ec.message().c_str());
    return 1;
  }

  std::string roots_joined;
  for (const std::string &root : roots_used)
    roots_joined += (roots_joined.empty() ? "" : ";") + root;

  const Meta meta = {
      {"strategy", spec.name},
      {"symbol", "SPY"},
      {"data_source", "surface_db_multi_root"},
      {"db_roots", roots_joined},
      {"requested_from", lo},
      {"requested_to", hi},
      {"window_start", clock->refs().front().date},
      {"window_end", clock->refs().back().date},
      {"n_steps", std::to_string(r.size())},
      {"sessions_in_window", std::to_string(clock->size())},
      {"delta_target", fmt_num(args.delta)},
      {"tenor_years", fmt_num(args.tenor_years)},
      {"roll_months", fmt_num(args.roll_months)},
      {"roll_at_T", fmt_num(spec.lifecycle.roll_at_T)},
      {"contracts", fmt_num(args.contracts)},
      {"sign", fmt_num(args.sign)},
      {"multiplier", "100"},
      {"hedge", "delta_to_zero_daily"},
      {"frictions", "none"},
      {"reconcile_nav", "on"},
      {"final_nav", fmt_num(final_nav)},
      {"final_nav_liquidation", fmt_num(final_liq)},
      {"total_return", fmt_num(ts.total_return)},
      {"ann_vol", fmt_num(ts.ann_vol)},
      {"sharpe", fmt_num(ts.sharpe)},
      {"max_drawdown", fmt_num(ts.max_drawdown)},
      {"hit_rate", fmt_num(ts.hit_rate)},
      {"avg_gross_vega", fmt_num(ts.avg_gross_vega)},
  };

  const std::string track_path = (fs::path(args.out) / "track.tsv").string();
  if (const Status st = write_backtest_pnl_tsv(r, meta, track_path); !st) {
    std::fprintf(stderr, "write_backtest_pnl_tsv: %s\n", st.error().to_string().c_str());
    return 1;
  }
  const std::string series_path = (fs::path(args.out) / "series.tsv").string();
  if (const Status st = write_backtest_tsv(r, series_path); !st) {
    std::fprintf(stderr, "write_backtest_tsv: %s\n", st.error().to_string().c_str());
    return 1;
  }

  std::printf("=== SPY %.0f-delta %.2gy strangle, %.3g-month roll, daily delta hedge ===\n"
              "window %s .. %s | %zu sessions | sign %+.0f x %.0f contract(s)\n"
              "final nav $%.2f (liquidation $%.2f) | sharpe %.2f | maxDD $%.2f\n"
              "wrote %s and %s\n",
              args.delta * 100.0, args.tenor_years, args.roll_months,
              clock->refs().front().date.c_str(), clock->refs().back().date.c_str(),
              clock->size(), args.sign, args.contracts, final_nav, final_liq, ts.sharpe,
              ts.max_drawdown, track_path.c_str(), series_path.c_str());
  return 0;
}
