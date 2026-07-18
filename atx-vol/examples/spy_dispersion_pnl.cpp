// spy_dispersion_pnl.cpp — the WS-D acceptance driver: the vega-flat dispersion
// PnL-track backtest, rendered end-to-end from a fitted SurfaceDb.
//
// Long top-N 40Δ 3M single-name strangles vs a short SPY 40Δ 3M strangle sized
// NET-VEGA-ZERO at entry (FlatVega), a NEW clip every trading day, DAILY
// delta-hedged, HELD TO EXPIRY. Strikes/expiries are resolved off the
// serialized surface (projection path — no listed-contract snapping). Emits the
// PnL-track TSV (`# key=value` meta header + per-step series) the Python
// renderer `tools/spy_dispersion_pnl_report.py` turns into the acceptance PNG.
//
//   spy_dispersion_pnl --db DIR [--out DIR]
//       [--universe data/universe/spy_top50_2026-01-01.csv | --names A,B,C]
//       [--index SPY] [--start YYYY-MM-DD] [--end YYYY-MM-DD] [--top-n 50]
//       [--delta 0.40] [--tenor-days 90] [--theta-per-name 10.0]
//       [--min-names 4] [--no-hedge] [--frictions] [--threads N]
//
// One command takes the universe (--universe/--names) + date range
// (--start/--end); parameterized so the PM runs it on the full YTD pull later.
// Flow: open db -> windowed Clock -> make_dispersion_strangle_spec
// {hold_to_expiry, DeltaToZero daily hedge} -> DeclarativeStrategy ->
// run_backtest (timed) -> tearsheet -> write pnl_track.tsv -> console summary.
// Exit codes: 2 bad args, 1 runtime error. OFF by default (ATX_BUILD_EXAMPLES).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"              // Err, Ok, ATX_TRY
#include "atx/vol/backtest.hpp"            // Clock, run_backtest, RunConfig, SnapshotCache
#include "atx/vol/corpus.hpp"              // CorpusManifest, CorpusEntry (windowed clock)
#include "atx/vol/dispersion.hpp"          // MissingNamePolicy, MissingNameSpec
#include "atx/vol/dispersion_strangle.hpp" // DispersionStrangleConfig, make_dispersion_strangle_spec
#include "atx/vol/strategy.hpp"            // DeclarativeStrategy
#include "atx/vol/surface_db.hpp"          // SurfaceDb
#include "atx/vol/tearsheet.hpp"           // TearSheet, tearsheet, write_backtest_pnl_tsv
#include "atx/vol/types.hpp"               // Result, Status
#include "atx/vol/universe.hpp"            // canonical_symbol

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

using Meta = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] std::string fmt_num(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  return buf;
}

[[nodiscard]] std::vector<std::string> split(std::string_view s, char delim) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t end = s.find(delim, start);
    const std::string_view field =
        s.substr(start, end == std::string_view::npos ? s.size() - start : end - start);
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

[[nodiscard]] std::string join(const std::vector<std::string> &v) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += v[i];
  }
  return out;
}

// Parse a universe fixture (D1 TSV: header `effective_date<TAB>symbol<TAB>...`,
// weight-descending rows; also accepts comma-separated). Returns the symbol
// column in file order (already sorted by index weight), the index symbol
// excluded.
[[nodiscard]] Result<std::vector<std::string>> read_universe_symbols(const std::string &path,
                                                                     const std::string &index_sym) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return atx::core::Err(ErrorCode::NotFound, "cannot open universe file: " + path);
  }
  const std::string canon_index = canonical_symbol(index_sym);
  std::vector<std::string> names;
  std::string line;
  bool header = true;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const char delim = (line.find('\t') != std::string::npos) ? '\t' : ',';
    const std::vector<std::string> f = split(line, delim);
    if (header) {
      header = false; // skip the column-name row
      continue;
    }
    if (f.size() < 2) {
      continue;
    }
    const std::string &symbol = f[1];
    if (canonical_symbol(symbol) == canon_index) {
      continue; // the index leg is added separately
    }
    names.push_back(symbol);
  }
  if (names.empty()) {
    return atx::core::Err(ErrorCode::InvalidArgument, "universe file has no names: " + path);
  }
  return atx::core::Ok(std::move(names));
}

// GOOG + GOOGL are both Alphabet share classes (class C vs class A). Keeping
// both would double-count Alphabet's idiosyncratic vol in the dispersion
// basket, so we keep exactly one. Decision: retain GOOGL (the class-A/voting
// reference class, higher index weight in the SPY N-PORT fixture), drop GOOG.
// Deterministic + logged. Returns the dropped symbol (empty if none).
[[nodiscard]] std::string dedup_alphabet(std::vector<std::string> &names) {
  bool has_googl = false;
  for (const std::string &n : names) {
    if (canonical_symbol(n) == "GOOGL") {
      has_googl = true;
    }
  }
  if (!has_googl) {
    return {};
  }
  std::string dropped;
  std::vector<std::string> kept;
  kept.reserve(names.size());
  for (std::string &n : names) {
    if (canonical_symbol(n) == "GOOG") {
      dropped = n;
      continue;
    }
    kept.push_back(std::move(n));
  }
  names = std::move(kept);
  return dropped;
}

// Build a Clock over the db restricted to [start, end] (inclusive, ISO dates
// sort chronologically). Empty bounds keep the whole db. Uses only public APIs:
// the full from_surface_db clock, then Clock::from_manifest over the in-range
// partition refs.
[[nodiscard]] Result<Clock> windowed_clock(const SurfaceDb &db, const std::string &start,
                                           const std::string &end) {
  ATX_TRY(Clock full, Clock::from_surface_db(db));
  if (start.empty() && end.empty()) {
    return atx::core::Ok(std::move(full));
  }
  CorpusManifest m;
  for (const SnapshotRef &r : full.refs()) {
    if (!start.empty() && r.date < start) {
      continue;
    }
    if (!end.empty() && r.date > end) {
      continue;
    }
    m.dates.push_back(r.date);
    CorpusEntry e;
    e.date = r.date;
    e.symbol = "*"; // one Ok entry per date; from_manifest keys on date + Ok
    e.status = CorpusFitStatus::Ok;
    e.archive_path = r.archive_path;
    m.entries.push_back(std::move(e));
  }
  if (m.dates.empty()) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "no db partitions in [" + start + ", " + end + "]");
  }
  return Clock::from_manifest(m);
}

struct Args {
  std::string db;
  std::string out;
  std::string universe;                    // universe fixture path (--universe)
  std::vector<std::string> names;          // explicit names (--names); overrides --universe
  std::string index_symbol{"SPY"};
  std::string start;                       // inclusive window bounds (empty = whole db)
  std::string end;
  std::size_t top_n{50};                   // cap on names after dedup
  double delta{0.40};
  double tenor_days{90.0};
  double theta_per_name{10.0};
  std::size_t min_names{4};
  bool hedge{true};
  bool frictions{false};
  unsigned threads{0};
};

void usage() {
  std::fprintf(stderr,
               "usage: spy_dispersion_pnl --db DIR [--out DIR] "
               "[--universe FILE | --names A,B,C] [--index SPY] "
               "[--start YYYY-MM-DD] [--end YYYY-MM-DD] [--top-n 50] "
               "[--delta 0.40] [--tenor-days 90] [--theta-per-name 10.0] "
               "[--min-names 4] [--no-hedge] [--frictions] [--threads N]\n");
}

[[nodiscard]] bool parse_args(int argc, char **argv, Args &a) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto nv = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : ""; };
    if (arg == "--db") {
      a.db = nv();
    } else if (arg == "--out") {
      a.out = nv();
    } else if (arg == "--universe") {
      a.universe = nv();
    } else if (arg == "--names") {
      a.names = split(nv(), ',');
    } else if (arg == "--index") {
      a.index_symbol = nv();
    } else if (arg == "--start") {
      a.start = nv();
    } else if (arg == "--end") {
      a.end = nv();
    } else if (arg == "--top-n") {
      a.top_n = static_cast<std::size_t>(std::strtoul(nv(), nullptr, 10));
    } else if (arg == "--delta") {
      a.delta = std::strtod(nv(), nullptr);
    } else if (arg == "--tenor-days") {
      a.tenor_days = std::strtod(nv(), nullptr);
    } else if (arg == "--theta-per-name") {
      a.theta_per_name = std::strtod(nv(), nullptr);
    } else if (arg == "--min-names") {
      a.min_names = static_cast<std::size_t>(std::strtoul(nv(), nullptr, 10));
    } else if (arg == "--no-hedge") {
      a.hedge = false;
    } else if (arg == "--frictions") {
      a.frictions = true;
    } else if (arg == "--threads") {
      a.threads = static_cast<unsigned>(std::strtoul(nv(), nullptr, 10));
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return false;
    }
  }
  if (a.db.empty()) {
    std::fprintf(stderr, "--db is required\n");
    return false;
  }
  if (a.out.empty()) {
    a.out = (fs::temp_directory_path() / "atx-spy-dispersion-pnl").string();
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    usage();
    return 2;
  }

  // ── Resolve the universe: explicit --names, else the --universe fixture. ──
  std::vector<std::string> names = args.names;
  if (names.empty()) {
    if (args.universe.empty()) {
      std::fprintf(stderr, "one of --names or --universe is required\n");
      return 2;
    }
    auto u = read_universe_symbols(args.universe, args.index_symbol);
    if (!u) {
      std::fprintf(stderr, "read_universe_symbols: %s\n", u.error().to_string().c_str());
      return 1;
    }
    names = std::move(*u);
  }
  const std::string dropped_alphabet = dedup_alphabet(names);
  if (names.size() > args.top_n) {
    names.resize(args.top_n); // file is weight-descending -> keep the heaviest N
  }

  auto db = SurfaceDb::open(args.db);
  if (!db) {
    std::fprintf(stderr, "SurfaceDb::open(%s): %s\n", args.db.c_str(),
                 db.error().to_string().c_str());
    return 1;
  }
  auto clock = windowed_clock(*db, args.start, args.end);
  if (!clock) {
    std::fprintf(stderr, "windowed_clock: %s\n", clock.error().to_string().c_str());
    return 1;
  }

  DispersionStrangleConfig cfg;
  cfg.names = names;
  cfg.index_symbol = args.index_symbol;
  cfg.target_abs_delta = args.delta;
  cfg.tenor_days = args.tenor_days;
  cfg.theta_per_name_daily = args.theta_per_name;
  cfg.hold_to_expiry = true; // acceptance strategy holds to expiry
  cfg.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, args.min_names};
  if (args.hedge) {
    cfg.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};
  }
  auto spec = make_dispersion_strangle_spec(cfg);
  if (!spec) {
    std::fprintf(stderr, "make_dispersion_strangle_spec: %s\n", spec.error().to_string().c_str());
    return 1;
  }

  DeclarativeStrategy strat(*spec);
  RunConfig rc;
  rc.snapshot_cache = std::make_shared<SnapshotCache>();
  rc.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  rc.price.n_threads = args.threads;
  if (args.frictions) {
    rc.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
    rc.frictions.half_spread_bps = 5.0;
    rc.frictions.per_contract_cost = 0.65;
  }

  const auto t0 = std::chrono::steady_clock::now();
  auto res = run_backtest(*clock, strat, rc);
  const auto t1 = std::chrono::steady_clock::now();
  if (!res) {
    std::fprintf(stderr, "run_backtest: %s\n", res.error().to_string().c_str());
    return 1;
  }
  const BacktestResult &r = *res;
  const TearSheet ts = tearsheet(r);
  const double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double steps_per_s = (wall_ms > 0.0) ? 1000.0 * static_cast<double>(r.size()) / wall_ms : 0.0;
  const double peak_lots =
      r.size() ? *std::max_element(r.n_open_lots.begin(), r.n_open_lots.end()) : 0.0;

  std::error_code ec;
  fs::create_directories(args.out, ec);

  // ── The self-describing PnL-track TSV (D5): identity + config + headline
  //    stats + engine timing + surface stats in the meta header; the renderer
  //    titles the chart and fills its stats box from these keys. ──
  const Meta meta = {
      {"strategy", "spy_dispersion_vega_flat"},
      {"names", join(cfg.names)},
      {"index_symbol", cfg.index_symbol},
      {"data_source", "surface_db"},
      {"db_root", db->root()},
      {"db_generation", std::to_string(db->generation())},
      {"db_partitions", std::to_string(db->partitions().size())},
      {"window_start", clock->refs().front().date},
      {"window_end", clock->refs().back().date},
      {"n_steps", std::to_string(r.size())},
      {"n_names", std::to_string(cfg.names.size())},
      {"delta_target", fmt_num(cfg.target_abs_delta)},
      {"tenor_days", fmt_num(cfg.tenor_days)},
      {"theta_per_name_daily", fmt_num(cfg.theta_per_name_daily)},
      {"entry_every_n_days", std::to_string(cfg.entry_every_n_days)},
      {"multiplier", "100"},
      {"hold_to_expiry", "on"},
      {"hedge", args.hedge ? "delta_to_zero_daily" : "off"},
      {"frictions", args.frictions ? "on" : "off"},
      {"missing_policy", "drop_renormalize"},
      {"min_names", std::to_string(cfg.missing.min_names)},
      {"dropped_alphabet_class", dropped_alphabet.empty() ? "none" : dropped_alphabet},
      {"total_return", fmt_num(ts.total_return)},
      {"ann_return", fmt_num(ts.ann_return)},
      {"ann_vol", fmt_num(ts.ann_vol)},
      {"sharpe", fmt_num(ts.sharpe)},
      {"max_drawdown", fmt_num(ts.max_drawdown)},
      {"hit_rate", fmt_num(ts.hit_rate)},
      {"avg_gross_vega", fmt_num(ts.avg_gross_vega)},
      {"avg_gross_gamma", fmt_num(ts.avg_gross_gamma)},
      {"return_on_gross_vega", fmt_num(ts.return_on_gross_vega)},
      {"peak_open_lots", fmt_num(peak_lots)},
      {"wall_clock_ms", fmt_num(wall_ms)},
      {"steps_per_s", fmt_num(steps_per_s)},
  };

  const std::string tsv_path = (fs::path(args.out) / "pnl_track.tsv").string();
  const Status st = write_backtest_pnl_tsv(r, meta, tsv_path);
  if (!st) {
    std::fprintf(stderr, "write_backtest_pnl_tsv: %s\n", st.error().to_string().c_str());
    return 1;
  }

  const std::string dropped_note =
      dropped_alphabet.empty() ? std::string{}
                               : (" | dropped " + dropped_alphabet + " (Alphabet dedup)");
  std::printf("=== SPY vega-flat dispersion PnL track ===\n"
              "db: %s (gen %llu) | window: %s .. %s (%zu steps)\n"
              "names: %zu vs index %s | delta=%.2f tenor=%.0fd theta/name=$%.2f/day held-to-expiry "
              "hedge=%s frictions=%s%s\n"
              "[timing] run_backtest: %.1f ms over %zu steps (%.1f steps/s) peak_lots=%.0f\n"
              "[tearsheet] total_return=%.2f sharpe=%.3f ann_vol=%.2f max_drawdown=%.2f "
              "hit_rate=%.3f avg_gross_vega=%.1f\n"
              "[wrote] %s\n"
              "  render: python tools/spy_dispersion_pnl_report.py %s\n",
              db->root().c_str(), static_cast<unsigned long long>(db->generation()),
              clock->refs().front().date.c_str(), clock->refs().back().date.c_str(), r.size(),
              cfg.names.size(), cfg.index_symbol.c_str(), cfg.target_abs_delta, cfg.tenor_days,
              cfg.theta_per_name_daily, args.hedge ? "on" : "off", args.frictions ? "on" : "off",
              dropped_note.c_str(), wall_ms, r.size(), steps_per_s, peak_lots, ts.total_return,
              ts.sharpe, ts.ann_vol, ts.max_drawdown, ts.hit_rate, ts.avg_gross_vega,
              tsv_path.c_str(), tsv_path.c_str());
  return 0;
}
