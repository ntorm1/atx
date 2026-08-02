// strangle_varswap_driver.cpp — the XOM strangle-vs-varswap comparison driver.
//
// One fixed-expiry, daily-restriked 40-delta strangle against one uncapped
// variance swap struck fair and sized to that strangle's entry vega, both run on
// ONE clock off a fitted SurfaceDb, delta-hedged to zero every session. Emits the
// self-describing track TSV the Python renderer
// `tools/render_strangle_vs_varswap.py` turns into the comparison report.
//
//   atx-vol-strangle-varswap-driver --db DIR --out DIR
//       [--symbol XOM] [--from YYYY-MM-DD] [--to YYYY-MM-DD]
//       [--delta 0.40] [--tenor-days 91] [--contracts 100]
//
// Flow: parse -> open db -> windowed Clock -> PROBE each session for the
// symbol's surface -> strategy -> run_backtest -> track.tsv -> console summary.
// Exit codes: 2 bad args, 1 runtime error. OFF by default (ATX_BUILD_EXAMPLES);
// this is an acceptance driver, not a shipped operator CLI.
//
// ## Why the session grid is PROBED rather than taken from the clock
//
// The comparison carries a LIVE VARIANCE SWAP, and the swap lane fails the whole
// run closed on any session whose board is missing (backtest.hpp: "An absent
// surface for ANY live swap lot — held OR settling — is NotFound"). There is no
// `ExcludeAndReport` escape for it: that policy governs unpriced OPTION lots. So
// a session the db lists but on which this symbol has no surface — an
// index-only partition, a fit that dropped — cannot be stepped over; it has to
// be absent from the clock in the first place. Probing every ref and dropping
// the dark ones is what makes the run possible at all, and it is the same probe
// the sp100 driver performs in Python (`run_sp100_strangle_backtest.py`,
// `session_timestamps`), with one difference: that one has a whole universe to
// fall back on and treats a partition serving NONE of it as fatal, while here
// the one symbol IS the universe, so a dark partition is a dropped session
// rather than an error. Every drop is reported — on stderr and in the TSV meta —
// because a hole in the calendar changes what the run measured.
//
// DROPPING A SESSION IS A CLAIM ABOUT THE DATA, so the probe is careful about
// what earns one. It is a TWO-STEP: open the partition ARCHIVE, then look the
// SYMBOL up inside it. `SurfaceDb::map_surface` cannot be used for this, because
// it folds a missing FILE and a missing SYMBOL into the same `NotFound`
// (`SurfaceArchiveV2::open_file` returns `NotFound` for an absent path) — so a
// database whose manifest lists partitions whose files are gone would read as
// "this symbol has no surface on those dates", and the run would come back with
// a quietly shortened window that looks like a fact about the market. The
// archive-level split makes the two unambiguous: a failure to OPEN is always an
// error, and only the archive's own directory probe can call a session dark.
//
// The probe pays for itself twice: the surviving refs become the clock, and the
// surfaces' own `now_ts_ns` values become `StrangleVarswapConfig::session_ts`,
// the grid the strategy snaps each cycle's expiry onto. An expiry that is not a
// session the run observes can never be settled, so the two MUST be the same
// list — which is why they are built in one pass here rather than derived twice.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"           // Err, Ok, ATX_TRY
#include "atx/vol/backtest.hpp"         // Clock, RunConfig, SnapshotCache, run_backtest
#include "atx/vol/corpus.hpp"           // CorpusManifest, CorpusEntry (the filtered clock)
#include "atx/vol/strangle_varswap.hpp" // StrangleVarswapConfig, StrangleVsVarswapStrategy
#include "atx/vol/surface_archive.hpp"  // SurfaceArchiveV2 (the two-step session probe)
#include "atx/vol/surface_db.hpp"       // SurfaceDb
#include "atx/vol/tools/tearsheet.hpp"  // TearSheet, tearsheet, write_backtest_pnl_tsv
#include "atx/vol/types.hpp"            // Result, Status, ErrorCode

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

using Meta = std::vector<std::pair<std::string, std::string>>;

// Calendar year in nanoseconds — the library's own T convention
// (`kNsPerYear`, portfolio_pricer.hpp). `--tenor-days` is therefore CALENDAR
// days: 91 here is 91 days of wall clock, which is what the strategy's snap
// anchor (`base_ts + tenor`) measures against a session-timestamp grid.
constexpr double kDaysPerYear = 365.25;

[[nodiscard]] std::string fmt_num(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  return buf;
}

// Truncated, comma-joined rendering of a date list for the meta header, so a
// long outage cannot blow the header line up to megabytes.
[[nodiscard]] std::string join_dates(const std::vector<std::string> &dates, std::size_t cap) {
  if (dates.empty()) {
    return "none";
  }
  std::string out;
  const std::size_t shown = std::min(dates.size(), cap);
  for (std::size_t i = 0; i < shown; ++i) {
    if (i != 0) {
      out += ',';
    }
    out += dates[i];
  }
  if (dates.size() > shown) {
    out += ",...(+" + std::to_string(dates.size() - shown) + " more)";
  }
  return out;
}

struct Args {
  std::string db;
  std::string out;
  std::string symbol{"XOM"};
  std::string from; // inclusive window bounds; empty = whatever the db covers
  std::string to;
  double delta{0.40};
  double tenor_days{91.0};
  double contracts{100.0};
};

void usage() {
  std::fprintf(stderr, "usage: atx-vol-strangle-varswap-driver --db DIR --out DIR\n"
                       "         [--symbol XOM] [--from YYYY-MM-DD] [--to YYYY-MM-DD]\n"
                       "         [--delta 0.40] [--tenor-days 91] [--contracts 100]\n"
                       "\n"
                       "  Runs a fixed-expiry, daily-restriked strangle against an "
                       "equal-vega uncapped\n"
                       "  variance swap on one clock and writes <out>/track.tsv.\n"
                       "  Render it with: python tools/render_strangle_vs_varswap.py "
                       "<out>/track.tsv <out>/report.html\n");
}

[[nodiscard]] bool parse_args(int argc, char **argv, Args &a) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto nv = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : ""; };
    if (arg == "--help" || arg == "-h") {
      return false;
    } else if (arg == "--db") {
      a.db = nv();
    } else if (arg == "--out") {
      a.out = nv();
    } else if (arg == "--symbol") {
      a.symbol = nv();
    } else if (arg == "--from") {
      a.from = nv();
    } else if (arg == "--to") {
      a.to = nv();
    } else if (arg == "--delta") {
      a.delta = std::strtod(nv(), nullptr);
    } else if (arg == "--tenor-days") {
      a.tenor_days = std::strtod(nv(), nullptr);
    } else if (arg == "--contracts") {
      a.contracts = std::strtod(nv(), nullptr);
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return false;
    }
  }
  // Validated HERE, once, at the boundary: everything downstream may assume it.
  // The strategy re-checks its own knobs on the first step (it has an error
  // channel and a constructor does not), but a driver that hands it garbage and
  // lets the engine report it makes the operator read an engine message for a
  // typo in an argument.
  if (a.db.empty()) {
    std::fprintf(stderr, "--db is required\n");
    return false;
  }
  if (a.out.empty()) {
    std::fprintf(stderr, "--out is required\n");
    return false;
  }
  if (a.symbol.empty()) {
    std::fprintf(stderr, "--symbol must be non-empty\n");
    return false;
  }
  if (!(std::isfinite(a.delta) && a.delta > 0.0 && a.delta < 1.0)) {
    std::fprintf(stderr, "--delta must lie in (0,1)\n");
    return false;
  }
  if (!(std::isfinite(a.tenor_days) && a.tenor_days > 0.0)) {
    std::fprintf(stderr, "--tenor-days must be finite and positive\n");
    return false;
  }
  if (!(std::isfinite(a.contracts) && a.contracts != 0.0)) {
    std::fprintf(stderr, "--contracts must be finite and non-zero\n");
    return false;
  }
  if (!a.from.empty() && !a.to.empty() && a.from > a.to) {
    std::fprintf(stderr, "--from '%s' is after --to '%s'\n", a.from.c_str(), a.to.c_str());
    return false;
  }
  return true;
}

// The run's timeline: the sessions on which `symbol` actually has a surface,
// their market instants, and the ones that were dropped for not having one.
struct SessionGrid {
  Clock clock;                          // only the surviving refs
  std::vector<std::int64_t> session_ts; // their `now_ts_ns`, ascending + unique
  std::vector<std::string> dark_dates;  // listed by the db, dark for this symbol
  bool ts_reordered{false};             // partition-key order != market-instant order
};

// Probe every ref of `full` for `symbol` and keep the ones that answer.
//
// Only the SECOND step can call a session dark; see the file header for why the
// two are split rather than taken from one `SurfaceDb::map_surface` call.
[[nodiscard]] Result<SessionGrid> probe_sessions(const Clock &full, const std::string &symbol) {
  SessionGrid grid;
  CorpusManifest live;
  std::vector<std::int64_t> stamps;
  stamps.reserve(full.size());
  for (const SnapshotRef &ref : full.refs()) {
    // STEP 1 — THE PARTITION FILE. Every ref here came out of `db.partitions()`,
    // so the manifest lists it BY CONSTRUCTION and `partition_listed` has
    // nothing left to discriminate. The failure that IS live is the other half
    // of the same manifest/disk disagreement the db API splits out
    // (surface_db.hpp): the key is listed and the FILE is gone — a crash between
    // `write_partition`'s rename and its manifest persist, a manifest restored
    // from an older copy, a partially-copied root. EVERY failure to open is
    // propagated, `NotFound` included, because "listed but absent" is a broken
    // database and never a dark session.
    //
    // Probed BY PATH rather than through the db, so the bytes checked here are
    // the exact file `MarketSnapshot::load` will open for this ref during the
    // run — the probe verifies the thing the engine is about to depend on.
    auto archive = SurfaceArchiveV2::open_mapped(ref.archive_path);
    if (!archive) {
      return atx::core::Err(archive.error().code(),
                            "partition " + ref.date +
                                " is listed by the db manifest but its archive did not open (" +
                                ref.archive_path + "): " + archive.error().to_string() +
                                " — the database is inconsistent, not this symbol's calendar");
    }
    // STEP 2 — THE SYMBOL. A `NotFound` from the archive's OWN directory probe
    // is unambiguous: the file is open and holds no record under this name. That
    // is the one condition that earns a dropped session. Name resolution matches
    // the strategy's (`MarketSnapshot::uid_of`) — both canonicalize through
    // `canonical_symbol` — so the probe keeps exactly the sessions the strategy
    // can resolve, no more and no fewer.
    auto surface = archive->map_symbol(symbol);
    if (!surface) {
      if (surface.error().code() == ErrorCode::NotFound) {
        grid.dark_dates.push_back(ref.date);
        continue;
      }
      return atx::core::Err(surface.error().code(), "probe " + symbol + " on " + ref.date + ": " +
                                                        surface.error().to_string());
    }
    stamps.push_back(surface->pricing().now_ts_ns);
    live.dates.push_back(ref.date);
    CorpusEntry e;
    e.date = ref.date;
    e.symbol = "*"; // one Ok entry per date; from_manifest keys on date + Ok
    e.status = CorpusFitStatus::Ok;
    e.archive_path = ref.archive_path;
    live.entries.push_back(std::move(e));
  }
  if (live.dates.empty()) {
    return atx::core::Err(ErrorCode::NotFound, "no partition in the window carries a '" + symbol +
                                                   "' surface, so there is no session to run on");
  }
  ATX_TRY(Clock live_clock, Clock::from_manifest(live));
  grid.clock = std::move(live_clock);

  // The strategy binary-searches this grid, so it must be sorted; it is also the
  // settlement calendar, so a repeated instant would make one expiry ambiguous.
  // Sorting + de-duplicating is the same repair the sp100 driver performs, and
  // the disagreement is recorded rather than silently smoothed.
  grid.session_ts = stamps;
  std::sort(grid.session_ts.begin(), grid.session_ts.end());
  grid.session_ts.erase(std::unique(grid.session_ts.begin(), grid.session_ts.end()),
                        grid.session_ts.end());
  grid.ts_reordered = (grid.session_ts != stamps);
  return atx::core::Ok(std::move(grid));
}

// Ride the swap lane's two columns out through the TSV's dynamic signal tail.
//
// `swap_pv`/`swap_pnl` are DELIBERATELY not part of `kBacktestSeriesColumns`
// (backtest_series_columns.hpp) — that table's {name, order} is pinned to the
// frozen RunArchive registry whose fold is `ra_schema_hash()`, so adding them
// there is a build error by design and a schema decision this driver has no
// business making. The writer's per-signal columns are the one dynamic tail it
// already emits, so the pair rides out there: ONE TSV format, no fork, and the
// renderer reads them by name exactly like the strategy's own signals.
[[nodiscard]] Status attach_swap_columns(BacktestResult &r) {
  if (r.swap_pv.size() != r.size() || r.swap_pnl.size() != r.size()) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "attach_swap_columns: swap columns are not row-parallel (" +
                              std::to_string(r.swap_pv.size()) + "/" +
                              std::to_string(r.swap_pnl.size()) + " vs " +
                              std::to_string(r.size()) + " rows)");
  }
  for (const auto &sig : r.signals) {
    if (sig.first == "swap_pv" || sig.first == "swap_pnl") {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "attach_swap_columns: the strategy already emits a '" + sig.first +
                                "' signal, which would make the TSV header ambiguous");
    }
  }
  r.signals.emplace_back("swap_pv", r.swap_pv);
  r.signals.emplace_back("swap_pnl", r.swap_pnl);
  return atx::core::Ok();
}

[[nodiscard]] double sum(const std::vector<double> &v) noexcept {
  double total = 0.0;
  for (const double x : v) {
    total += x;
  }
  return total;
}

// The last row's value of a cumulative signal counter, or 0 when the run
// recorded no rows / the strategy emitted no such signal.
[[nodiscard]] double last_signal(const BacktestResult &r, std::string_view name) noexcept {
  for (const auto &sig : r.signals) {
    if (sig.first == name && !sig.second.empty()) {
      return sig.second.back();
    }
  }
  return 0.0;
}

} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    usage();
    return 2;
  }

  auto db = SurfaceDb::open(args.db);
  if (!db) {
    std::fprintf(stderr, "SurfaceDb::open(%s): %s\n", args.db.c_str(),
                 db.error().to_string().c_str());
    return 1;
  }
  auto full = Clock::from_surface_db(*db);
  if (!full) {
    std::fprintf(stderr, "Clock::from_surface_db: %s\n", full.error().to_string().c_str());
    return 1;
  }
  // `between` CLAMPS an over-wide window, so an unspecified bound is the db's
  // own edge rather than a special case here.
  const std::string lo = args.from.empty() ? full->refs().front().date : args.from;
  const std::string hi = args.to.empty() ? full->refs().back().date : args.to;
  auto windowed = full->between(lo, hi);
  if (!windowed) {
    std::fprintf(stderr, "Clock::between: %s\n", windowed.error().to_string().c_str());
    return 1;
  }

  auto grid = probe_sessions(*windowed, args.symbol);
  if (!grid) {
    std::fprintf(stderr, "probe_sessions: %s\n", grid.error().to_string().c_str());
    return 1;
  }
  if (!grid->dark_dates.empty()) {
    std::fprintf(stderr,
                 "WARNING: %zu of %zu partition(s) in the window carry no '%s' surface and were "
                 "DROPPED from the run (a live variance swap fails the run closed on a missing "
                 "board, so they cannot be stepped over): %s\n",
                 grid->dark_dates.size(), windowed->size(), args.symbol.c_str(),
                 join_dates(grid->dark_dates, 30).c_str());
  }
  if (grid->ts_reordered) {
    std::fprintf(stderr,
                 "WARNING: the corpus's session timestamps are not in partition-key order (or "
                 "repeat); the snap grid is the sorted, de-duplicated set.\n");
  }

  StrangleVarswapConfig cfg;
  cfg.symbol = args.symbol;
  cfg.target_abs_delta = args.delta;
  cfg.tenor_years = args.tenor_days / kDaysPerYear;
  cfg.contracts = args.contracts;
  cfg.session_ts = grid->session_ts;
  cfg.enable_swap_leg = true;
  // `cfg.deriv_cfg` IS LEFT AT ITS DEFAULT ON PURPOSE. The engine marks a live
  // swap under its own hard-coded default `DerivConfig`, so only a default here
  // opens the swap at a genuine zero PV and keeps the `strangle_vega ==
  // swap_vega` identity that makes the two legs comparable at inception.
  // Exposing it as a flag would let an operator break that silently.

  StrangleVsVarswapStrategy strat(cfg);
  RunConfig rc;
  rc.snapshot_cache = std::make_shared<SnapshotCache>();
  // The hedge lane needs this: a session on which a hedge share cannot be marked
  // must be reported and excluded rather than aborting a run whose swap leg is
  // otherwise fine. It does NOT soften the swap lane — that one fails closed
  // regardless, which is why dark sessions are dropped from the clock above.
  rc.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  // NAV is a cumulative flow sum; this recomputes it from the cash/share/book
  // ledgers every row and aborts on drift. A comparison whose two legs are read
  // off NAV columns is worth exactly as much as NAV is, so it is audited.
  rc.reconcile_nav = true;

  auto run = run_backtest(grid->clock, strat, rc);
  if (!run) {
    std::fprintf(stderr, "run_backtest: %s\n", run.error().to_string().c_str());
    return 1;
  }
  BacktestResult &r = *run;
  const Status attached = attach_swap_columns(r);
  if (!attached) {
    std::fprintf(stderr, "%s\n", attached.error().to_string().c_str());
    return 1;
  }

  const TearSheet ts = tearsheet(r);
  const double swap_total = sum(r.swap_pnl);
  const double combined_total = r.nav.empty() ? 0.0 : r.nav.back();
  const double strangle_total = combined_total - swap_total;
  const double skipped_restrikes = last_signal(r, "skipped_restrikes");
  const double skipped_swaps = last_signal(r, "skipped_swaps");
  const std::string window_start = grid->clock.refs().front().date;
  const std::string window_end = grid->clock.refs().back().date;

  std::error_code ec;
  fs::create_directories(args.out, ec);
  if (ec) {
    std::fprintf(stderr, "cannot create --out %s: %s\n", args.out.c_str(), ec.message().c_str());
    return 1;
  }

  const Meta meta = {
      {"strategy", "xom_strangle_vs_varswap"},
      {"symbol", args.symbol},
      {"data_source", "surface_db"},
      {"db_root", db->root()},
      {"db_generation", std::to_string(db->generation())},
      {"requested_from", lo},
      {"requested_to", hi},
      {"window_start", window_start},
      {"window_end", window_end},
      {"n_steps", std::to_string(r.size())},
      {"sessions_in_window", std::to_string(windowed->size())},
      {"sessions_dark", std::to_string(grid->dark_dates.size())},
      {"sessions_dark_list", join_dates(grid->dark_dates, 30)},
      {"delta_target", fmt_num(args.delta)},
      {"tenor_days", fmt_num(args.tenor_days)},
      {"tenor_years", fmt_num(cfg.tenor_years)},
      {"contracts", fmt_num(args.contracts)},
      {"multiplier", "100"},
      {"hedge", "delta_to_zero_daily"},
      {"swap_leg", "uncapped_variance_equal_entry_vega"},
      {"unpriced_policy", "exclude_and_report"},
      {"reconcile_nav", "on"},
      {"strangle_total", fmt_num(strangle_total)},
      {"swap_total", fmt_num(swap_total)},
      {"combined_total", fmt_num(combined_total)},
      {"skipped_restrikes", fmt_num(skipped_restrikes)},
      {"skipped_swaps", fmt_num(skipped_swaps)},
      {"total_return", fmt_num(ts.total_return)},
      {"ann_return", fmt_num(ts.ann_return)},
      {"ann_vol", fmt_num(ts.ann_vol)},
      {"sharpe", fmt_num(ts.sharpe)},
      {"max_drawdown", fmt_num(ts.max_drawdown)},
      {"hit_rate", fmt_num(ts.hit_rate)},
      {"avg_gross_vega", fmt_num(ts.avg_gross_vega)},
  };

  const std::string tsv_path = (fs::path(args.out) / "track.tsv").string();
  const Status st = write_backtest_pnl_tsv(r, meta, tsv_path);
  if (!st) {
    std::fprintf(stderr, "write_backtest_pnl_tsv: %s\n", st.error().to_string().c_str());
    return 1;
  }

  std::printf("=== %s strangle vs variance swap ===\n"
              "db: %s (gen %llu) | requested %s .. %s | ran %s .. %s (%zu rows)\n"
              "sessions: %zu in window, %zu dark and dropped\n"
              "config: %.2f delta, %.0fd fixed-expiry cycles, %.0f contracts/wing, "
              "delta-hedged daily\n"
              "legs: strangle $%.2f | var-swap $%.2f | combined $%.2f\n"
              "schedule: %.0f restrike skip(s), %.0f cycle(s) opened without a swap\n"
              "tearsheet: sharpe=%.3f ann_vol=%.2f max_drawdown=%.2f hit_rate=%.3f\n"
              "[wrote] %s\n"
              "  render: python tools/render_strangle_vs_varswap.py %s %s\n",
              args.symbol.c_str(), db->root().c_str(),
              static_cast<unsigned long long>(db->generation()), lo.c_str(), hi.c_str(),
              window_start.c_str(), window_end.c_str(), r.size(), windowed->size(),
              grid->dark_dates.size(), args.delta, args.tenor_days, args.contracts, strangle_total,
              swap_total, combined_total, skipped_restrikes, skipped_swaps, ts.sharpe, ts.ann_vol,
              ts.max_drawdown, ts.hit_rate, tsv_path.c_str(), tsv_path.c_str(),
              (fs::path(args.out) / "report.html").string().c_str());
  return 0;
}
