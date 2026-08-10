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
//       --dividends <tsv> --out labels.tsv [--threads N] [--events <tsv>]
//
// --events <tsv> is OPTIONAL and, in this task, feature-only: one ISO
// (YYYY-MM-DD) announcement date per line, `#` comment lines and blank lines
// skipped, CR tolerated. Timestamps are midnight UTC of the announcement
// date -- a deliberate day-resolution approximation. When omitted (or the
// path is empty), no calendar is loaded and the per-candidate event count is
// NaN. The label TSV's row schema is UNCHANGED by this flag; the event count
// is only stamped onto the in-memory `PendingJob` for a later consumer.
//
// Also feature-only: a one-time spot-history pre-pass (`load_spot_history`,
// Task F-2) walks the corpus once up front to build a trailing realized-vol
// panel (`RvEstimator::CloseToClose`, 252 annualization, up to
// `kRvHistoryBars` spot-mirror closes) for EVERY entry date, ending at and
// including that date's own session close. The resulting `rv_21d`/`rv_63d`
// are stamped onto every surviving candidate's `PendingJob`, NaN when the
// trailing history is too short — again a row-schema-invisible, in-memory-
// only feature for a later consumer, not a TSV column in this task. A
// pre-pass load failure IS fatal (unlike a per-date RV shortfall): a corpus
// that can serve surfaces but not spots is broken.
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
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/backtest.hpp"         // Clock
#include "atx/vol/breakeven.hpp"        // BevDayState/Spec/Job/LabelFrame, solve_breakeven_batch,
                                        // load_bev_path, BevExpirySnap
#include "atx/vol/event_vol.hpp"        // EventSchedule, count_events_at
#include "atx/vol/portfolio_pricer.hpp" // kNsPerYear, SurfaceRef
#include "atx/vol/priced_surface.hpp"   // PricedSurface
#include "atx/vol/rates_curve.hpp"      // DividendEvent
#include "atx/vol/realized_vol.hpp"     // OhlcBar, RvEstimator, RvPanel, realized_vol_panel
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
  std::string events; // optional; empty = no events calendar (Task F-1)
  std::string out;
  unsigned n_threads{0}; // 0 = auto (atx_auto_worker_count())
};

// [[maybe_unused]]: only `main()` (below, compiled out under
// ATX_BEV_LABEL_FACTORY_NO_MAIN) calls this -- the gate test exercises
// parse_args()'s rejection path directly instead of via usage()'s stderr text.
[[maybe_unused]] void usage() {
  std::fprintf(stderr, "usage: bev_label_factory --db ROOT --uid SYMBOL --entry-start DATE "
                       "--entry-end DATE\n    --tenor-days N --delta-lo X --delta-hi X "
                       "--dividends TSV --out FILE\n    [--threads N] [--events TSV]\n");
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
    } else if (flag == "--events" && next(v)) {
      args.events = v;
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

// ── Events-calendar TSV loader (Task F-1) ────────────────────────────────
//
// `days_from_civil`/`civil_from_days` mirror databento_spy_dispersion_
// definitions.cpp's own local, self-contained copies of Howard Hinnant's
// civil-days algorithm (rather than pulling in atx/core/datetime.hpp) --
// same precedent, same reason: this driver is deliberately header-free (see
// the file banner) and that example keeps its own local date kernel even
// though it separately depends on atx/core/datetime.hpp for other things.

[[nodiscard]] std::int64_t days_from_civil(int year, unsigned month, unsigned day) noexcept {
  year -= month <= 2U;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153U * (month > 2U ? month - 3U : month + 9U) + 2U) / 5U + day - 1U;
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct Civil {
  int year{0};
  unsigned month{0};
  unsigned day{0};
};

[[nodiscard]] Civil civil_from_days(std::int64_t serial) noexcept {
  serial += 719468;
  const std::int64_t era = (serial >= 0 ? serial : serial - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(serial - era * 146097);
  const unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
  const int year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  const unsigned mp = (5U * doy + 2U) / 153U;
  const unsigned day = doy - (153U * mp + 2U) / 5U + 1U;
  const unsigned month = mp < 10U ? mp + 3U : mp - 9U;
  return Civil{year + static_cast<int>(month <= 2U), month, day};
}

// `YYYY-MM-DD` -> epoch ns at 00:00 UTC. Rejects out-of-range months/days AND
// calendar-invalid combinations (e.g. 2026-02-30) via a civil-days round-trip
// (parse -> serial -> parse back, compare fields).
[[nodiscard]] Result<std::int64_t> iso_date_to_ns(std::string_view iso) {
  if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') {
    return Err(ErrorCode::ParseError, "iso_date_to_ns: bad date '" + std::string{iso} + "'");
  }
  const auto number = [](std::string_view field, int &value) {
    const auto [end, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
    return ec == std::errc{} && end == field.data() + field.size();
  };
  int year = 0;
  int month = 0;
  int day = 0;
  if (!number(iso.substr(0, 4), year) || !number(iso.substr(5, 2), month) ||
      !number(iso.substr(8, 2), day) || month < 1 || month > 12 || day < 1 || day > 31) {
    return Err(ErrorCode::ParseError, "iso_date_to_ns: bad date '" + std::string{iso} + "'");
  }
  const std::int64_t serial =
      days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  const Civil round_trip = civil_from_days(serial);
  if (round_trip.year != year || round_trip.month != static_cast<unsigned>(month) ||
      round_trip.day != static_cast<unsigned>(day)) {
    return Err(ErrorCode::ParseError, "iso_date_to_ns: bad date '" + std::string{iso} + "'");
  }
  return Ok(serial * 86400LL * 1000000000LL);
}

// Empty `path` => Ok(nullopt): no calendar, feature-column NaN downstream
// (this task only stamps the count onto PendingJob -- see the file banner).
// A zero-date file (all comments/blank) is a VALID, empty calendar for an
// underlier with no scheduled events, not an error.
[[nodiscard]] Result<std::optional<EventSchedule>> load_events_tsv(std::string_view path) {
  if (path.empty()) {
    return Ok(std::optional<EventSchedule>{std::nullopt});
  }
  std::ifstream in{std::string{path}, std::ios::binary};
  if (!in) {
    return Err(ErrorCode::IoError, "load_events_tsv: cannot open '" + std::string{path} + "'");
  }
  std::vector<std::int64_t> event_ts_ns;
  std::string line;
  // Bounded by the file's own line count -- std::getline terminates at EOF.
  while (std::getline(in, line)) {
    const std::string_view raw = trim(rstrip_cr(line));
    if (raw.empty() || raw.front() == '#') {
      continue;
    }
    const Result<std::int64_t> ns = iso_date_to_ns(raw);
    if (!ns.has_value()) {
      return Err(ErrorCode::ParseError, "load_events_tsv: bad date '" + line + "'");
    }
    event_ts_ns.push_back(*ns);
  }
  return Ok(std::optional<EventSchedule>{EventSchedule{std::move(event_ts_ns)}});
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

// Task F-2: trailing spot-history depth for the per-entry-date RV panel. The
// widest window realized_vol_panel computes is 252 sessions, which needs 253
// closes (252 log-returns); a longer available history is truncated by that
// panel's own trailing-slice logic anyway, so there is no reason to load
// more.
constexpr std::size_t kRvHistoryBars = 253;

// Index of `full_clock.refs()`'s ref whose date == `date`, or nullopt if no
// ref matches. Only called twice per run (the requested entry window's first
// and last date), so a linear scan over the corpus timeline is not worth a
// dedicated date->index map.
[[nodiscard]] std::optional<std::size_t> find_ref_index(const Clock &full_clock,
                                                        std::string_view date) noexcept {
  const std::span<const SnapshotRef> refs = full_clock.refs();
  // Bounded by refs.size() -- the whole corpus timeline.
  for (std::size_t i = 0; i < refs.size(); ++i) {
    if (refs[i].date == date) {
      return i;
    }
  }
  return std::nullopt;
}

// Task F-2: spot-history pre-pass. One MarketSnapshot::load per session in
// [first_needed_idx, last_needed_idx] of clock.refs(), emitting a
// spot-mirror OhlcBar (open == high == low == close == S) per session -- the
// in-memory array the per-entry-date realized_vol_panel calls slice, so the
// pre-pass is one corpus walk instead of one per entry date. Spot resolve
// mirrors load_bev_path's own pattern exactly (src/breakeven.cpp:213-244):
// uid_of -> find, Err(NotFound, ...) naming uid and ts on a missing surface;
// guard std::isfinite(S) && S > 0.0 with Err(InvalidArgument, ...) naming
// ts_ns.
[[nodiscard]] Result<std::vector<OhlcBar>> load_spot_history(const Clock &clock,
                                                             std::string_view uid,
                                                             std::size_t first_needed_idx,
                                                             std::size_t last_needed_idx) {
  const std::span<const SnapshotRef> refs = clock.refs();
  if (refs.empty() || first_needed_idx > last_needed_idx || last_needed_idx >= refs.size()) {
    return Err(ErrorCode::InvalidArgument, "load_spot_history: index range [" +
                                               std::to_string(first_needed_idx) + ", " +
                                               std::to_string(last_needed_idx) + "] invalid for " +
                                               std::to_string(refs.size()) + " refs");
  }
  std::vector<OhlcBar> bars;
  bars.reserve(last_needed_idx - first_needed_idx + 1);
  // JPL Rule 2: bounded by [first_needed_idx, last_needed_idx], a fixed,
  // already-validated sub-range of clock.refs().
  for (std::size_t i = first_needed_idx; i <= last_needed_idx; ++i) {
    const SnapshotRef &ref = refs[i];
    ATX_TRY(const MarketSnapshot session, MarketSnapshot::load(ref.archive_path));
    const std::int64_t ts = session.ts_ns();
    const std::optional<std::uint32_t> resolved_uid = session.uid_of(uid);
    const SurfaceRef surf = resolved_uid.has_value() ? session.find(*resolved_uid) : SurfaceRef{};
    if (surf == nullptr) {
      return Err(ErrorCode::NotFound, "load_spot_history: no surface for uid '" + std::string(uid) +
                                          "' at ts_ns=" + std::to_string(ts));
    }
    const double S = surf.pricing().S;
    if (!(std::isfinite(S) && S > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "load_spot_history: non-finite/non-positive spot (S=" + std::to_string(S) +
                     ") at ts_ns=" + std::to_string(ts));
    }
    bars.push_back(OhlcBar{.ts_ns = ts, .open = S, .high = S, .low = S, .close = S});
  }
  return Ok(std::move(bars));
}

// One surviving candidate: a strike/side/expiry job plus everything the TSV
// row needs that solve_breakeven_batch's SoA frame does not itself carry.
// `path_idx` indexes `owned_paths` (I1: every candidate for one entry date
// shares the SAME loaded path, since load_bev_path's arguments never depend
// on delta target or side -- `owned_paths` therefore has one entry per entry
// date, not one per pending job, so this is an indirection rather than `i`).
struct PendingJob {
  std::int64_t entry_ts_ns{0};
  double strike{0.0};
  std::int64_t expiry_ns{0}; // == the loaded path's settle_ts_ns (Task 5)
  Side side{Side::Call};
  double sigma_entry_iv{0.0};
  bool snapped{false};
  std::size_t path_idx{0};
  // Scheduled-events count in (entry_ts_ns, entry_ts_ns + T] (Task F-1's
  // count_events_at). NaN when no --events calendar was loaded. Same value
  // across the whole candidate lattice for one entry date -- it depends only
  // on entry_ts_ns/T, never on strike/side. File-local; a later task (F-3)
  // consumes it as a feature column.
  double n_events{0.0};
  // Task F-2: trailing realized-vol panel (RvEstimator::CloseToClose,
  // 252.0 annualization) over up to kRvHistoryBars spot-mirror bars ending
  // at and including entry_ts_ns's own session close -- the information set
  // available at entry. NaN when the trailing history is too short
  // (realized_vol_panel's own per-slot NaN contract). Same value across the
  // whole candidate lattice for one entry date, like n_events above.
  // File-local; a later task (F-3) consumes it as a feature column.
  double rv_21d{std::numeric_limits<double>::quiet_NaN()};
  double rv_63d{std::numeric_limits<double>::quiet_NaN()};
};

struct RunCounters {
  std::size_t n_entry_dates{0};
  std::size_t n_entry_dates_skipped{0};
  std::size_t n_candidates_considered{0};
  std::size_t n_candidates_prebuild_skipped{0};
  std::size_t n_jobs_solved_ok{0};
  std::size_t n_rows_written{0};
  // Task F-2: entry dates (out of n_entry_dates - n_entry_dates_skipped)
  // whose 21d RV slot came out NaN -- trailing history shorter than
  // realized_vol_panel needs for that window. Not itself an error; NaN rides
  // through to every PendingJob for that date.
  std::size_t n_entry_dates_rv_short{0};
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
// its replay path (Task 5) and appending to `pending` (each entry's
// `path_idx` points into `owned_paths`, NOT index-aligned with `pending` --
// see PendingJob's comment). `rv_21d`/`rv_63d` (Task F-2) are precomputed by
// the caller from the spot-history pre-pass and stamped onto every surviving
// candidate unchanged, exactly like `n_events`. Soft failures (unreachable
// delta, invalid iv, path load failure) skip just that candidate; nothing
// here is fatal to the whole run.
void collect_entry_date_jobs(const Clock &full_clock, const BevFactoryArgs &args,
                             const PricedSurface &surf, std::int64_t entry_ts_ns, double T,
                             const std::optional<EventSchedule> &events, double rv_21d,
                             double rv_63d, std::string_view entry_date, RunCounters &counters,
                             std::vector<PendingJob> &pending, std::vector<BevPath> &owned_paths) {
  const auto nominal_expiry_ns =
      entry_ts_ns + static_cast<std::int64_t>(std::llround(T * kNsPerYear));

  // Task F-1: same value for every candidate below (depends only on
  // entry_ts_ns/T, never strike/side) -- computed once per entry date rather
  // than once per surviving candidate. NaN (not 0) when no calendar was
  // loaded, so a downstream consumer can distinguish "zero events" from "no
  // calendar" rather than silently treating them the same.
  const double n_events = events.has_value()
                              ? static_cast<double>(count_events_at(*events, entry_ts_ns, T))
                              : std::numeric_limits<double>::quiet_NaN();

  // I1: every argument to load_bev_path below (full_clock aside) is invariant
  // across the whole delta*side candidate lattice below -- target/side never
  // influence entry_ts_ns/nominal_expiry_ns. Call it ONCE per entry date and
  // share the resulting BevPath across every surviving candidate's BevJob,
  // rather than up to 2*(1/kDeltaGridStep + 1) times with byte-identical
  // arguments (38 at the documented default --delta-lo 0.05 --delta-hi 0.95).
  // `full_clock.between(entry_date, ...)` date-bounds the walk to [this entry
  // date, corpus end] so load_bev_path does not re-open (MarketSnapshot::load)
  // every archive from the start of the WHOLE corpus on every entry date just
  // to skip past ts < entry_ts_ns -- the upper bound is left at the corpus's
  // own last date (rather than an exact expiry-date bound, which would need a
  // ts_ns->date conversion this driver has no utility for) because
  // load_bev_path's own walk already breaks as soon as it sees a session past
  // expiry_ns, so bounding the top further would save at most one archive
  // open. `entry_date` is already the exact partition key `entry_ts_ns` was
  // read from (the caller's own `SnapshotRef::date`), so this cannot exclude
  // the entry session itself.
  std::optional<std::size_t> path_idx;
  {
    const Result<Clock> bounded_clock =
        full_clock.between(entry_date, full_clock.refs().back().date);
    if (bounded_clock.has_value()) {
      Result<BevPath> path = load_bev_path(*bounded_clock, args.uid, entry_ts_ns, nominal_expiry_ns,
                                           kTenorProbeYears, BevExpirySnap::LastSessionAtOrBefore);
      if (path.has_value()) {
        path_idx = owned_paths.size();
        owned_paths.push_back(std::move(*path));
      }
    }
  }

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
      if (!path_idx.has_value()) {
        ++counters.n_candidates_prebuild_skipped; // shared path (I1) failed to load
        continue;
      }
      const BevPath &loaded = owned_paths[*path_idx];
      pending.push_back(PendingJob{.entry_ts_ns = entry_ts_ns,
                                   .strike = *k,
                                   .expiry_ns = loaded.settle_ts_ns,
                                   .side = side,
                                   .sigma_entry_iv = sigma_entry_iv,
                                   .snapped = loaded.snapped,
                                   .path_idx = *path_idx,
                                   .n_events = n_events,
                                   .rv_21d = rv_21d,
                                   .rv_63d = rv_63d});
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
  ATX_TRY(const std::optional<EventSchedule> events, load_events_tsv(args.events));

  ATX_TRY(SurfaceDb db, SurfaceDb::open(args.db));
  ATX_TRY(const Clock full_clock, Clock::from_surface_db(db));
  ATX_TRY(const Clock entry_clock, full_clock.between(args.entry_start, args.entry_end));

  // Task F-2: spot-history pre-pass. Loaded ONCE, ahead of the entry loop
  // below, over [first entry date - kRvHistoryBars, last entry date] of
  // full_clock.refs() -- indices are into full_clock (not entry_clock's own,
  // narrower span), since the trailing RV window for the FIRST entry date
  // reaches back before entry_clock's own start. A load failure here is a
  // HARD error (ATX_TRY, not a per-date skip): a corpus that can serve
  // surfaces but not spots is broken, and silently stamping NaN onto every
  // label would be worse than failing loudly.
  const std::optional<std::size_t> first_entry_idx =
      find_ref_index(full_clock, entry_clock.refs().front().date);
  const std::optional<std::size_t> last_entry_idx =
      find_ref_index(full_clock, entry_clock.refs().back().date);
  if (!first_entry_idx.has_value() || !last_entry_idx.has_value()) {
    return Err(ErrorCode::NotFound,
               "run_bev_label_factory: entry date range not found in full clock");
  }
  const std::size_t first_needed_idx =
      *first_entry_idx > kRvHistoryBars ? *first_entry_idx - kRvHistoryBars : 0;
  ATX_TRY(const std::vector<OhlcBar> spot_history,
          load_spot_history(full_clock, args.uid, first_needed_idx, *last_entry_idx));

  RunCounters counters;
  std::vector<PendingJob> pending;
  std::vector<BevPath> owned_paths;

  // Walking pointer into `spot_history`: entry dates ascend (entry_clock.
  // refs() is ordered), so this only ever advances across the whole loop
  // below, never re-scans a bar it has already passed.
  std::size_t bar_ptr = 0;

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

    // Task F-2: per-entry-date RV panel over the trailing spot-mirror bars
    // ENDING AT AND INCLUDING this entry date's own session close (the
    // information set available at entry). NaN when the panel's 21d slot
    // comes out short -- not an error, just a feature-quality flag (counted
    // below).
    double rv_21d = std::numeric_limits<double>::quiet_NaN();
    double rv_63d = std::numeric_limits<double>::quiet_NaN();
    // Bounded by spot_history.size().
    while (bar_ptr < spot_history.size() && spot_history[bar_ptr].ts_ns < *entry_ts_ns) {
      ++bar_ptr;
    }
    if (bar_ptr < spot_history.size() && spot_history[bar_ptr].ts_ns == *entry_ts_ns) {
      const std::size_t window_begin =
          (bar_ptr + 1 > kRvHistoryBars) ? bar_ptr + 1 - kRvHistoryBars : 0;
      const std::span<const OhlcBar> window{spot_history.data() + window_begin,
                                            bar_ptr + 1 - window_begin};
      // Err is unreachable in practice (spot-mirror bars always pass OHLC
      // validation, per this task's brief); if it ever fires, rv_21d/rv_63d
      // simply stay at NaN -- the n_entry_dates_rv_short counter below still
      // catches it.
      const Result<RvPanel> panel = realized_vol_panel(window, RvEstimator::CloseToClose, 252.0);
      if (panel.has_value()) {
        rv_21d = panel->vol[1];
        rv_63d = panel->vol[2];
      }
    }
    if (!std::isfinite(rv_21d)) {
      ++counters.n_entry_dates_rv_short;
    }

    collect_entry_date_jobs(full_clock, args, *surf, *entry_ts_ns, *T, events, rv_21d, rv_63d,
                            ref.date, counters, pending, owned_paths);
  }

  // Build the batch's job spans only once every path is stable in
  // `owned_paths` -- BevJob::path/dividends are non-owning, so nothing may
  // reference an `owned_paths[...].days` buffer before that vector's final
  // contents (and therefore each element's OWN heap-backed `days` buffer) are
  // settled. `pending[i].path_idx` indexes `owned_paths` (I1: every candidate
  // for one entry date shares the SAME loaded path, so `owned_paths` has one
  // entry per entry date, not one per pending job -- `i` would be wrong).
  std::vector<BevJob> jobs;
  jobs.reserve(pending.size());
  for (std::size_t i = 0; i < pending.size(); ++i) {
    jobs.push_back(BevJob{.path = owned_paths[pending[i].path_idx].days,
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
                   " solved_ok=" + std::to_string(counters.n_jobs_solved_ok) +
                   " rv_short=" + std::to_string(counters.n_entry_dates_rv_short) + ")");
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
      {"events", args.events},
      {"threads", std::to_string(args.n_threads)},
      {"n_entry_dates", std::to_string(counters.n_entry_dates)},
      {"n_entry_dates_skipped", std::to_string(counters.n_entry_dates_skipped)},
      {"n_candidates_considered", std::to_string(counters.n_candidates_considered)},
      {"n_candidates_prebuild_skipped", std::to_string(counters.n_candidates_prebuild_skipped)},
      {"n_jobs_solved_ok", std::to_string(counters.n_jobs_solved_ok)},
      {"n_entry_dates_rv_short", std::to_string(counters.n_entry_dates_rv_short)},
      {"n_rows_written", std::to_string(counters.n_rows_written)},
  };

  ATX_TRY_VOID(write_labels_tsv(args.out, args.uid, meta, rows));

  std::printf("[bev_label_factory] entry_dates=%zu (skipped %zu) candidates=%zu "
              "(prebuild_skipped %zu) solved_ok=%zu rv_short=%zu rows=%zu -> %s\n",
              counters.n_entry_dates, counters.n_entry_dates_skipped,
              counters.n_candidates_considered, counters.n_candidates_prebuild_skipped,
              counters.n_jobs_solved_ok, counters.n_entry_dates_rv_short, counters.n_rows_written,
              args.out.c_str());
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
