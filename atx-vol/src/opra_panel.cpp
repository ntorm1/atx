#include "atx/vol/opra_panel.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"   // time::Timestamp
#include "atx/core/error.hpp"      // Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/hash.hpp"       // hash_bytes
#include "atx/core/io/parquet.hpp" // read_parquet, ParquetTable, DType
#include "atx/vol/data.hpp"        // QuoteFrame/Row, build_uid_list, iso_to_ns
#include "atx/vol/dividend.hpp"    // imply_forward_atm_pcp, CoTermQuote
#include "atx/vol/vol_time.hpp"    // TimeSpec, time_to_expiry_years

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

namespace io = atx::core::io;

// Unset-price sentinel written by the OPRA pull (bid_px/ask_px).
constexpr std::int64_t kUnsetPx = std::numeric_limits<std::int64_t>::min();

// Fixed-point price scale: raw fields are 1e-9 dollars.
constexpr double kPxScale = 1e-9;

// Days since the Unix epoch for a civil date (Howard Hinnant; matches
// data.cpp's `days_since_epoch`). Used only for the U.S.-Eastern DST rule below.
[[nodiscard]] std::int64_t days_since_epoch(int yy, int mm, int dd) noexcept {
  yy -= (mm <= 2) ? 1 : 0;
  const int era = (yy >= 0 ? yy : yy - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(yy - era * 400);
  const int mp_int = ((mm > 2 ? mm - 3 : mm + 9) * 153 + 2) / 5;
  const unsigned doy = static_cast<unsigned>(mp_int) + static_cast<unsigned>(dd) - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Civil weekday, 0 = Sunday .. 6 = Saturday (1970-01-01 was a Thursday = 4).
[[nodiscard]] int civil_weekday(int yy, int mm, int dd) noexcept {
  const std::int64_t days = days_since_epoch(yy, mm, dd);
  int wd = static_cast<int>((days + 4) % 7);
  if (wd < 0) {
    wd += 7;
  }
  return wd;
}

// Day-of-month of the `nth` (1-based) Sunday of month `mm` in year `yy`.
[[nodiscard]] int nth_sunday(int yy, int mm, int nth) noexcept {
  const int first_wd = civil_weekday(yy, mm, 1);     // 0 = Sunday
  const int first_sunday = 1 + ((7 - first_wd) % 7); // first Sunday's day-of-month
  return first_sunday + 7 * (nth - 1);
}

// Is a U.S.-Eastern civil DATE in daylight saving time (EDT)? EDT runs from the
// 2nd Sunday of March to the 1st Sunday of November; the 16:00-ET option close is
// always after the 02:00 transition, so the boundary Sundays resolve by date
// alone (2nd-Sun-Mar = EDT, 1st-Sun-Nov = EST). Pure integer arithmetic.
[[nodiscard]] bool us_eastern_is_dst(int yy, int mm, int dd) noexcept {
  if (mm < 3 || mm > 11) {
    return false; // Dec, Jan, Feb -> EST
  }
  if (mm > 3 && mm < 11) {
    return true; // Apr .. Oct -> EDT
  }
  if (mm == 3) {
    return dd >= nth_sunday(yy, 3, 2); // on/after the 2nd Sunday
  }
  return dd < nth_sunday(yy, 11, 1); // mm == 11: before the 1st Sunday
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && s[b] == ' ') {
    ++b;
  }
  while (e > b && s[e - 1] == ' ') {
    --e;
  }
  return s.substr(b, e - b);
}

// Parse a full-width unsigned decimal field into `out`. Returns false unless the
// entire span was consumed as digits.
[[nodiscard]] bool parse_field(std::string_view s, int &out) noexcept {
  const char *first = s.data();
  const char *last = s.data() + s.size();
  const std::from_chars_result res = std::from_chars(first, last, out);
  return res.ec == std::errc{} && res.ptr == last;
}

// Rewrite a bare "YYYY-MM-DD" expiry into the U.S.-equity PM close instant
// "YYYY-MM-DDT16:00:00-04:00" (EDT) or "-05:00" (EST). No-op for any string that
// is not exactly a 10-char bare date (already carries a time, or malformed).
// [[maybe_unused]]: the OPRA loader was unified onto G1's always-on
// expiry_instant_ns() path (merge 2026-07-20), so main's opt-in ExpiryCloseConvention
// no longer calls this. Retained (not deleted) so ExpiryCloseConvention can be
// re-wired later without reconstructing the DST-aware conversion.
[[maybe_unused, nodiscard]] std::string to_us_equity_pm_close(std::string_view date_iso) {
  if (date_iso.size() != 10) {
    return std::string(date_iso);
  }
  int yy = 0;
  int mm = 0;
  int dd = 0;
  if (!parse_field(date_iso.substr(0, 4), yy) || !parse_field(date_iso.substr(5, 2), mm) ||
      !parse_field(date_iso.substr(8, 2), dd)) {
    return std::string(date_iso);
  }
  std::string out(date_iso);
  out.append("T16:00:00");
  out.append(us_eastern_is_dst(yy, mm, dd) ? "-04:00" : "-05:00");
  return out;
}

[[nodiscard]] bool numeric_symbol_fallback(std::string_view symbol,
                                           std::int64_t instrument_id) noexcept {
  std::int64_t parsed = 0;
  const std::from_chars_result result =
      std::from_chars(symbol.data(), symbol.data() + symbol.size(), parsed);
  return result.ec == std::errc{} && result.ptr == symbol.data() + symbol.size() &&
         parsed == instrument_id;
}

void append_source_u64(std::string &out, std::uint64_t value) {
  char text[32];
  const auto [ptr, ec] = std::to_chars(text, text + sizeof text, value);
  (void)ec;
  out.append(text, static_cast<std::size_t>(ptr - text));
  out.push_back('|');
}

void append_source_text(std::string &out, std::string_view value) {
  append_source_u64(out, value.size());
  out.append(value);
  out.push_back('|');
}

// Content hash over the frame's SOURCE rows/identities only (schema version,
// snapshot stamp, uid, every row's quote fields, and the instrument-identity
// map) -- it intentionally does NOT fold in `frame.time`/`spec.time`
// (TimeSpec), so two panels loaded from byte-identical source rows under
// DIFFERENT T conventions (e.g. Calendar365 vs VolTime) share the same
// fingerprint; it identifies the market-data rows, not the fitting
// convention applied to them.
[[nodiscard]] std::uint64_t
source_fingerprint(const QuoteFrame &frame, std::span<const std::uint32_t> instrument_ids,
                   const std::map<std::uint32_t, std::string> &identities,
                   std::uint32_t schema_version) {
  std::string bytes;
  append_source_u64(bytes, schema_version);
  append_source_text(bytes, frame.snapshot_iso);
  append_source_u64(bytes, static_cast<std::uint64_t>(frame.snapshot_ts_ns));
  append_source_text(bytes, frame.uid);
  append_source_u64(bytes, frame.rows.size());
  for (std::size_t i = 0; i < frame.rows.size(); ++i) {
    const QuoteRow &row = frame.rows[i];
    append_source_text(bytes, row.uid);
    append_source_text(bytes, row.expiry_iso);
    append_source_u64(bytes, std::bit_cast<std::uint64_t>(row.strike));
    append_source_u64(bytes, static_cast<std::uint64_t>(row.side));
    append_source_u64(bytes, std::bit_cast<std::uint64_t>(row.bid));
    append_source_u64(bytes, std::bit_cast<std::uint64_t>(row.ask));
    append_source_u64(bytes, static_cast<std::uint64_t>(row.bid_size));
    append_source_u64(bytes, static_cast<std::uint64_t>(row.ask_size));
    append_source_u64(bytes, i < instrument_ids.size() ? instrument_ids[i] : 0u);
  }
  append_source_u64(bytes, identities.size());
  for (const auto &[instrument_id, raw_symbol] : identities) {
    append_source_u64(bytes, instrument_id);
    append_source_text(bytes, raw_symbol);
  }
  const std::uint64_t hash = atx::core::hash_bytes(bytes.data(), bytes.size());
  return hash == 0u ? 1u : hash;
}

// Record the first row's `ts` as epoch nanoseconds, tolerating either the real
// Timestamp column or an Int64(ns) column. 0 when absent/empty.
[[nodiscard]] std::int64_t first_ts_ns(const io::ParquetTable &table) {
  const io::ColumnInfo *col = table.schema().find("ts");
  if (col == nullptr || table.num_rows() <= 0) {
    return 0;
  }
  if (col->dtype == io::DType::Timestamp) {
    // column_view<Timestamp> is a rejected specialization (Arrow stores raw i64 in
    // the array's own unit, so it cannot alias) — it ALWAYS errors. Must go through
    // to_column<Timestamp>, which copies + normalises each cell to i64 nanoseconds.
    const auto v = table.to_column<atx::core::time::Timestamp>("ts");
    if (v.has_value() && v->size() > 0) {
      return (*v)[0].unix_nanos();
    }
  } else if (col->dtype == io::DType::Int64) {
    const auto v = table.column_view<std::int64_t>("ts");
    if (v.has_value() && !v->empty()) {
      return (*v)[0];
    }
  }
  return 0;
}

// Format an epoch-ns instant as `YYYY-MM-DDTHH:MM:SSZ`. Used only to spell BOTH
// sides of a snapshot-stamp mismatch in the same comparable form (FIX-C-1's
// error text) -- `ns_to_iso_date` alone drops the time-of-day, which is exactly
// the field a wrong `--snapshot-suffix` gets wrong.
[[nodiscard]] std::string ns_to_iso_instant(std::int64_t ns) {
  constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
  std::int64_t rem = ns % kDayNs;
  if (rem < 0) {
    rem += kDayNs;
  }
  const std::int64_t secs = rem / 1000000000LL;
  const auto pad2 = [](std::int64_t v) {
    std::string s = std::to_string(v);
    return s.size() < 2 ? "0" + s : s;
  };
  return ns_to_iso_date(ns) + "T" + pad2(secs / 3600) + ":" + pad2((secs / 60) % 60) + ":" +
         pad2(secs % 60) + "Z";
}

// Imply the underlying spot from put-call parity on the earliest expiry that
// carries at least one co-terminal call/put pair with both mids > 0. `rate_at`
// yields the continuously-compounded rate at a maturity T (a flat scalar, or a
// term YieldCurve query); the front expiry is discounted at its OWN r(T_front).
// `time` governs every year-fraction below (default Calendar365 is
// BIT-IDENTICAL to the historical `year_fraction`-derived T).
template <typename RateFn>
[[nodiscard]] Result<double> imply_spot_from_pcp(const QuoteFrame &frame, RateFn rate_at,
                                                 const TimeSpec &time,
                                                 SettlementSession settlement) {
  const std::int64_t snapshot_ns = iso_to_ns(frame.snapshot_iso);
  struct MidPair {
    double call_mid = -1.0;
    double put_mid = -1.0;
  };
  // W1-B (F35). The anchor below is `argmin |call_mid - put_mid|`, so a leg
  // with no bid is not merely admitted — it is PREFERRED: halving one side's
  // mid shrinks the gap, and the most defective pair on the board becomes the
  // anchor for the spot the entire pipeline is then built on. Require both legs
  // strictly two-sided (`bid > 0 && ask > bid`) to enter the pair table at all.
  // This is the same rule board classification, the Configured cascade and the
  // selector holdout already use, so the loader no longer trusts a quote the
  // fitter would refuse. Measured no-op on both OPRA corpora (1.22M rows): no
  // surviving row has a zero bid or a locked market, so no board's implied spot
  // moves — it closes the hole rather than papering over one.
  std::map<std::string, std::map<double, MidPair>> by_expiry;
  for (const QuoteRow &row : frame.rows) {
    if (!(row.bid > 0.0) || !(row.ask > row.bid)) {
      continue;
    }
    const double mid = 0.5 * (row.bid + row.ask);
    if (!(mid > 0.0)) {
      continue;
    }
    MidPair &pair = by_expiry[row.expiry_iso][row.strike];
    if (row.side == Side::Call) {
      pair.call_mid = mid;
    } else {
      pair.put_mid = mid;
    }
  }

  // Real OPRA chains include 0DTE / same-day expiries whose year-fraction is
  // ~0 (or negative against an intraday snapshot) — a PCP forward back-out there
  // is wildly ill-conditioned. Require a few days to expiry so the earliest
  // WELL-CONDITIONED expiry sets the spot; a per-expiry PCP failure is non-fatal
  // (we try the next), and only a fully empty search errors.
  constexpr double kMinSpotT = 3.0 / 365.0;
  // Small local: back out one expiry's PCP forward from its co-terminal pairs.
  // `out_gap` receives the nearest-ATM |call_mid - put_mid| of the chosen ref
  // strike — the conditioning proxy the fallback below minimizes across expiries.
  const auto forward_for_expiry = [&](const std::string &expiry, double t_front,
                                      double &out_gap) -> Result<double> {
    std::vector<CoTermQuote> quotes;
    for (const auto &[strike, pair] : by_expiry.at(expiry)) {
      if (pair.call_mid > 0.0 && pair.put_mid > 0.0) {
        quotes.push_back(CoTermQuote{strike, pair.call_mid, pair.put_mid});
      }
    }
    if (quotes.empty()) {
      return Err(ErrorCode::Unavailable, "no co-terminal pair");
    }
    // This expiry's own rate (flat scalar, or the term curve at T_front). On the
    // flat path `rate_at` returns the scalar verbatim, so every float op below is
    // bit-identical to the historical single-`r` code.
    const double r = rate_at(t_front);
    // ATM reference: the strike whose call/put mids are closest (C == P at F).
    double s_ref = quotes.front().strike;
    double best = std::numeric_limits<double>::infinity();
    for (const CoTermQuote &q : quotes) {
      const double gap = std::fabs(q.call_mid - q.put_mid);
      if (gap < best) {
        best = gap;
        s_ref = q.strike;
      }
    }
    out_gap = best;
    const Result<double> forward =
        imply_forward_atm_pcp(std::span<const CoTermQuote>(quotes), s_ref, t_front, r);
    if (!forward.has_value() || !(*forward > 0.0) || !std::isfinite(*forward)) {
      return Err(ErrorCode::Unavailable, "no usable forward");
    }
    const double spot = *forward * std::exp(-r * t_front);
    if (!(spot > 0.0) || !std::isfinite(spot)) {
      return Err(ErrorCode::Unavailable, "degenerate implied spot");
    }
    return Ok(spot);
  };

  // First pass — historical behavior, BIT-IDENTICAL for any name that carries at
  // least one well-conditioned (T > 3/365) co-terminal pair: the earliest such
  // expiry sets the spot and we return immediately. Healthy names (AAPL/SPY) all
  // qualify here, so their implied spot is unchanged.
  for (const auto &[expiry, strikes] : by_expiry) {
    (void)strikes;
    // Use the explicitly selected product settlement instant so the
    // front-expiry PCP back-out discounts at the same intraday T data_install uses.
    // A VolTime coverage failure (vol_time.hpp) aborts the whole spot
    // implication -- skipping the expiry instead would quietly pick a different,
    // farther-dated reference and report a spot the caller never asked for.
    ATX_TRY(const double t_front,
            time_to_expiry_years(snapshot_ns, expiry_instant_ns(expiry, settlement), time));
    if (!(t_front > kMinSpotT)) {
      continue; // 0DTE / same-week: too ill-conditioned for a PCP spot back-out
    }
    double gap = 0.0;
    const Result<double> spot = forward_for_expiry(expiry, t_front, gap);
    if (spot.has_value()) {
      return spot; // this expiry yielded a usable forward; done
    }
  }

  // FIX D fallback — only reached when NO T > 3/365 expiry produced a usable
  // spot (the ~thinnest names carry only very-short-dated two-sided pairs). Widen
  // the horizon toward ~1 day and pick the BEST-CONDITIONED co-terminal pair
  // across ALL qualifying expiries (nearest-ATM: smallest |call_mid - put_mid|),
  // rather than the first. Still rejects any non-positive / non-finite forward,
  // so a degenerate pair never yields a garbage spot. Because this runs only
  // after the historical pass fully failed, no healthy-name spot is affected.
  constexpr double kMinSpotTFallback = 1.0 / 365.0;
  double best_spot = std::numeric_limits<double>::quiet_NaN();
  double best_gap = std::numeric_limits<double>::infinity();
  for (const auto &[expiry, strikes] : by_expiry) {
    (void)strikes;
    // Use the explicitly selected product settlement instant so the
    // front-expiry PCP back-out discounts at the same intraday T data_install uses.
    ATX_TRY(const double t_front,
            time_to_expiry_years(snapshot_ns, expiry_instant_ns(expiry, settlement), time));
    if (!(t_front > kMinSpotTFallback)) {
      continue; // sub-1-day: PCP forward back-out too ill-conditioned even here
    }
    double gap = 0.0;
    const Result<double> spot = forward_for_expiry(expiry, t_front, gap);
    if (spot.has_value() && gap < best_gap) {
      best_gap = gap;
      best_spot = *spot;
    }
  }
  if (std::isfinite(best_spot) && best_spot > 0.0) {
    return Ok(best_spot);
  }
  // W1-B: separate the two refusals an operator has to act on differently.
  // "No pair at all" is a board-shape fact — the GNK / ATAI cohort, where no
  // strike carries a two-sided call AND a two-sided put — and no tolerance
  // widening will change it. "No well-conditioned expiry" means pairs exist but
  // every one of them is sub-1-day or back-solves to a degenerate forward. The
  // single old message conflated the two and read as the second.
  std::size_t n_pairs = 0;
  for (const auto &[expiry, strikes] : by_expiry) {
    (void)expiry;
    for (const auto &[strike, pair] : strikes) {
      (void)strike;
      if (pair.call_mid > 0.0 && pair.put_mid > 0.0) {
        ++n_pairs;
      }
    }
  }
  if (n_pairs == 0) {
    return Err(ErrorCode::Unavailable,
               "no strike carries a two-sided call and a two-sided put on any expiry, so "
               "put-call parity cannot imply a spot; pass spot_override");
  }
  return Err(ErrorCode::Unavailable,
             "no well-conditioned co-terminal expiry to imply spot; pass spot_override");
}

// Materialize every column the panel core reads, once, and index the rows by
// `underlying` (P-01). Permissive about the OPTIONAL columns in exactly the way
// `panel_from_scan` is: `symbol`, `bid_px`, `ask_px`, `bid_sz`, `ask_sz` are
// required (their absence fails at the same ATX_TRY, with the same code and
// text, that the per-row path used to), while `underlying` and `instrument_id`
// are optional. The public `scan_opra_cbbo_table` adds the hive-v2 8-column
// pre-check on top.
[[nodiscard]] Result<OpraTableScan> scan_table(const io::ParquetTable &table);

// The scan-driven core shared by the file loader and both in-memory-table seams:
// everything AFTER the OPRA cbbo-1m table is materialized — row filtering, OSI
// parse, PCP spot implication, frame assembly. Every public entry point delegates
// here, so they are byte-identical on the same rows. `spec.path` is NOT read here
// (the table is already decoded); it is unused by this core.
[[nodiscard]] Result<OpraPanel> panel_from_scan(const OpraTableScan &scan,
                                                const OpraLoadSpec &spec);

// `scan_table` + `panel_from_scan`, for the entry points that hold a table
// rather than a scan.
[[nodiscard]] Result<OpraPanel> panel_from_table(const io::ParquetTable &table,
                                                 const OpraLoadSpec &spec);

} // namespace

Result<OsiSymbol> parse_osi_symbol(std::string_view sym) {
  constexpr std::size_t kFixedLen = 15;
  if (sym.size() < kFixedLen) {
    return Err(ErrorCode::InvalidArgument, "OSI symbol shorter than 15 chars");
  }
  const std::size_t n = sym.size();
  const std::string_view fixed = sym.substr(n - kFixedLen);

  int yy = 0;
  int mm = 0;
  int dd = 0;
  if (!parse_field(fixed.substr(0, 2), yy) || !parse_field(fixed.substr(2, 2), mm) ||
      !parse_field(fixed.substr(4, 2), dd)) {
    return Err(ErrorCode::ParseError, "OSI date field not numeric");
  }
  if (mm < 1 || mm > 12 || dd < 1 || dd > 31) {
    return Err(ErrorCode::ParseError, "OSI date field out of range");
  }

  const char cp = fixed[6];
  const Side side = (cp == 'P' || cp == 'p') ? Side::Put : Side::Call;

  std::int64_t strike_milli = 0;
  const char *sfirst = fixed.data() + 7;
  const char *slast = fixed.data() + kFixedLen;
  const std::from_chars_result sres = std::from_chars(sfirst, slast, strike_milli);
  if (sres.ec != std::errc{} || sres.ptr != slast) {
    return Err(ErrorCode::ParseError, "OSI strike field not numeric");
  }
  const double strike = static_cast<double>(strike_milli) / 1000.0;
  if (!(strike > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "OSI strike non-positive");
  }

  OsiSymbol out;
  out.root = std::string(trim(sym.substr(0, n - kFixedLen)));
  out.expiry_iso.reserve(10);
  out.expiry_iso.append("20")
      .append(fixed.substr(0, 2))
      .append("-")
      .append(fixed.substr(2, 2))
      .append("-")
      .append(fixed.substr(4, 2));
  out.side = side;
  out.strike = strike;
  return Ok(std::move(out));
}

// The one definition of the ticker <-> OSI-root identity rule (FIX-E, E2-b).
// Was duplicated byte-for-byte here and in `listed_opra.cpp`; the contract, the
// trailing-digit policy and the reason this is a predicate rather than a
// normaliser all live on the declaration in `opra_panel.hpp`. Walk `ticker`
// skipping '.', and require it to consume `root` exactly — so the ONLY tolerated
// difference is punctuation, and a trailing digit (an adjusted deliverable) never
// matches.
bool osi_root_matches_ticker(std::string_view root, std::string_view ticker) noexcept {
  std::size_t i = 0;
  for (const char c : ticker) {
    if (c == '.') {
      continue;
    }
    if (i >= root.size() || root[i] != c) {
      return false;
    }
    ++i;
  }
  return i == root.size();
}

InstrumentConventions us_listed_conventions(std::string_view root) noexcept {
  struct ClassEntry {
    std::string_view root;
    ExpiryCloseConvention expiry_close;
    ExerciseStyle exercise_style;
  };
  // Cash-settled U.S. index option classes, from their published contract
  // specifications. Ordering is alphabetical for review, not for lookup.
  //
  // The AM/PM split is per CLASS, not per index: the traditional third-Friday
  // contract on a broad-based index settles against the constituents' OPENING
  // prints (`UsIndexAmOpen`), while the weekly / end-of-month class listed on the
  // SAME index — the trailing `W`/`P` roots — settles PM. They are separate OSI
  // roots, which is exactly why keying on the root works and keying on the index
  // would not.
  //
  // OEX/XEO are the pair that forbids inferring style from "is it an index":
  // same underlying index, different exercise right.
  static constexpr ClassEntry kCashIndexClasses[] = {
      {"DJX", ExpiryCloseConvention::UsEquityPmClose, ExerciseStyle::European},
      {"MXEA", ExpiryCloseConvention::UsIndexAmOpen, ExerciseStyle::European},
      {"MXEF", ExpiryCloseConvention::UsIndexAmOpen, ExerciseStyle::European},
      {"NANOS", ExpiryCloseConvention::UsEquityPmClose, ExerciseStyle::European},
      {"NDX", ExpiryCloseConvention::UsIndexAmOpen, ExerciseStyle::European},
      {"NDXP", ExpiryCloseConvention::UsEquityPmClose, ExerciseStyle::European},
      {"OEX", ExpiryCloseConvention::UsIndexAmOpen, ExerciseStyle::American},
      {"RUT", ExpiryCloseConvention::UsIndexAmOpen, ExerciseStyle::European},
      {"RUTW", ExpiryCloseConvention::UsEquityPmClose, ExerciseStyle::European},
      {"SPX", ExpiryCloseConvention::UsIndexAmOpen, ExerciseStyle::European},
      {"SPXW", ExpiryCloseConvention::UsEquityPmClose, ExerciseStyle::European},
      {"VIX", ExpiryCloseConvention::UsIndexAmOpen, ExerciseStyle::European},
      {"VIXW", ExpiryCloseConvention::UsIndexAmOpen, ExerciseStyle::European},
      {"XEO", ExpiryCloseConvention::UsIndexAmOpen, ExerciseStyle::European},
      {"XSP", ExpiryCloseConvention::UsEquityPmClose, ExerciseStyle::European},
  };
  for (const ClassEntry &entry : kCashIndexClasses) {
    if (entry.root == root) {
      return InstrumentConventions{entry.expiry_close, entry.exercise_style};
    }
  }
  // Every equity, ETF and ETN: PM-settled, American-exercise. This is the value
  // the loader used unconditionally before the registry existed, so an unlisted
  // board loads bit-identically.
  return InstrumentConventions{};
}

Result<OpraPanel> load_opra_cbbo_parquet(const OpraLoadSpec &spec) {
  // ── Projected read (W4.3) ─────────────────────────────────────────────────
  // Decode ONLY the columns panel construction consumes (verified against every
  // table access in this file): ts, symbol, bid_px/ask_px/bid_sz/ask_sz, plus
  // the optional instrument_id / underlying. We project the intersection of that
  // set with the file schema, so a file MISSING an optional column still loads,
  // and a missing REQUIRED column still fails at the identical downstream
  // column_view/strings call (same code + text) the read-all path hit. Kept
  // values and row count are byte-identical to a full read, so fitted surfaces
  // are unchanged — only the decoded-byte count drops.
  static constexpr std::array<std::string_view, 8> kOpraWanted = {
      "ts", "symbol", "bid_px", "ask_px", "bid_sz", "ask_sz", "instrument_id", "underlying"};
  auto scan_res = io::LazyParquet::scan(spec.path);
  if (!scan_res.has_value()) {
    return Err(ErrorCode::InvalidArgument, scan_res.error().to_string());
  }
  const io::Schema &file_schema = scan_res.value().schema();
  std::vector<std::string_view> projection;
  projection.reserve(kOpraWanted.size());
  for (const std::string_view name : kOpraWanted) {
    if (file_schema.find(name) != nullptr) {
      projection.push_back(name);
    }
  }
  auto table_res = io::read_parquet(spec.path, std::span<const std::string_view>{projection});
  if (!table_res.has_value()) {
    return Err(ErrorCode::InvalidArgument, table_res.error().to_string());
  }
  const io::ParquetTable table = std::move(table_res.value());
  return panel_from_table(table, spec);
}

namespace {

Result<OpraTableScan> scan_table(const io::ParquetTable &table) {
  const io::Schema &schema = table.schema();
  OpraTableScan scan;
  scan.n_rows = static_cast<std::size_t>(std::max<std::int64_t>(0, table.num_rows()));

  ATX_TRY(auto symbols, table.strings("symbol"));
  scan.symbols = std::move(symbols);
  ATX_TRY(const auto bid_px, table.column_view<std::int64_t>("bid_px"));
  scan.bid_px = bid_px;
  ATX_TRY(const auto ask_px, table.column_view<std::int64_t>("ask_px"));
  scan.ask_px = ask_px;
  ATX_TRY(const auto bid_sz, table.column_view<std::int64_t>("bid_sz"));
  scan.bid_sz = bid_sz;
  ATX_TRY(const auto ask_sz, table.column_view<std::int64_t>("ask_sz"));
  scan.ask_sz = ask_sz;
  ATX_TRY(auto bid_null, table.null_mask("bid_px"));
  scan.bid_null = std::move(bid_null);
  ATX_TRY(auto ask_null, table.null_mask("ask_px"));
  scan.ask_null = std::move(ask_null);

  scan.has_instrument_id = schema.find("instrument_id") != nullptr;
  if (scan.has_instrument_id) {
    ATX_TRY(const auto ids, table.column_view<std::int64_t>("instrument_id"));
    scan.instrument_ids = ids;
  }

  scan.has_underlying = schema.find("underlying") != nullptr;
  if (scan.has_underlying) {
    ATX_TRY(auto u, table.strings("underlying"));
    scan.underlyings = std::move(u);
  }
  scan.first_ts_ns = first_ts_ns(table);

  // ── The per-underlying row index ─────────────────────────────────────────
  // One hashed pass assigns each row a group, a prefix sum turns the group
  // counts into offsets, and a second ASCENDING pass fills the row ids. The
  // ascending fill is load-bearing: it is what makes a split visit its rows in
  // the same order a full-table scan would, which is what keeps an indexed panel
  // byte-identical (row order feeds the frame, and the frame feeds the source
  // fingerprint). Guarded on a 32-bit row count so `rows` stays 4 bytes/row; a
  // table past that simply gets no index and takes the full-scan path.
  if (scan.has_underlying &&
      scan.n_rows <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    std::vector<std::uint32_t> slot(scan.n_rows);
    std::vector<std::uint32_t> counts;
    scan.slot_of.reserve(128u);
    for (std::size_t i = 0; i < scan.n_rows; ++i) {
      const auto [it, inserted] =
          scan.slot_of.try_emplace(scan.underlyings[i], static_cast<std::uint32_t>(counts.size()));
      if (inserted) {
        counts.push_back(0u);
      }
      slot[i] = it->second;
      ++counts[it->second];
    }
    scan.offsets.assign(counts.size() + 1u, 0u);
    for (std::size_t g = 0; g < counts.size(); ++g) {
      scan.offsets[g + 1u] = scan.offsets[g] + counts[g];
    }
    std::vector<std::uint32_t> cursor(scan.offsets.begin(), scan.offsets.end() - 1);
    scan.rows.resize(scan.n_rows);
    for (std::size_t i = 0; i < scan.n_rows; ++i) {
      scan.rows[cursor[slot[i]]++] = static_cast<std::uint32_t>(i);
    }
    scan.indexed = true;
  }
  return Ok(std::move(scan));
}

Result<OpraPanel> panel_from_scan(const OpraTableScan &scan, const OpraLoadSpec &spec) {
  const std::size_t n_rows = scan.n_rows;
  const std::span<const std::string_view> symbols{scan.symbols};
  const std::span<const std::int64_t> bid_px = scan.bid_px;
  const std::span<const std::int64_t> ask_px = scan.ask_px;
  const std::span<const std::int64_t> bid_sz = scan.bid_sz;
  const std::span<const std::int64_t> ask_sz = scan.ask_sz;
  const std::span<const std::uint8_t> bid_null{scan.bid_null};
  const std::span<const std::uint8_t> ask_null{scan.ask_null};
  const std::span<const std::int64_t> instrument_ids = scan.instrument_ids;
  const std::span<const std::string_view> underlyings{scan.underlyings};
  const bool has_instrument_id = scan.has_instrument_id;
  const bool has_underlying = scan.has_underlying;

  if (!has_instrument_id && spec.provenance_mode == OpraProvenanceMode::Strict) {
    return Err(ErrorCode::InvalidArgument, "strict OPRA provenance requires 'instrument_id'");
  }

  const std::string_view filter = spec.underlying;

  // ── P2-2 multi-symbol validation ────────────────────────────────────────
  // This loader implies a SINGLE underlier's spot, so ambiguous multi-symbol
  // input must be rejected rather than silently mixing books.
  if (!filter.empty() && !has_underlying) {
    return Err(ErrorCode::InvalidArgument,
               "underlying filter '" + std::string(filter) +
                   "' requested but parquet has no 'underlying' column");
  }

  // P-01: the rows THIS call visits. With a filter and an index that is the
  // filter's own group; otherwise it is every row and the loops below are
  // exactly the pre-index full scan. `selected` is ascending, so both forms
  // visit rows in the same relative order.
  const bool indexed = !filter.empty() && scan.indexed;
  const std::span<const std::uint32_t> selected =
      indexed ? scan.rows_for(filter) : std::span<const std::uint32_t>{};
  const std::size_t n_visit = indexed ? selected.size() : n_rows;
  const auto row_at = [indexed, selected](std::size_t t) noexcept -> std::size_t {
    return indexed ? static_cast<std::size_t>(selected[t]) : t;
  };

  if (has_underlying) {
    if (indexed) {
      // The index answers "is the filter present" in O(1); an empty group is
      // the zero-match case, reported with the exact text the full scan used.
      // The mixed-symbol branch below is unreachable here because `indexed`
      // requires a non-empty filter.
      if (selected.empty()) {
        return Err(ErrorCode::InvalidArgument,
                   "underlying '" + std::string(filter) + "' not found in parquet");
      }
    } else {
      std::vector<std::string_view> distinct;
      bool filter_present = false;
      for (std::size_t i = 0; i < n_rows; ++i) {
        const std::string_view und = underlyings[i];
        if (und.empty()) {
          continue;
        }
        if (!filter.empty() && und == filter) {
          filter_present = true;
        }
        if (std::find(distinct.begin(), distinct.end(), und) == distinct.end()) {
          distinct.push_back(und);
        }
      }
      if (filter.empty() && distinct.size() > 1) {
        std::string list;
        for (std::size_t j = 0; j < distinct.size(); ++j) {
          if (j != 0) {
            list.push_back(',');
          }
          list.append(distinct[j]);
        }
        return Err(ErrorCode::InvalidArgument, "mixed-symbol parquet: found {" + list +
                                                   "}; set OpraLoadSpec.underlying to select one");
      }
      if (!filter.empty() && !filter_present) {
        return Err(ErrorCode::InvalidArgument,
                   "underlying '" + std::string(filter) + "' not found in parquet");
      }
    }
  }

  bool provenance_complete = has_instrument_id;
  std::map<std::uint32_t, std::string> source_mappings;
  if (has_instrument_id) {
    for (std::size_t t = 0; t < n_visit; ++t) {
      const std::size_t i = row_at(t);
      const std::string_view und = has_underlying ? underlyings[i] : std::string_view{};
      if (!filter.empty() && und != filter) {
        continue;
      }
      const std::int64_t raw_id = instrument_ids[i];
      const bool valid_id = raw_id > 0 && raw_id <= static_cast<std::int64_t>(
                                                        std::numeric_limits<std::uint32_t>::max());
      const bool fallback = numeric_symbol_fallback(symbols[i], raw_id);
      if (!valid_id || fallback) {
        provenance_complete = false;
        if (spec.provenance_mode == OpraProvenanceMode::Strict) {
          return Err(ErrorCode::InvalidArgument,
                     fallback ? "strict OPRA provenance rejects numeric-symbol fallback"
                              : "strict OPRA provenance rejects invalid instrument_id");
        }
        continue;
      }
      const std::uint32_t id = static_cast<std::uint32_t>(raw_id);
      const auto [it, inserted] = source_mappings.try_emplace(id, std::string(symbols[i]));
      if (!inserted && it->second != symbols[i]) {
        return Err(ErrorCode::InvalidArgument,
                   "ambiguous instrument_id maps to multiple raw symbols");
      }
    }
  }

  // Snapshot stamp is hoisted above the row loop so the loop can drop expired /
  // same-day contracts by year-fraction. (snapshot_ts_ns / snapshot_iso depend only
  // on the table + spec, never on the kept rows.)
  const std::int64_t snapshot_ts_ns = scan.first_ts_ns;
  std::string snapshot_iso = spec.snapshot_iso;
  if (snapshot_iso.empty()) {
    snapshot_iso = ns_to_iso_date(snapshot_ts_ns);
  }
  const std::int64_t snapshot_spec_ns = iso_to_ns(snapshot_iso);
  // FIX-C-1 (snapshot stamp). The hive schema stamps ONE `ts` per date file and
  // that column IS the instant the quotes were snapshotted at -- the ground
  // truth for every T-to-expiry computation below (the same-day-expiry drop, the
  // term-curve rate lookup, the PCP forward) and for the panel's source
  // fingerprint. An operator-supplied `OpraLoadSpec::snapshot_iso` (the build
  // CLI's `--snapshot-suffix`, applied uniformly across a whole `--from`/`--to`
  // range) is a CLAIM about that instant, not the instant itself, and an
  // ET-anchored hive carries two of them: 19:55Z under EDT, 20:55Z under EST.
  //
  // Before this guard a valid-but-wrong stamp was silently PREFERRED over the
  // file: exit 0, `n_load_errors 0`, every date written, ~1e-3 relative IV drift,
  // a butterfly-arbitrage slice rejection, and `frame.snapshot_ts_ns` still
  // reading correct in every diagnostic (it is stamped from the file below), so
  // nothing in the output could show it. The two values were both in scope here
  // and never compared.
  //
  // So: when a stamp NAMES AN INSTANT it must AGREE with the file, and the value
  // that drives the math is taken from the FILE. Not a behaviour change for a
  // correct build -- the two are equal by the check, which is what makes this
  // re-gateable bit-for-bit against the pilot baselines. Two cases cannot
  // arbitrate and keep their pre-existing behaviour verbatim:
  //   * a file with no readable `ts` (`first_ts_ns == 0`: no column, empty
  //     table) -- there is nothing to compare against;
  //   * a bare `YYYY-MM-DD` stamp, which is NOT a claim about the minute but the
  //     legacy midnight-UTC valuation convention (a great many synthetic panel
  //     fixtures pass one against a placeholder `ts`). Every production stamp is
  //     an instant: `opra_hive.cpp:144` and `opra_batch.cpp:308` both build
  //     `date + snapshot_suffix`, and `OpraHiveSpec::snapshot_suffix` defaults to
  //     `T19:55:00Z` -- so the review's own failure scenario (an operator repairs
  //     three EST dates and OMITS `--snapshot-suffix`, taking the 19:55Z default
  //     against 20:55Z files) is checked, and so is every wrong-but-well-formed
  //     value the format validator lets through.
  const bool stamp_names_an_instant =
      spec.snapshot_iso.size() > 10 &&
      (spec.snapshot_iso[10] == 'T' || spec.snapshot_iso[10] == ' ');
  if (stamp_names_an_instant && snapshot_ts_ns != 0 && snapshot_spec_ns != snapshot_ts_ns) {
    return Err(ErrorCode::InvalidArgument,
               "snapshot stamp '" + snapshot_iso + "' (" + std::to_string(snapshot_spec_ns) +
                   " ns) disagrees with the file's own stamped ts '" +
                   ns_to_iso_instant(snapshot_ts_ns) + "' (" + std::to_string(snapshot_ts_ns) +
                   " ns) (from '" + spec.path +
                   "'): the file's ts is the snapshot instant, so every "
                   "time-to-expiry would be wrong by their difference");
  }
  const std::int64_t snapshot_iso_ns =
      stamp_names_an_instant && snapshot_ts_ns != 0 ? snapshot_ts_ns : snapshot_spec_ns;

  // P-01: sized for the rows this call VISITS, not for the table. These two
  // vectors are moved into the returned panel and stay live for the rest of the
  // build, so an n_rows-sized reservation for a symbol owning ~1% of the rows is
  // not slack — it is ~47 MB of committed address space per symbol, retained,
  // and 102 of them is the ~4.8 GB commit spike that made 102-name builds die of
  // bad_alloc at a 123 MB working set.
  std::vector<QuoteRow> rows;
  rows.reserve(n_visit);
  std::vector<std::uint32_t> kept_instrument_ids;
  kept_instrument_ids.reserve(n_visit);
  std::map<std::uint32_t, std::string> kept_mappings;
  std::size_t n_dropped = 0;
  std::size_t n_one_sided = 0;
  std::string first_underlying;
  std::string first_root;

  // The board's convention key. A load is always ONE underlier — the multi-symbol
  // guard rejects an empty filter over a multi-underlying table — so a single
  // lookup describes every row: the caller's `underlying` filter when it named
  // one, else the first parseable row's OSI root. Unfiltered loads pay one extra
  // symbol parse; the production (filtered) path pays nothing.
  std::string board_key{filter};
  for (std::size_t t = 0; board_key.empty() && t < n_visit; ++t) {
    const Result<OsiSymbol> probe = parse_osi_symbol(symbols[row_at(t)]);
    if (probe.has_value()) {
      board_key = probe->root;
    }
  }
  // Settlement instant and exercise right are properties of the option CLASS, not
  // of a loader default. Historical OPRA definitions can omit both, so they come
  // from the curated registry keyed on the board's own identity, with an explicit
  // spec value — a caller stating the convention itself — always winning.
  const InstrumentConventions conventions = [&spec, &board_key]() noexcept {
    InstrumentConventions resolved = us_listed_conventions(board_key);
    if (spec.expiry_close.has_value()) {
      resolved.expiry_close = *spec.expiry_close;
    }
    if (spec.exercise_style.has_value()) {
      resolved.exercise_style = *spec.exercise_style;
    }
    return resolved;
  }();
  const SettlementSession settlement =
      conventions.expiry_close == ExpiryCloseConvention::UsIndexAmOpen ? SettlementSession::Am
                                                                       : SettlementSession::Pm;

  for (std::size_t t = 0; t < n_visit; ++t) {
    const std::size_t i = row_at(t);
    const std::string_view und = has_underlying ? underlyings[i] : std::string_view{};
    if (!filter.empty() && und != filter) {
      continue; // filtered out (not a drop)
    }
    // T6: an unset ASK is still a hard drop — a bid alone bounds nothing from
    // above. An unset BID is kept (as `bid = 0`) when the caller admits
    // one-sided quotes; see `OpraLoadSpec::admit_one_sided_quotes`.
    const bool bid_missing = bid_px[i] == kUnsetPx || bid_null[i] != 0;
    const bool ask_missing = ask_px[i] == kUnsetPx || ask_null[i] != 0;
    if (ask_missing || (bid_missing && !spec.admit_one_sided_quotes)) {
      ++n_dropped;
      continue;
    }
    auto osi = parse_osi_symbol(symbols[i]);
    if (!osi.has_value()) {
      ++n_dropped;
      continue;
    }
    // Use the explicitly selected product settlement instant. Historical OPRA
    // definitions can omit settlement and exercise metadata, so callers must
    // choose the policy rather than infer it from the feed. This instant is what
    // T, the drop filter, and every downstream consumer see.
    const std::int64_t expiry_instant = expiry_instant_ns(osi->expiry_iso, settlement);
    // Drop only genuinely EXPIRED contracts: T <= 0 against the TRUE expiry
    // instant. Same-session (0DTE) contracts are now KEPT — before 16:00 ET they
    // carry a small positive intraday T (e.g. a 15:55 ET snapshot leaves ~5 min),
    // which is a tradeable forward node; only after the 16:00 ET settle does T go
    // non-positive and the contract drop. (Previously the midnight-UTC parse made
    // every same-day expiry T <= 0 and hard-dropped the highest-volume segment.)
    // A VolTime coverage failure (vol_time.hpp) is NOT a row-level drop: it
    // means the loader cannot decide whether this contract is live, so the load
    // fails rather than silently shrinking the board.
    ATX_TRY(const double t_row,
            time_to_expiry_years(snapshot_iso_ns, expiry_instant, spec.time));
    if (!(t_row > 0.0)) {
      ++n_dropped;
      continue;
    }
    const double bid = bid_missing ? 0.0 : static_cast<double>(bid_px[i]) * kPxScale;
    const double ask = static_cast<double>(ask_px[i]) * kPxScale;
    if (bid > ask) { // crossed
      ++n_dropped;
      continue;
    }
    // A bid-less row whose ask is also non-positive brackets nothing.
    if (bid_missing && !(ask > 0.0)) {
      ++n_dropped;
      continue;
    }

    if (first_underlying.empty() && !und.empty()) {
      first_underlying = std::string(und);
    }
    if (first_root.empty() && !osi->root.empty()) {
      first_root = osi->root;
    }

    QuoteRow row;
    // FIX-C-1. The row's uid is the hive's `underlying` column — the SAME string
    // the filter above matched, `frame.uid` is set from below, `board.symbol`
    // carries, and the manifest/archive key on. The OSI root is a FALLBACK, used
    // only when the file has no `underlying` column at all.
    //
    // Read the other way round (root first, column second) this line was the one
    // place a single underlier acquired TWO names. The two derivations are
    // byte-identical exactly when a DATA INVARIANT holds — per row, `underlying`
    // equals the OSI root of that row's own `symbol` — which is a property of the
    // hive, not of this code; see the guard below, which is what makes it a
    // checked precondition rather than an assumption. A production
    // 51-name build lost BRK.B entirely this way: the column said `BRK.B`, every
    // OSI symbol said `BRKB  260702C00270000`, so `data_install` interned BOTH,
    // filed all 1,838 quotes under `BRKB`, and handed the caller the handle to an
    // EMPTY `BRK.B` underlier. `OptionChain::from_frame` then succeeded over a
    // zero-expiry chain and config selection fail-closed-disabled the symbol.
    // Nothing compared the two strings, so nothing could report a mismatch.
    //
    // The dotted universe spelling is canonical because it is what the REST of
    // the pipeline already keys on end to end: the operator's `--symbols`, the
    // hive's discovery/filter (`opra_hive.cpp`), `CorpusBoard::symbol`, the
    // manifest's symbol table and the archive's `canonical_symbol` (which
    // preserves dots). The OSI root is a wire encoding of a contract, not the
    // identity of an underlier. Nothing on disk changes; only which of two
    // already-present strings the in-memory rows are keyed by.
    row.uid = !und.empty() ? std::string(und) : osi->root;
    // FIX-C-1 per-row identity guard — the one that can actually fire.
    //
    // Keying rows by the `underlying` column is only correct while that column
    // really does name the same underlier the row's OSI symbol does. Dots are the
    // expected, legitimate difference (`BRK.B` / `BRKB`): the OSI root namespace
    // cannot express punctuation, so those are two spellings of one identity and
    // the load proceeds. ANY OTHER difference is two different underliers, and
    // merging them is a silent pricing error rather than a lost symbol — the
    // adjusted-deliverable class is the live example: `pull_opra_hive.py:293-295`
    // strips a trailing digit from the root before mapping, so an `AAPL1` (post
    // corporate action, non-standard deliverable) row can carry
    // `underlying = "AAPL"`. Before FIX-C-1 such rows keyed to `AAPL1` and were
    // segregated into their own underlier — accidental, but protective, since an
    // adjusted deliverable is not comparable to the vanilla chain at the same
    // strike. Keying by the column alone would silently merge them into it.
    //
    // So: fail the cell LOUD (the hive loader tallies it in n_load_errors) rather
    // than drop the rows, which would silently change chain composition — the very
    // class of defect this fix exists to close. Verified not to fire on the live
    // hive: across all 140 date files the only raw divergence is
    // ('BRK.B','BRKB'), and after dot-stripping there is none at all.
    if (!und.empty() && !osi->root.empty() && und != osi->root &&
        !osi_root_matches_ticker(osi->root, und)) {
      return Err(ErrorCode::InvalidArgument,
                 "row symbol '" + std::string(symbols[i]) + "' has OSI root '" + osi->root +
                     "' but its underlying column says '" + std::string(und) +
                     "': these name two different underliers, not one");
    }
    row.expiry_iso = std::move(osi->expiry_iso);
    row.strike = osi->strike;
    row.side = osi->side;
    row.settle = settlement;
    row.exercise_style = conventions.exercise_style;
    row.expiry_ns = expiry_instant;
    row.bid = bid;
    row.ask = ask;
    constexpr std::int64_t kMaxQuoteCount =
        static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)());
    if (bid_sz[i] < 0 || ask_sz[i] < 0 || bid_sz[i] > kMaxQuoteCount ||
        ask_sz[i] > kMaxQuoteCount) {
      return Err(ErrorCode::OutOfRange,
                 "OPRA displayed size exceeds nonnegative int32 contract range");
    }
    // No bid means no size behind one, whatever the source column happens to
    // carry in that cell.
    row.bid_size = bid_missing ? 0 : static_cast<std::int32_t>(bid_sz[i]);
    row.ask_size = static_cast<std::int32_t>(ask_sz[i]);
    n_one_sided += bid_missing ? 1u : 0u;
    rows.push_back(std::move(row));
    if (has_instrument_id && instrument_ids[i] > 0 &&
        instrument_ids[i] <= static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) &&
        !numeric_symbol_fallback(symbols[i], instrument_ids[i])) {
      const std::uint32_t id = static_cast<std::uint32_t>(instrument_ids[i]);
      kept_instrument_ids.push_back(id);
      kept_mappings.try_emplace(id, std::string(symbols[i]));
    } else {
      kept_instrument_ids.push_back(0u);
    }
  }

  // The frame's default uid, in the SAME namespace every row above was keyed in
  // (the `underlying` column, OSI root only when that column is absent) — see the
  // FIX-C-1 note on `row.uid`. `spec.underlying` is both the row filter and this
  // default, so on a filtered load every kept row carries exactly this string;
  // the two fallbacks below mirror the row rule for an unfiltered load.
  std::string frame_uid = spec.underlying;
  if (frame_uid.empty()) {
    frame_uid = !first_underlying.empty() ? first_underlying : first_root;
  }

  // (snapshot_ts_ns / snapshot_iso computed above the row loop.)

  // ── P2-3 term-structure yield curve ─────────────────────────────────────
  // Caller-supplied pillars define a real term curve queried at each maturity;
  // absent them we keep the historical flat {T=1, r=spec.r} pillar. A SINGLE
  // pillar is treated as flat too: a 1-pillar YieldCurve interpolates as a
  // CONSTANT discount factor (zero(T) = r*T0/T, NOT a flat rate), so the flat /
  // single-pillar path uses the scalar rate directly and never routes through
  // the curve — keeping it BIT-IDENTICAL to the pre-P2-3 behavior (this is the
  // SPY/XOM held-quality invariant).
  if (spec.yc_pillar_t.size() != spec.yc_pillar_r.size()) {
    return Err(ErrorCode::InvalidArgument, "OpraLoadSpec.yc_pillar_t/_r length mismatch");
  }
  const bool has_term_curve = spec.yc_pillar_t.size() >= 2;
  YieldCurve yield;
  if (has_term_curve) {
    ATX_TRY(auto yc, YieldCurve::create(spec.yc_pillar_t, spec.yc_pillar_r));
    yield = std::move(yc);
  }
  // The scalar rate the flat / single-pillar path uses: the sole supplied
  // pillar's rate, else spec.r (the historical scalar, verbatim).
  const double flat_r = spec.yc_pillar_r.empty() ? spec.r : spec.yc_pillar_r.front();
  const auto rate_at = [&](double T) -> double { return has_term_curve ? yield.zero(T) : flat_r; };

  QuoteFrame frame;
  frame.uid = std::move(frame_uid);
  frame.snapshot_iso = snapshot_iso;
  frame.snapshot_ts_ns = snapshot_ts_ns;
  frame.spot_ts_ns = snapshot_ts_ns;
  // Stamp the governing T convention onto the frame itself (QuoteFrame::time):
  // `data_install` reads it for Chain::T, so the installed chains automatically
  // share the convention every T computed below (PCP spot, 0DTE filter, term
  // rates) was derived under — no caller threading required.
  frame.time = spec.time;
  if (spec.yc_pillar_t.empty()) {
    frame.yc_pillar_t = {1.0};
    frame.yc_pillar_r = {spec.r};
  } else {
    frame.yc_pillar_t = spec.yc_pillar_t;
    frame.yc_pillar_r = spec.yc_pillar_r;
  }
  frame.divs = spec.cash_divs;
  frame.rows = std::move(rows);

  // Per-expiry rate wiring (P2-3): when the caller supplied pillars, stamp each
  // kept row's source rate with the curve rate at its expiry's year-fraction so
  // build_expiry_inputs surfaces the per-(uid, expiry) term rate. Left UNTOUCHED
  // on the no-pillars flat path (rate_source stays NaN, ExpiryInputs.rate
  // absent) so that path's frame is byte-for-byte the historical one.
  if (!spec.yc_pillar_t.empty()) {
    for (QuoteRow &row : frame.rows) {
      // Use the row's TRUE stamped expiry instant (16:00 ET), so the term-curve
      // rate is queried at the same T `data_install` will assign to Chain::T.
      ATX_TRY(const double T, time_to_expiry_years(snapshot_iso_ns, row.expiry_ns, spec.time));
      if (std::isfinite(T) && T > 0.0) {
        row.rate_source = rate_at(T);
      }
    }
  }

  ATX_TRY_VOID(build_uid_list(frame));
  // FIX-C-1 frame-level tripwire. A FILTERED load addresses exactly ONE underlier,
  // so its frame must carry exactly ONE uid — `spec.underlying`, seeded first by
  // build_uid_list and then matched by every kept row. Unfiltered (discover/mixed)
  // loads are exempt: several uids are their normal shape.
  //
  // Be precise about what this is and is not. Given the row-uid rule above it is
  // UNREACHABLE on today's code — the filter at the top of the row loop drops every
  // row whose `underlying` differs from `spec.underlying`, so every kept row's uid
  // IS the frame's uid and build_uid_list dedupes to one. The *reachable* identity
  // check is the per-row `osi_root_matches_underlying` guard above; this one is a
  // cheap regression tripwire that fails loudly if a future edit reintroduces a
  // second derivation of the frame's name, which is exactly how BRK.B was lost.
  // Kept for that reason, not because it can fire today.
  if (!filter.empty() && frame.uid_strs.size() > 1u) {
    std::string list;
    for (std::size_t j = 0; j < frame.uid_strs.size(); ++j) {
      if (j != 0) {
        list.push_back(',');
      }
      list.append(frame.uid_strs[j]);
    }
    return Err(ErrorCode::InvalidArgument, "underlying '" + std::string(filter) +
                                               "': rows resolved to more than one uid {" + list +
                                               "} (frame uid and row uids disagree)");
  }
  build_expiry_inputs(frame);

  const std::size_t n_contracts = frame.rows.size();

  std::vector<std::string_view> distinct_expiries;
  for (const QuoteRow &row : frame.rows) {
    if (std::find(distinct_expiries.begin(), distinct_expiries.end(), row.expiry_iso) ==
        distinct_expiries.end()) {
      distinct_expiries.push_back(row.expiry_iso);
    }
  }
  const std::size_t n_expiries = distinct_expiries.size();

  double implied_spot = 0.0;
  if (spec.spot_override > 0.0) {
    implied_spot = spec.spot_override;
  } else {
    ATX_TRY(const double s, imply_spot_from_pcp(frame, rate_at, spec.time, settlement));
    implied_spot = s;
  }
  frame.spot = implied_spot;

  OpraPanel panel;
  panel.frame = std::move(frame);
  panel.implied_spot = implied_spot;
  panel.snapshot_iso = std::move(snapshot_iso);
  panel.n_contracts = n_contracts;
  panel.n_expiries = n_expiries;
  panel.n_dropped = n_dropped;
  panel.n_one_sided = n_one_sided;
  panel.source_schema_version = has_instrument_id ? 2u : 1u;
  panel.source_fingerprint = source_fingerprint(panel.frame, kept_instrument_ids, kept_mappings,
                                                panel.source_schema_version);
  panel.provenance_complete = provenance_complete;
  panel.bid_size_available = true;
  panel.ask_size_available = true;
  // cbbo-1m has no interval-volume or open-interest columns.
  panel.volume_available = false;
  panel.open_interest_available = false;
  panel.source_instrument_ids = std::move(kept_instrument_ids);
  panel.source_identities.reserve(kept_mappings.size());
  for (auto &[instrument_id, raw_symbol] : kept_mappings) {
    panel.source_identities.push_back(OpraInstrumentIdentity{instrument_id, std::move(raw_symbol)});
  }
  panel.fit_context = spec.fit_context;
  panel.market_input_provenance = spec.market_input_provenance;
  panel.time = spec.time;
  return Ok(std::move(panel));
}

Result<OpraPanel> panel_from_table(const io::ParquetTable &table, const OpraLoadSpec &spec) {
  ATX_TRY(const OpraTableScan scan, scan_table(table));
  return panel_from_scan(scan, spec);
}

// The 8 canonical hive-v2 columns. Shared by the per-symbol table seam and the
// per-table scan entry point so both reject the same file with the same words.
[[nodiscard]] Status require_hive_columns(const io::ParquetTable &table, std::string_view path) {
  static constexpr std::array<std::string_view, 8> kRequiredColumns = {
      "ts", "underlying", "symbol", "instrument_id", "bid_px", "ask_px", "bid_sz", "ask_sz"};
  const io::Schema &schema = table.schema();
  for (const std::string_view name : kRequiredColumns) {
    if (schema.find(name) == nullptr) {
      return Err(ErrorCode::InvalidArgument, "OPRA table missing required column '" +
                                                 std::string(name) + "' (from '" +
                                                 std::string(path) + "')");
    }
  }
  return Ok();
}

} // namespace

Result<OpraPanel> load_opra_cbbo_from_table(const io::ParquetTable &table,
                                            const OpraLoadSpec &spec) {
  // The in-memory-table seam requires the full canonical OPRA schema — the 8
  // columns the hive v2 date partition guarantees. A missing column is a
  // contract violation reported up front with the column name and spec.path
  // context, rather than the opaque deep column_view failure the raw core would
  // otherwise surface to a caller that never touched a file.
  ATX_TRY_VOID(require_hive_columns(table, spec.path));
  return panel_from_table(table, spec);
}

Result<OpraTableScan> scan_opra_cbbo_table(const io::ParquetTable &table,
                                           std::string_view path_for_errors) {
  ATX_TRY_VOID(require_hive_columns(table, path_for_errors));
  return scan_table(table);
}

Result<OpraPanel> load_opra_cbbo_from_scan(const OpraTableScan &scan, const OpraLoadSpec &spec) {
  // An empty filter has no group to look up: `panel_from_scan` would silently
  // fall back to the full-table path, which is a whole-table load wearing a
  // per-symbol call's clothes. Reject it here rather than serve it.
  if (spec.underlying.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "load_opra_cbbo_from_scan requires OpraLoadSpec.underlying (from '" + spec.path +
                   "')");
  }
  return panel_from_scan(scan, spec);
}

} // namespace atx::vol
