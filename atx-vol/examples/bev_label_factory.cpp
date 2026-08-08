// bev_label_factory.cpp — offline breakeven-vol label-factory driver (THEO-6).
//
// Walks a SurfaceDb date range, builds one breakeven-vol solve job per
// (entry date, expiry, strike, side) that survives the strike-lattice delta
// filter, solves the whole batch (Task 4's `solve_breakeven_batch`), and
// writes a byte-deterministic, sorted TSV whose target column is
// `log_ratio = ln(sigma_be / sigma_entry_iv)` — the model target from the
// research doc's Section 8.2. This driver stays CARRY-ONLY: it does not join
// an earnings calendar or count events (that happens Python-side at training
// time), and it does not touch bev_replay_pnl / solve_breakeven_vol / the
// batch runner / the path loader, only fans work into them.
//
//   bev_label_factory --db <root> --uid <symbol> --entry-start <date>
//       --entry-end <date> --tenor-days <n> --delta-lo 0.05 --delta-hi 0.95
//       --dividends <tsv> --out labels.tsv [--threads N]
//
// Flow, per entry date in [--entry-start, --entry-end]:
//   1. Open that date's partition and reconstruct an OWNED PricedSurface for
//      --uid (SurfaceDb::open_partition -> SurfaceArchiveV2::reconstruct_symbol).
//      An owned reconstruction is required (not the MarketSnapshot zero-copy
//      view route) because only PricedSurface::context() exposes the discrete
//      fitted tenor pillars the "closest to --tenor-days" selection needs.
//   2. Pick the pillar T whose calendar-day tenor is closest to --tenor-days
//      (ties broken toward the shorter tenor) and derive a nominal expiry_ns
//      from it.
//   3. For each side and each delta on a fixed 5%-step lattice inside
//      [--delta-lo, --delta-hi], resolve a strike via `resolve_strike_by_delta`
//      and re-check the resolved strike's actual |delta| (the surface's own
//      analytic delta accessor) falls in range — the wing ill-conditioning
//      filter (research doc Section 7.4).
//   4. Skip any candidate whose served entry iv(K, T) is unavailable/non-finite
//      (it cannot produce a target) or whose replay path fails to load
//      (Task 5's `load_bev_path`, LastSessionAtOrBefore snap).
// Every surviving candidate becomes a `BevJob`; the whole set solves in ONE
// `solve_breakeven_batch` call (deterministic across --threads, Task 4's own
// contract). Rows with a rejected solve (status_ok == 0) are dropped; rows
// with a nonzero BevFlag (NoBracket / ExercisedEarly / MaxIter) are KEPT — the
// flag is data for the trainer to filter on, not a reason to drop a label.
//
// Exit codes: 2 bad args, 1 runtime error (including "produced zero labels" —
// an all-skip run is not a successful one). OFF by default (ATX_BUILD_EXAMPLES).

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/backtest.hpp"         // Clock
#include "atx/vol/breakeven.hpp"        // BevDayState/Spec/Job/LabelFrame, solve_breakeven_batch,
                                        // load_bev_path, BevExpirySnap
#include "atx/vol/portfolio_pricer.hpp" // kNsPerYear, SurfaceRef
#include "atx/vol/priced_surface.hpp"   // PricedSurface
#include "atx/vol/rates_curve.hpp"      // DividendEvent
#include "atx/vol/strategy.hpp"         // resolve_strike_by_delta
#include "atx/vol/surface_archive.hpp"  // SurfaceArchiveV2
#include "atx/vol/surface_db.hpp"       // SurfaceDb
#include "atx/vol/surface_parity.hpp"   // SliceContext
#include "atx/vol/types.hpp"            // Result, Status, Side, ErrorCode

using namespace atx::vol;
using atx::core::Err;
using atx::core::Ok;

namespace {

// ── CLI arg struct ────────────────────────────────────────────────────────
//
// Deliberately header-free: `BevFactoryArgs` is a small, CLI-only bag with no
// reuse value beyond this one driver and its one gate test, so it does not
// warrant a dedicated `.hpp`. The gate test reaches it (and `run_bev_label_
// factory` below) by `#define ATX_BEV_LABEL_FACTORY_NO_MAIN` followed by a
// textual `#include` of THIS file — a macro-guarded re-inclusion of the whole
// example TU, with `main()` compiled out. That include appears in exactly one
// test TU (see tests/bev_label_factory_gate_test.cpp's own banner), so there
// is no second declaration of this struct anywhere to drift out of sync with
// this one (no ODR risk) — an explicit, controller-approved access mechanism
// for a driver whose logic must stay callable without argv/exit.

struct BevFactoryArgs {
  std::string db;
  std::string uid;
  std::string entry_start;
  std::string entry_end;
  int tenor_days{30};
  double delta_lo{0.05};
  double delta_hi{0.95};
  std::string dividends;
  std::string out;
  unsigned n_threads{0}; // 0 = auto (atx_auto_worker_count())
};

// [[maybe_unused]]: only `main()` (below, compiled out under
// ATX_BEV_LABEL_FACTORY_NO_MAIN) calls this -- the gate test exercises
// parse_args()'s rejection path directly instead of via usage()'s stderr text.
[[maybe_unused]] void usage() {
  std::fprintf(stderr, "usage: bev_label_factory --db ROOT --uid SYMBOL --entry-start DATE "
                       "--entry-end DATE\n    --tenor-days N --delta-lo X --delta-hi X "
                       "--dividends TSV --out FILE\n    [--threads N]\n");
}

// argv -> BevFactoryArgs, plus the required-field and range validation. Left
// OUTSIDE the ATX_BEV_LABEL_FACTORY_NO_MAIN guard (unlike `main` below) on
// purpose: the gate test exercises this parsing directly (a canned argv
// vector in, a field-for-field BevFactoryArgs out; a missing required flag
// rejected) rather than only indirectly through a real process's argv/exit.
bool parse_args(int argc, char **argv, BevFactoryArgs &args) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag{argv[i]};
    auto next = [&](std::string &dst) {
      if (i + 1 >= argc)
        return false;
      dst = argv[++i];
      return true;
    };
    std::string v;
    if (flag == "--db" && next(v)) {
      args.db = v;
    } else if (flag == "--uid" && next(v)) {
      args.uid = v;
    } else if (flag == "--entry-start" && next(v)) {
      args.entry_start = v;
    } else if (flag == "--entry-end" && next(v)) {
      args.entry_end = v;
    } else if (flag == "--tenor-days" && next(v)) {
      args.tenor_days = std::atoi(v.c_str());
    } else if (flag == "--delta-lo" && next(v)) {
      args.delta_lo = std::atof(v.c_str());
    } else if (flag == "--delta-hi" && next(v)) {
      args.delta_hi = std::atof(v.c_str());
    } else if (flag == "--dividends" && next(v)) {
      args.dividends = v;
    } else if (flag == "--out" && next(v)) {
      args.out = v;
    } else if (flag == "--threads" && next(v)) {
      args.n_threads = static_cast<unsigned>(std::atoi(v.c_str()));
    } else {
      std::fprintf(stderr, "unknown/incomplete flag: %.*s\n", static_cast<int>(flag.size()),
                   flag.data());
      return false;
    }
  }
  if (args.db.empty() || args.uid.empty() || args.entry_start.empty() || args.entry_end.empty() ||
      args.dividends.empty() || args.out.empty()) {
    std::fprintf(stderr, "--db/--uid/--entry-start/--entry-end/--dividends/--out are required\n");
    return false;
  }
  if (args.tenor_days <= 0 || !(args.delta_lo > 0.0) || !(args.delta_hi < 1.0) ||
      !(args.delta_lo < args.delta_hi)) {
    std::fprintf(stderr, "invalid argument values (need tenor-days>0, 0<delta-lo<delta-hi<1)\n");
    return false;
  }
  return true;
}

// ── Dividends TSV loader: mirrors earnings_forecast_loader.cpp's parsing
// style (ifstream, string_view line slicing, no exceptions) for the simpler
// two-column "epoch_ns<TAB or space>amount" schema, `#` comments skipped. ──

[[nodiscard]] std::string_view rstrip_cr(std::string_view v) noexcept {
  if (!v.empty() && v.back() == '\r') {
    v.remove_suffix(1);
  }
  return v;
}

[[nodiscard]] std::string_view trim(std::string_view v) noexcept {
  const std::size_t start = v.find_first_not_of(" \t");
  if (start == std::string_view::npos) {
    return {};
  }
  const std::size_t end = v.find_last_not_of(" \t");
  return v.substr(start, end - start + 1);
}

[[nodiscard]] Result<std::vector<DividendEvent>> load_dividends_tsv(const std::string &path) {
  std::ifstream in{path, std::ios::binary};
  if (!in) {
    return Err(ErrorCode::IoError, "load_dividends_tsv: cannot open '" + path + "'");
  }
  std::vector<DividendEvent> events;
  std::string line;
  // Bounded by the file's own line count -- std::getline terminates at EOF.
  while (std::getline(in, line)) {
    const std::string_view raw = trim(rstrip_cr(line));
    if (raw.empty() || raw.front() == '#') {
      continue;
    }
    const std::size_t sep = raw.find_first_of(" \t");
    if (sep == std::string_view::npos) {
      return Err(ErrorCode::InvalidArgument,
                 "load_dividends_tsv: malformed line (need '<ns> <amount>'): '" + line + "'");
    }
    const std::string_view ns_field = raw.substr(0, sep);
    const std::string_view amount_field = trim(raw.substr(sep + 1));
    if (ns_field.empty() || amount_field.empty()) {
      return Err(ErrorCode::InvalidArgument, "load_dividends_tsv: malformed line: '" + line + "'");
    }
    DividendEvent ev{};
    const auto ns_r =
        std::from_chars(ns_field.data(), ns_field.data() + ns_field.size(), ev.ex_date_ns);
    if (ns_r.ec != std::errc{} || ns_r.ptr != ns_field.data() + ns_field.size()) {
      return Err(ErrorCode::InvalidArgument,
                 "load_dividends_tsv: unparseable epoch-ns field: '" + std::string{ns_field} + "'");
    }
    const auto amt_r =
        std::from_chars(amount_field.data(), amount_field.data() + amount_field.size(), ev.amount);
    if (amt_r.ec != std::errc{} || amt_r.ptr != amount_field.data() + amount_field.size()) {
      return Err(ErrorCode::InvalidArgument, "load_dividends_tsv: unparseable amount field: '" +
                                                 std::string{amount_field} + "'");
    }
    events.push_back(ev);
  }
  return Ok(std::move(events));
}

// ── Job accumulation ─────────────────────────────────────────────────────

// A5% delta lattice step: the CLI exposes only the [delta-lo, delta-hi]
// FILTER bounds (research doc Section 7.4), not a granularity knob, so the
// candidate targets sampled inside that window are a fixed, documented
// constant rather than an undocumented magic number buried in the loop.
constexpr double kDeltaGridStep = 0.05;

// Floor applied to each session's remaining tenor before load_bev_path probes
// q_eff_at -- about one calendar day, mirroring breakeven_test.cpp's
// kBevPathProbeFloor convention.
constexpr double kTenorProbeYears = 1.0 / 365.25;

// One surviving candidate: a strike/side/expiry job plus everything the TSV
// row needs that solve_breakeven_batch's SoA frame does not itself carry.
struct PendingJob {
  std::int64_t entry_ts_ns{0};
  double strike{0.0};
  std::int64_t expiry_ns{0}; // == the loaded path's settle_ts_ns (Task 5)
  Side side{Side::Call};
  double sigma_entry_iv{0.0};
  bool snapped{false};
};

struct RunCounters {
  std::size_t n_entry_dates{0};
  std::size_t n_entry_dates_skipped{0};
  std::size_t n_candidates_considered{0};
  std::size_t n_candidates_prebuild_skipped{0};
  std::size_t n_jobs_solved_ok{0};
  std::size_t n_rows_written{0};
};

// The fitted pillar (year-fraction T) whose calendar-day tenor is closest to
// `tenor_days`, ties broken toward the SHORTER tenor. `std::nullopt` if the
// surface carries no slices.
[[nodiscard]] std::optional<double> pick_tenor_pillar(const PricedSurface &surf,
                                                      int tenor_days) noexcept {
  bool found = false;
  double best_diff = 0.0;
  double best_T = 0.0;
  // Bounded by n_slices() (a handful of term-structure pillars).
  for (const SliceContext &sc : surf.context()) {
    const double days = sc.T * 365.25;
    const double diff = std::fabs(days - static_cast<double>(tenor_days));
    if (!found || diff < best_diff || (diff == best_diff && sc.T < best_T)) {
      found = true;
      best_diff = diff;
      best_T = sc.T;
    }
  }
  if (!found) {
    return std::nullopt;
  }
  return best_T;
}

// Build every surviving (strike, side) candidate for one entry date, loading
// its replay path (Task 5) and appending to `pending`/`owned_paths` (index-
// aligned). Soft failures (unreachable delta, invalid iv, path load failure)
// skip just that candidate; nothing here is fatal to the whole run.
void collect_entry_date_jobs(const Clock &full_clock, const BevFactoryArgs &args,
                             const PricedSurface &surf, std::int64_t entry_ts_ns, double T,
                             RunCounters &counters, std::vector<PendingJob> &pending,
                             std::vector<BevPath> &owned_paths) {
  const auto nominal_expiry_ns =
      entry_ts_ns + static_cast<std::int64_t>(std::llround(T * kNsPerYear));

  const int lo_i = static_cast<int>(std::ceil(args.delta_lo / kDeltaGridStep - 1e-9));
  const int hi_i = static_cast<int>(std::floor(args.delta_hi / kDeltaGridStep + 1e-9));
  // Bounded: at most 1/kDeltaGridStep + 1 targets, times two sides.
  for (int i = lo_i; i <= hi_i; ++i) {
    const double target = static_cast<double>(i) * kDeltaGridStep;
    if (!(target > 0.0 && target < 1.0)) {
      continue; // resolve_strike_by_delta's open-interval domain
    }
    for (const Side side : {Side::Call, Side::Put}) {
      ++counters.n_candidates_considered;
      const Result<double> k = resolve_strike_by_delta(surf, T, side, target);
      if (!k.has_value()) {
        ++counters.n_candidates_prebuild_skipped;
        continue;
      }
      const Result<double> actual_delta = surf.delta(*k, T, side);
      if (!actual_delta.has_value()) {
        ++counters.n_candidates_prebuild_skipped;
        continue;
      }
      const double ad = std::fabs(*actual_delta);
      if (!(ad >= args.delta_lo && ad <= args.delta_hi)) {
        ++counters.n_candidates_prebuild_skipped; // wing ill-conditioning filter
        continue;
      }
      const double sigma_entry_iv = surf.iv(*k, T);
      if (!(sigma_entry_iv > 0.0) || !std::isfinite(sigma_entry_iv)) {
        ++counters.n_candidates_prebuild_skipped; // can't produce a target
        continue;
      }
      Result<BevPath> path = load_bev_path(full_clock, args.uid, entry_ts_ns, nominal_expiry_ns,
                                           kTenorProbeYears, BevExpirySnap::LastSessionAtOrBefore);
      if (!path.has_value()) {
        ++counters.n_candidates_prebuild_skipped;
        continue;
      }
      pending.push_back(PendingJob{.entry_ts_ns = entry_ts_ns,
                                   .strike = *k,
                                   .expiry_ns = path->settle_ts_ns,
                                   .side = side,
                                   .sigma_entry_iv = sigma_entry_iv,
                                   .snapped = path->snapped});
      owned_paths.push_back(std::move(*path));
    }
  }
}

// ── Output row + TSV writer (mirrors tools/tearsheet.hpp's `# key=value`
// meta header + %.17g round-trip-precision double formatting) ──────────────

struct LabelRow {
  std::int64_t entry_ts_ns{0};
  double strike{0.0};
  std::int64_t expiry_ns{0};
  Side side{Side::Call};
  double sigma_be{0.0};
  double sigma_entry_iv{0.0};
  double log_ratio{0.0};
  double premium{0.0};
  double vega{0.0};
  std::uint16_t n_days{0};
  std::uint8_t iters{0};
  std::uint8_t flag{0};
  bool snapped{false};
};

void append_sanitized(std::string &out, std::string_view s) {
  for (const char c : s) {
    out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
  }
}

void append_meta_header(std::string &out,
                        std::span<const std::pair<std::string, std::string>> meta) {
  for (const auto &[k, v] : meta) {
    out += "# ";
    append_sanitized(out, k);
    out += '=';
    append_sanitized(out, v);
    out += '\n';
  }
}

// Doubles use "%.17g" -- std::numeric_limits<double>::max_digits10 significant
// digits, the minimum that round-trips any IEEE-754 double bit-exactly back
// through strtod -- via snprintf into a fixed buffer (no stream/locale state,
// so the byte output cannot depend on the process locale). '.' is always the
// decimal separator under "C" numeric formatting, which snprintf uses
// regardless of the global std::locale.
void append_double(std::string &out, double v) {
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%.17g", v);
  out.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

void append_i64(std::string &out, std::int64_t v) {
  char buf[32];
  const int len = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
  out.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

void append_u32(std::string &out, std::uint32_t v) {
  char buf[16];
  const int len = std::snprintf(buf, sizeof buf, "%u", v);
  out.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

// Appends the header row + one row per `rows` entry (already sorted).
void append_rows_tsv(std::string &out, std::string_view uid, std::span<const LabelRow> rows) {
  out += "entry_ts_ns\tuid\tstrike\texpiry_ns\tside\tsigma_be\tsigma_entry_iv\tlog_ratio\t"
         "premium\tvega\tn_days\titers\tflag\tsnapped\n";
  for (const LabelRow &r : rows) {
    append_i64(out, r.entry_ts_ns);
    out += '\t';
    append_sanitized(out, uid);
    out += '\t';
    append_double(out, r.strike);
    out += '\t';
    append_i64(out, r.expiry_ns);
    out += '\t';
    out += (r.side == Side::Call) ? 'C' : 'P';
    out += '\t';
    append_double(out, r.sigma_be);
    out += '\t';
    append_double(out, r.sigma_entry_iv);
    out += '\t';
    append_double(out, r.log_ratio);
    out += '\t';
    append_double(out, r.premium);
    out += '\t';
    append_double(out, r.vega);
    out += '\t';
    append_u32(out, r.n_days);
    out += '\t';
    append_u32(out, r.iters);
    out += '\t';
    append_u32(out, r.flag);
    out += '\t';
    out += r.snapped ? '1' : '0';
    out += '\n';
  }
}

[[nodiscard]] Status write_labels_tsv(std::string_view path, std::string_view uid,
                                      std::span<const std::pair<std::string, std::string>> meta,
                                      std::span<const LabelRow> rows) {
  std::string out;
  append_meta_header(out, meta);
  append_rows_tsv(out, uid, rows);
  std::ofstream os(std::string(path), std::ios::binary | std::ios::trunc);
  if (!os) {
    return Err(ErrorCode::IoError, "write_labels_tsv: cannot open '" + std::string(path) + "'");
  }
  os.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!os) {
    return Err(ErrorCode::IoError, "write_labels_tsv: write failed");
  }
  return Ok();
}

char num_buf[64];
std::string fmt_num(double v) {
  std::snprintf(num_buf, sizeof num_buf, "%.10g", v);
  return num_buf;
}

} // namespace

// Library-level entry point (extracted so the gate test can call it directly
// without going through argv/exit). See the file banner for the full flow.
[[nodiscard]] Result<int> run_bev_label_factory(const BevFactoryArgs &args) {
  ATX_TRY(const std::vector<DividendEvent> dividends, load_dividends_tsv(args.dividends));

  ATX_TRY(SurfaceDb db, SurfaceDb::open(args.db));
  ATX_TRY(const Clock full_clock, Clock::from_surface_db(db));
  ATX_TRY(const Clock entry_clock, full_clock.between(args.entry_start, args.entry_end));

  RunCounters counters;
  std::vector<PendingJob> pending;
  std::vector<BevPath> owned_paths;

  // Bounded by entry_clock.size() -- the requested entry-date window.
  for (const SnapshotRef &ref : entry_clock.refs()) {
    ++counters.n_entry_dates;
    const Result<std::int64_t> entry_ts_ns = db.session_ts(ref.date);
    const Result<SurfaceArchiveV2> archive = db.open_partition(ref.date);
    if (!entry_ts_ns.has_value() || !archive.has_value()) {
      ++counters.n_entry_dates_skipped;
      continue;
    }
    const Result<PricedSurface> surf = archive->reconstruct_symbol(args.uid);
    if (!surf.has_value()) {
      ++counters.n_entry_dates_skipped;
      continue;
    }
    const std::optional<double> T = pick_tenor_pillar(*surf, args.tenor_days);
    if (!T.has_value()) {
      ++counters.n_entry_dates_skipped;
      continue;
    }
    collect_entry_date_jobs(full_clock, args, *surf, *entry_ts_ns, *T, counters, pending,
                            owned_paths);
  }

  // Build the batch's job spans only once every path is stable in
  // `owned_paths` -- BevJob::path/dividends are non-owning, so nothing may
  // reference `owned_paths[i].days` before that vector's final contents (and
  // therefore each element's OWN heap-backed `days` buffer) are settled.
  std::vector<BevJob> jobs;
  jobs.reserve(pending.size());
  for (std::size_t i = 0; i < pending.size(); ++i) {
    jobs.push_back(BevJob{.path = owned_paths[i].days,
                          .spec = BevSpec{.strike = pending[i].strike,
                                          .expiry_ns = pending[i].expiry_ns,
                                          .side = pending[i].side},
                          .dividends = dividends});
  }

  ATX_TRY(const BevLabelFrame frame, solve_breakeven_batch(jobs, BevSolveConfig{}, args.n_threads));

  std::vector<LabelRow> rows;
  rows.reserve(pending.size());
  for (std::size_t i = 0; i < pending.size(); ++i) {
    if (frame.status_ok[i] == 0) {
      continue; // input-rejected job: not a label
    }
    ++counters.n_jobs_solved_ok;
    const double sigma_be = frame.sigma_be[i];
    const double sigma_entry_iv = pending[i].sigma_entry_iv;
    if (!(sigma_be > 0.0) || !(sigma_entry_iv > 0.0)) {
      continue; // guards the ln() below against a non-positive argument
    }
    const double log_ratio = std::log(sigma_be / sigma_entry_iv);
    if (!std::isfinite(log_ratio)) {
      continue;
    }
    rows.push_back(LabelRow{.entry_ts_ns = pending[i].entry_ts_ns,
                            .strike = pending[i].strike,
                            .expiry_ns = pending[i].expiry_ns,
                            .side = pending[i].side,
                            .sigma_be = sigma_be,
                            .sigma_entry_iv = sigma_entry_iv,
                            .log_ratio = log_ratio,
                            .premium = frame.premium_at_be[i],
                            .vega = frame.vega_at_be[i],
                            .n_days = frame.n_days[i],
                            .iters = frame.iters[i],
                            .flag = frame.flag[i],
                            .snapped = pending[i].snapped});
  }
  counters.n_rows_written = rows.size();

  if (rows.empty()) {
    return Err(ErrorCode::NotFound,
               "run_bev_label_factory: produced zero labels (entry_dates=" +
                   std::to_string(counters.n_entry_dates) +
                   " skipped=" + std::to_string(counters.n_entry_dates_skipped) +
                   " candidates=" + std::to_string(counters.n_candidates_considered) +
                   " prebuild_skipped=" + std::to_string(counters.n_candidates_prebuild_skipped) +
                   " solved_ok=" + std::to_string(counters.n_jobs_solved_ok) + ")");
  }

  // Byte-stable sort: (entry_ts_ns, expiry_ns, strike, side), side compared by
  // its Side::Call=0 < Side::Put=1 ordinal (== 'C' < 'P' in the emitted
  // column). std::sort is a deterministic algorithm, so two runs over the
  // same (byte-identical, same insertion order) `rows` vector produce
  // byte-identical output regardless of instability on ties.
  std::sort(rows.begin(), rows.end(), [](const LabelRow &a, const LabelRow &b) {
    if (a.entry_ts_ns != b.entry_ts_ns) {
      return a.entry_ts_ns < b.entry_ts_ns;
    }
    if (a.expiry_ns != b.expiry_ns) {
      return a.expiry_ns < b.expiry_ns;
    }
    if (a.strike != b.strike) {
      return a.strike < b.strike;
    }
    return static_cast<std::uint8_t>(a.side) < static_cast<std::uint8_t>(b.side);
  });

  // Metadata: args echo + deterministic run counters only -- no timestamps,
  // hostnames, or absolute build paths, so two runs with identical args are
  // byte-identical files (the gate's memcmp requirement).
  const std::vector<std::pair<std::string, std::string>> meta = {
      {"tool", "bev_label_factory"},
      {"db", args.db},
      {"uid", args.uid},
      {"entry_start", args.entry_start},
      {"entry_end", args.entry_end},
      {"tenor_days", std::to_string(args.tenor_days)},
      {"delta_lo", fmt_num(args.delta_lo)},
      {"delta_hi", fmt_num(args.delta_hi)},
      {"dividends", args.dividends},
      {"threads", std::to_string(args.n_threads)},
      {"n_entry_dates", std::to_string(counters.n_entry_dates)},
      {"n_entry_dates_skipped", std::to_string(counters.n_entry_dates_skipped)},
      {"n_candidates_considered", std::to_string(counters.n_candidates_considered)},
      {"n_candidates_prebuild_skipped", std::to_string(counters.n_candidates_prebuild_skipped)},
      {"n_jobs_solved_ok", std::to_string(counters.n_jobs_solved_ok)},
      {"n_rows_written", std::to_string(counters.n_rows_written)},
  };

  ATX_TRY_VOID(write_labels_tsv(args.out, args.uid, meta, rows));

  std::printf("[bev_label_factory] entry_dates=%zu (skipped %zu) candidates=%zu "
              "(prebuild_skipped %zu) solved_ok=%zu rows=%zu -> %s\n",
              counters.n_entry_dates, counters.n_entry_dates_skipped,
              counters.n_candidates_considered, counters.n_candidates_prebuild_skipped,
              counters.n_jobs_solved_ok, counters.n_rows_written, args.out.c_str());
  return Ok(0);
}

#ifndef ATX_BEV_LABEL_FACTORY_NO_MAIN
int main(int argc, char **argv) {
  BevFactoryArgs args;
  if (!parse_args(argc, argv, args)) {
    usage();
    return 2;
  }
  const Result<int> rc = run_bev_label_factory(args);
  if (!rc.has_value()) {
    std::fprintf(stderr, "bev_label_factory: %s\n", rc.error().to_string().c_str());
    return 1;
  }
  return *rc;
}
#endif
