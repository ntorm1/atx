#pragma once

// chain_export — the SCHEMA + ROW-ASSEMBLY seam of `atx-vol-chain-export`.
//
// The tool emits one `tblOptionIntradayHist`-shaped Parquet row per listed
// contract of one session: the market slice we read, the fair value and fair
// vol we fitted, and the nine SpiderRock greeks. This header owns every
// decision that can silently corrupt such a row — the vendor column contract,
// the pinned greek convention scales, the cash-settled-index spot fallback, and
// the sentinel census — so each is reachable from `atx-vol-tests` without
// spawning the CLI. `chain_export_main.cpp` keeps only argv parsing, I/O and
// the fit loop. Gate: `ChainExport*` (tests/chain_export_test.cpp).
//
// ## The column contract
//
// `kColumnNames` uses the VENDOR's exact spellings so a consumer can union our
// output with the SpiderRock store. The names and physical types are taken from
// the enforced 64-column schema in `atx-vol/scripts/oracle_store_metadata.py`
// and were re-confirmed by reading the live drop at
// `C:/atx-cache/oracle/spiderrock/date=2026-08-14/bucket_et=0940`. We emit a
// SUBSET of that schema; every column we do emit carries the vendor's name and
// the vendor's physical type.
//
// Two facts from that inspection are load-bearing and are NOT what a reader
// would guess:
//   * `okey_cp` is spelled "Call"/"Put", not "C"/"P".
//   * `date` and `timestamp` are STRING stamps formatted
//     "YYYY-MM-DD HH:MM:SS.ffffff" in UTC (`bucket_et=0940` carries
//     "2026-08-14 13:40:00.000000"), while `tradingDate` is a bare
//     "YYYY-MM-DD".
//
// ## The missing sentinel
//
// The vendor writes -99 for a field it could not produce
// (`atx-vol/scripts/oracle_ingest.py`). Every field of a default-constructed
// `ExportRow` is already that sentinel, so a value only ever appears in the
// output because the driver COMPUTED it. `ExportColumns` counts sentinels per
// column as rows are appended and the CLI prints that census — a silent -99 is
// worse than a refusal.
//
// The census recognises a sentinel by exact equality with -99, which cannot
// distinguish a genuinely computed -99.0 from an unset field. That ambiguity is
// inherent to the vendor's own convention; it is bounded here by the fact that
// nothing in the driver writes a sentinel except by leaving a default in place.
//
// ## Ownership / thread-safety
//
// Every type here is a value aggregate (Rule of Zero). The free functions are
// pure. `ExportColumns` is a plain owning column store: one writer at a time,
// and `write_columns()` hands out spans that BORROW it (see that function's
// contract).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/datetime.hpp"              // time::Date, civil_from_days
#include "atx/core/error.hpp"                 // Status
#include "atx/core/io/parquet_writer.hpp"     // WriteColumn, ParquetRowGroupWriter
#include "atx/vol/api/core/types.hpp"         // Side
#include "atx/vol/api/fitting/session.hpp"    // VolaSession
#include "atx/vol/api/fitting/vol_curve.hpp"  // VolCurveKind, CurveSurface, IVolCurve
#include "atx/vol/api/pricing/american.hpp"   // AmericanGreeks
#include "atx/vol/api/storage/surface_db.hpp" // SymbolFitConfig

namespace atx::vol::chainexport {

// ── The vendor missing sentinel ─────────────────────────────────────────────

inline constexpr double kMissingF64 = -99.0;
inline constexpr std::int64_t kMissingI64 = -99;

// ── The emitted column contract ─────────────────────────────────────────────
//
// Declaration order IS the Parquet column order, and `kColumnNames` is indexed
// by this enum. Adding a column means adding it to BOTH, plus a vector, an
// append step and a `write_columns` entry — the tests pin the correspondence.
enum class Col : std::size_t {
  OkeyTk = 0,
  OkeyYr,
  OkeyMn,
  OkeyDy,
  OkeyXx,
  OkeyCp,
  UndSecKeyTk,
  UBid,
  UAsk,
  UPrc,
  BidPrc,
  AskPrc,
  BidSz,
  AskSz,
  SrPrc,
  SrVol,
  De,
  Ga,
  Th,
  Ve,
  Rh,
  Ph,
  Vo,
  Va,
  DeDecay,
  Rate,
  Sdiv,
  Ddiv,
  Years,
  Date,
  Timestamp,
  TradingDate,
  Error,
  Count
};

inline constexpr std::size_t kColumnCount = static_cast<std::size_t>(Col::Count);

inline constexpr std::array<std::string_view, kColumnCount> kColumnNames{
    "okey_tk", "okey_yr", "okey_mn", "okey_dy", "okey_xx",  "okey_cp",  "undSecKey_tk",
    "uBid",    "uAsk",    "uPrc",    "bidPrc",  "askPrc",   "bidSz",    "askSz",
    "srPrc",   "srVol",   "de",      "ga",      "th",       "ve",       "rh",
    "ph",      "vo",      "va",      "deDecay", "rate",     "sdiv",     "ddiv",
    "years",   "date",    "timestamp", "tradingDate", "error"};

[[nodiscard]] constexpr std::size_t col_index(Col c) noexcept {
  return static_cast<std::size_t>(c);
}

// ── The pinned convention map ───────────────────────────────────────────────
//
// PINNED, NOT DERIVED. These nine numbers are the greek half of `kWinner` in
// `oracle/canonical:atx-vol/tools/oracle_conventions.cpp` — the map the
// deterministic Stage 3 convention sweep resolved and that every oracle gate
// re-derives and checks. They are restated here (rather than linked) because
// that TU belongs to the oracle bench and pulls its whole cohort-reader stack;
// `ChainExport.ProductionScalesEqualTheOracleCanonicalWinner` is the guard that
// makes a drift a test failure rather than a silent unit change.
struct GreekScales {
  double delta{1.0};
  double gamma{1.0};
  double theta{1.0 / 252.0};
  double vega{0.01};
  double rho{0.01};
  double phi{0.01};
  double volga{1.0e-4};
  double vanna{0.01};
  double delta_decay{1.0 / 252.0};
};

inline constexpr GreekScales kProductionGreekScales{};

// The nine SpiderRock greeks, in the vendor's own units.
//
// Defaults are the missing sentinel: an unset greek is a counted refusal, never
// a zero that reads as "flat".
struct VendorGreeks {
  double de{kMissingF64};
  double ga{kMissingF64};
  double th{kMissingF64};
  double ve{kMissingF64};
  double rh{kMissingF64};
  double ph{kMissingF64};
  double vo{kMissingF64};
  double va{kMissingF64};
  double de_decay{kMissingF64};
};

// Scale one raw `AmericanGreeks` bundle plus its carry sensitivity into the
// vendor's units.
//
// The eight-member bundle carries no carry-rho, so `dp_dq` (from
// `american_carry_greeks_al(...).dP_dq`) is passed alongside; `deDecay` is the
// bundle's `charm`. Both mappings are the ones `apply_convention_scales` uses in
// the canonical convention TU.
//
// A non-finite input maps to the missing sentinel rather than to a scaled NaN:
// publishing `nan * 0.01` as a number is exactly the failure this tool exists
// to avoid.
[[nodiscard]] inline VendorGreeks scale_greeks(const AmericanGreeks &g, double dp_dq,
                                               const GreekScales &s) noexcept {
  const auto scaled = [](double raw, double scale) noexcept -> double {
    const double v = raw * scale;
    return std::isfinite(v) ? v : kMissingF64;
  };
  VendorGreeks out;
  out.de = scaled(g.delta, s.delta);
  out.ga = scaled(g.gamma, s.gamma);
  out.th = scaled(g.theta, s.theta);
  out.ve = scaled(g.vega, s.vega);
  out.rh = scaled(g.rho, s.rho);
  out.ph = scaled(dp_dq, s.phi);
  out.vo = scaled(g.volga, s.volga);
  out.va = scaled(g.vanna, s.vanna);
  out.de_decay = scaled(g.charm, s.delta_decay);
  return out;
}

// Is the greek bundle meaningful for a row whose published `srVol` is
// `model_iv`?
//
// `AmericanGreeks` is evaluated AT the surface's model IV
// (`VolaSession::greeks` computes `sigma = model_iv(k, T)` and differentiates
// there), so a row for which the surface produced no usable IV has no point at
// which greeks are defined. The engine's degenerate short-circuit nevertheless
// hands back an ALL-ZERO bundle, and a zero delta on a deep-ITM contract reads
// as a fact rather than as the refusal it is — measured on the 2026-08-21
// 10-name run, where the five names whose fit dropped the 0DTE slice emitted
// 1,296 rows of `srVol = -99` beside `de = 0`. Gate the whole bundle on the IV
// so those rows say "not computed" nine times over.
[[nodiscard]] inline bool greeks_are_defined(double model_iv) noexcept {
  return std::isfinite(model_iv) && model_iv > 0.0; // rejects NaN, +-inf, 0 and the sentinel
}

// ── The cash-settled index complex ──────────────────────────────────────────
//
// A LIST, NOT AN INFERENCE — the same discipline the oracle's exercise-style
// tables follow. These OSI roots are cash-settled index series whose underlier
// is an index LEVEL, so no equity/ETF NBBO feed carries a row for them (verified
// on `C:/atx-data/underlier-hive/date=2026-08-21/underlier.parquet`: all 14 are
// present in the OPRA board and none of them in the 8,908-ticker feed). For
// these the only spot we have is the board's own put-call-parity forward, and
// `uBid`/`uAsk` are legitimately sentinel. Sorted so the lookup can bisect.
inline constexpr std::array<std::string_view, 14> kCashSettledIndexRoots{
    "DJX", "MRUT", "MXEA", "MXEF", "NDX", "NDXP", "RUT",
    "RUTW", "SPX", "SPXW", "VIX", "XEO", "XND", "XSP"};

static_assert(std::is_sorted(kCashSettledIndexRoots.begin(), kCashSettledIndexRoots.end()),
              "kCashSettledIndexRoots must stay sorted: the lookup bisects it");

[[nodiscard]] inline bool is_cash_settled_index_root(std::string_view root) noexcept {
  return std::binary_search(kCashSettledIndexRoots.begin(), kCashSettledIndexRoots.end(), root);
}

// ── Underlier resolution ────────────────────────────────────────────────────

// One underlier NBBO row, already decoded out of the feed's 1e-9 fixed point.
struct NbboQuote {
  double bid{0.0};
  double ask{0.0};
};

enum class SpotSource : std::uint8_t {
  Nbbo,           // two-sided underlier quote from the feed
  ParityForward,  // the board's own put-call-parity implied spot
  Unavailable,    // neither; every underlier field stays sentinel
};

struct UnderlierFields {
  double u_bid{kMissingF64};
  double u_ask{kMissingF64};
  double u_prc{kMissingF64};
  SpotSource source{SpotSource::Unavailable};
};

// Resolve `uBid`/`uAsk`/`uPrc` for one board.
//
// @param symbol      the board's OSI root / feed ticker.
// @param feed        the underlier-hive NBBO row for `symbol`, or nullptr when
//                    the feed carries none. BORROWED for the call.
// @param parity_spot the panel's put-call-parity implied spot (`<= 0` or
//                    non-finite when the board implied none).
//
// Precedence, and the reason for it:
//   1. A cash-settled index root NEVER reads `feed`, even when one is offered.
//      An index level has no equity NBBO; a row that appeared under such a
//      ticker would be a different instrument, and publishing it as `uPrc`
//      would be a wrong spot rather than a missing one.
//   2. Otherwise a two-sided, positive, uncrossed feed quote wins: `uBid`/`uAsk`
//      verbatim and `uPrc` their mid.
//   3. Otherwise (no row, or an unusable one) `uBid`/`uAsk` stay sentinel and
//      `uPrc` falls back to the parity forward.
//   4. With no parity forward either, all three stay sentinel.
[[nodiscard]] inline UnderlierFields resolve_underlier(std::string_view symbol,
                                                       const NbboQuote *feed,
                                                       double parity_spot) noexcept {
  UnderlierFields out;
  if (!is_cash_settled_index_root(symbol) && feed != nullptr) {
    const bool usable = std::isfinite(feed->bid) && std::isfinite(feed->ask) && feed->bid > 0.0 &&
                        feed->ask >= feed->bid;
    if (usable) {
      out.u_bid = feed->bid;
      out.u_ask = feed->ask;
      out.u_prc = 0.5 * (feed->bid + feed->ask);
      out.source = SpotSource::Nbbo;
      return out;
    }
  }
  if (std::isfinite(parity_spot) && parity_spot > 0.0) {
    out.u_prc = parity_spot;
    out.source = SpotSource::ParityForward;
  }
  return out;
}

// ── Contract key helpers ────────────────────────────────────────────────────

struct ExpiryYmd {
  std::int64_t year{kMissingI64};
  std::int64_t month{kMissingI64};
  std::int64_t day{kMissingI64};
};

// Split a settlement instant (UTC epoch-ns) into `okey_yr`/`okey_mn`/`okey_dy`.
//
// The loader stamps expiries at the DST-correct 16:00 ET close (20:00Z / 21:00Z)
// or 09:30 ET for AM-settled index series (13:30Z / 14:30Z); every one of those
// instants falls on the expiry's own UTC calendar day, so the UTC civil date IS
// the listed expiry date. A non-positive instant is unresolvable and yields the
// sentinel triple rather than 1970-01-01.
[[nodiscard]] inline ExpiryYmd expiry_ymd_from_ns(std::int64_t expiry_ns) noexcept {
  if (expiry_ns <= 0) {
    return ExpiryYmd{};
  }
  constexpr std::int64_t kNsPerDay = 86'400'000'000'000LL;
  const atx::core::time::Date d = atx::core::time::civil_from_days(expiry_ns / kNsPerDay);
  return ExpiryYmd{static_cast<std::int64_t>(d.year), static_cast<std::int64_t>(d.month),
                   static_cast<std::int64_t>(d.day)};
}

[[nodiscard]] constexpr std::string_view side_label(Side s) noexcept {
  switch (s) {
  case Side::Call:
    return "Call";
  case Side::Put:
    return "Put";
  }
  return {};
}

// Join a session date and a hive snapshot suffix into the vendor's stamp shape.
//
// `date_iso` is "YYYY-MM-DD" and `snapshot_suffix` is the hive spec's
// "T HH:MM:SS Z" (already format-validated by the CLI). The result is
// "YYYY-MM-DD HH:MM:SS.000000" in UTC — the spelling the live drop uses.
//
// The sub-second field is a literal ".000000" and that is a claim we can make:
// an OPRA cbbo-1m board is a MINUTE-SYNCHRONISED cross-section, so every row's
// observation instant is the snapshot minute itself. (The vendor's own bucket is
// not synchronised — its rows carry per-row last-update stamps scattered across
// and past the minute.)
//
// @return the stamp, or an EMPTY string when either input is malformed — the
//         caller treats that as a usage error rather than stamping a guess.
[[nodiscard]] inline std::string vendor_stamp(std::string_view date_iso,
                                              std::string_view snapshot_suffix) {
  const auto is_digit = [](char c) noexcept { return c >= '0' && c <= '9'; };
  if (date_iso.size() != 10 || date_iso[4] != '-' || date_iso[7] != '-') {
    return {};
  }
  for (const std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
    if (!is_digit(date_iso[i])) {
      return {};
    }
  }
  if (snapshot_suffix.size() != 10 || snapshot_suffix[0] != 'T' || snapshot_suffix[3] != ':' ||
      snapshot_suffix[6] != ':' || snapshot_suffix[9] != 'Z') {
    return {};
  }
  for (const std::size_t i : {1u, 2u, 4u, 5u, 7u, 8u}) {
    if (!is_digit(snapshot_suffix[i])) {
      return {};
    }
  }
  std::string out;
  out.reserve(26);
  out.append(date_iso);
  out.push_back(' ');
  out.append(snapshot_suffix.substr(1, 8)); // HH:MM:SS
  out.append(".000000");
  return out;
}

// ── Universe flags ──────────────────────────────────────────────────────────

namespace detail {

[[nodiscard]] constexpr char ascii_upper(char c) noexcept {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

[[nodiscard]] constexpr bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

// Trim, ASCII-upper-case, and append `token` to `out` unless it is blank.
//
// Upper-casing is deliberate: the hive's `underlying` namespace is upper-case
// (dots preserved, and `ascii_upper` leaves them alone), and a lower-case token
// would otherwise become a silent coverage hole under a name the operator never
// typed rather than a diagnostic.
inline void append_symbol(std::string_view token, std::vector<std::string> &out) {
  std::size_t b = 0;
  std::size_t e = token.size();
  while (b < e && is_space(token[b])) {
    ++b;
  }
  while (e > b && is_space(token[e - 1])) {
    --e;
  }
  if (b == e) {
    return;
  }
  std::string s(token.substr(b, e - b));
  for (char &c : s) {
    c = ascii_upper(c);
  }
  out.push_back(std::move(s));
}

} // namespace detail

// Split a `--symbols` value ("SPY,AAPL,BRK.B") into trimmed, upper-cased tokens
// in the GIVEN ORDER (`load_opra_hive` lays its entry grid out date-major x the
// caller's order, so sorting here would reorder every report). Blank tokens are
// skipped; an all-blank value yields an empty vector, which the caller must
// treat as an error — an empty `OpraHiveSpec::symbols` is the hive's DISCOVERY
// switch, not "no symbols".
[[nodiscard]] inline std::vector<std::string> parse_symbol_csv(std::string_view text) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t comma = text.find(',', pos);
    const std::size_t end = (comma == std::string_view::npos) ? text.size() : comma;
    detail::append_symbol(text.substr(pos, end - pos), out);
    if (comma == std::string_view::npos) {
      break;
    }
    pos = comma + 1;
  }
  return out;
}

// Read a `--symbols-file`: one symbol per line, file order preserved. A line
// whose first non-blank character is `#` is a comment; blank lines are skipped;
// leading/trailing whitespace (`\r` included, so a CRLF file parses either way)
// is trimmed. There are no inline comments — the whole trimmed line is the
// symbol.
//
// @return false iff the file could not be opened. An EMPTY parse is `true` with
//         an empty `out`; the caller decides (and must refuse, per the
//         discovery-switch note on `parse_symbol_csv`).
// STRONG GUARANTEE: `out` is untouched unless the whole file parsed.
[[nodiscard]] inline bool read_symbol_file(const std::string &path, std::vector<std::string> &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::vector<std::string> parsed;
  std::string line;
  while (std::getline(in, line)) {
    std::size_t b = 0;
    while (b < line.size() && detail::is_space(line[b])) {
      ++b;
    }
    if (b < line.size() && line[b] == '#') {
      continue;
    }
    detail::append_symbol(std::string_view(line), parsed);
  }
  if (in.bad()) {
    return false;
  }
  out = std::move(parsed);
  return true;
}

// ── One emitted row ─────────────────────────────────────────────────────────
//
// Every field defaults to the missing sentinel. The driver overwrites only what
// it actually computed, so "did not compute" and "computed -99" are the same
// bytes on disk (the vendor's convention) but the census still counts them.
struct ExportRow {
  std::string okey_tk;
  std::int64_t okey_yr{kMissingI64};
  std::int64_t okey_mn{kMissingI64};
  std::int64_t okey_dy{kMissingI64};
  double okey_xx{kMissingF64};
  std::string okey_cp;
  std::string und_sec_key_tk;
  double u_bid{kMissingF64};
  double u_ask{kMissingF64};
  double u_prc{kMissingF64};
  double bid_prc{kMissingF64};
  double ask_prc{kMissingF64};
  std::int64_t bid_sz{kMissingI64};
  std::int64_t ask_sz{kMissingI64};
  double sr_prc{kMissingF64};
  double sr_vol{kMissingF64};
  VendorGreeks greeks{};
  double rate{kMissingF64};
  double sdiv{kMissingF64};
  double ddiv{kMissingF64};
  double years{kMissingF64};
  std::string date;
  std::string timestamp;
  std::string trading_date;
  double error{kMissingF64};
};

// ── The column store ────────────────────────────────────────────────────────
//
// SoA accumulation of `ExportRow`s plus the per-column sentinel census. Rule of
// Zero (vectors + a counter array); one writer at a time.
class ExportColumns {
public:
  void reserve(std::size_t n) {
    okey_tk_.reserve(n);
    okey_yr_.reserve(n);
    okey_mn_.reserve(n);
    okey_dy_.reserve(n);
    okey_xx_.reserve(n);
    okey_cp_.reserve(n);
    und_tk_.reserve(n);
    u_bid_.reserve(n);
    u_ask_.reserve(n);
    u_prc_.reserve(n);
    bid_prc_.reserve(n);
    ask_prc_.reserve(n);
    bid_sz_.reserve(n);
    ask_sz_.reserve(n);
    sr_prc_.reserve(n);
    sr_vol_.reserve(n);
    de_.reserve(n);
    ga_.reserve(n);
    th_.reserve(n);
    ve_.reserve(n);
    rh_.reserve(n);
    ph_.reserve(n);
    vo_.reserve(n);
    va_.reserve(n);
    de_decay_.reserve(n);
    rate_.reserve(n);
    sdiv_.reserve(n);
    ddiv_.reserve(n);
    years_.reserve(n);
    date_.reserve(n);
    timestamp_.reserve(n);
    trading_date_.reserve(n);
    error_.reserve(n);
  }

  // Drop every row but KEEP the capacity and the schema, so the streaming sink
  // can reuse one buffer per board and pay the per-column allocation once.
  //
  // The census resets WITH the rows: a chunk's counts describe that chunk, and
  // the figure that has to survive the release lives in `ExportCensus`.
  void clear() noexcept {
    okey_tk_.clear();
    okey_yr_.clear();
    okey_mn_.clear();
    okey_dy_.clear();
    okey_xx_.clear();
    okey_cp_.clear();
    und_tk_.clear();
    u_bid_.clear();
    u_ask_.clear();
    u_prc_.clear();
    bid_prc_.clear();
    ask_prc_.clear();
    bid_sz_.clear();
    ask_sz_.clear();
    sr_prc_.clear();
    sr_vol_.clear();
    de_.clear();
    ga_.clear();
    th_.clear();
    ve_.clear();
    rh_.clear();
    ph_.clear();
    vo_.clear();
    va_.clear();
    de_decay_.clear();
    rate_.clear();
    sdiv_.clear();
    ddiv_.clear();
    years_.clear();
    date_.clear();
    timestamp_.clear();
    trading_date_.clear();
    error_.clear();
    sentinel_counts_.fill(0);
  }

  // Append one row, counting every column left at its missing sentinel.
  void append(const ExportRow &row) {
    push_str(Col::OkeyTk, okey_tk_, row.okey_tk);
    push_i64(Col::OkeyYr, okey_yr_, row.okey_yr);
    push_i64(Col::OkeyMn, okey_mn_, row.okey_mn);
    push_i64(Col::OkeyDy, okey_dy_, row.okey_dy);
    push_f64(Col::OkeyXx, okey_xx_, row.okey_xx);
    push_str(Col::OkeyCp, okey_cp_, row.okey_cp);
    push_str(Col::UndSecKeyTk, und_tk_, row.und_sec_key_tk);
    push_f64(Col::UBid, u_bid_, row.u_bid);
    push_f64(Col::UAsk, u_ask_, row.u_ask);
    push_f64(Col::UPrc, u_prc_, row.u_prc);
    push_f64(Col::BidPrc, bid_prc_, row.bid_prc);
    push_f64(Col::AskPrc, ask_prc_, row.ask_prc);
    push_i64(Col::BidSz, bid_sz_, row.bid_sz);
    push_i64(Col::AskSz, ask_sz_, row.ask_sz);
    push_f64(Col::SrPrc, sr_prc_, row.sr_prc);
    push_f64(Col::SrVol, sr_vol_, row.sr_vol);
    push_f64(Col::De, de_, row.greeks.de);
    push_f64(Col::Ga, ga_, row.greeks.ga);
    push_f64(Col::Th, th_, row.greeks.th);
    push_f64(Col::Ve, ve_, row.greeks.ve);
    push_f64(Col::Rh, rh_, row.greeks.rh);
    push_f64(Col::Ph, ph_, row.greeks.ph);
    push_f64(Col::Vo, vo_, row.greeks.vo);
    push_f64(Col::Va, va_, row.greeks.va);
    push_f64(Col::DeDecay, de_decay_, row.greeks.de_decay);
    push_f64(Col::Rate, rate_, row.rate);
    push_f64(Col::Sdiv, sdiv_, row.sdiv);
    push_f64(Col::Ddiv, ddiv_, row.ddiv);
    push_f64(Col::Years, years_, row.years);
    push_str(Col::Date, date_, row.date);
    push_str(Col::Timestamp, timestamp_, row.timestamp);
    push_str(Col::TradingDate, trading_date_, row.trading_date);
    push_f64(Col::Error, error_, row.error);
  }

  [[nodiscard]] std::size_t rows() const noexcept { return okey_tk_.size(); }

  [[nodiscard]] std::size_t sentinels(Col c) const noexcept {
    return sentinel_counts_[col_index(c)];
  }

  [[nodiscard]] std::size_t sentinel_total() const noexcept {
    std::size_t n = 0;
    for (const std::size_t c : sentinel_counts_) {
      n += c;
    }
    return n;
  }

  // The Parquet write plan, in `Col` order.
  //
  // BORROW, NOT A COPY: every returned `WriteColumn` holds a span into THIS
  // object's storage. The returned vector must not outlive this `ExportColumns`,
  // and no `append`/`reserve` may run while it is alive — either would
  // reallocate the vectors under the spans.
  [[nodiscard]] std::vector<atx::core::io::WriteColumn> write_columns() const {
    std::vector<atx::core::io::WriteColumn> cols;
    cols.reserve(kColumnCount);
    const auto add_str = [&](Col c, const std::vector<std::string> &v) {
      cols.push_back({std::string(kColumnNames[col_index(c)]), std::span<const std::string>(v)});
    };
    const auto add_i64 = [&](Col c, const std::vector<std::int64_t> &v) {
      cols.push_back({std::string(kColumnNames[col_index(c)]), std::span<const std::int64_t>(v)});
    };
    const auto add_f64 = [&](Col c, const std::vector<double> &v) {
      cols.push_back({std::string(kColumnNames[col_index(c)]), std::span<const double>(v)});
    };
    add_str(Col::OkeyTk, okey_tk_);
    add_i64(Col::OkeyYr, okey_yr_);
    add_i64(Col::OkeyMn, okey_mn_);
    add_i64(Col::OkeyDy, okey_dy_);
    add_f64(Col::OkeyXx, okey_xx_);
    add_str(Col::OkeyCp, okey_cp_);
    add_str(Col::UndSecKeyTk, und_tk_);
    add_f64(Col::UBid, u_bid_);
    add_f64(Col::UAsk, u_ask_);
    add_f64(Col::UPrc, u_prc_);
    add_f64(Col::BidPrc, bid_prc_);
    add_f64(Col::AskPrc, ask_prc_);
    add_i64(Col::BidSz, bid_sz_);
    add_i64(Col::AskSz, ask_sz_);
    add_f64(Col::SrPrc, sr_prc_);
    add_f64(Col::SrVol, sr_vol_);
    add_f64(Col::De, de_);
    add_f64(Col::Ga, ga_);
    add_f64(Col::Th, th_);
    add_f64(Col::Ve, ve_);
    add_f64(Col::Rh, rh_);
    add_f64(Col::Ph, ph_);
    add_f64(Col::Vo, vo_);
    add_f64(Col::Va, va_);
    add_f64(Col::DeDecay, de_decay_);
    add_f64(Col::Rate, rate_);
    add_f64(Col::Sdiv, sdiv_);
    add_f64(Col::Ddiv, ddiv_);
    add_f64(Col::Years, years_);
    add_str(Col::Date, date_);
    add_str(Col::Timestamp, timestamp_);
    add_str(Col::TradingDate, trading_date_);
    add_f64(Col::Error, error_);
    return cols;
  }

private:
  void push_f64(Col c, std::vector<double> &dst, double v) {
    if (v == kMissingF64) {
      ++sentinel_counts_[col_index(c)];
    }
    dst.push_back(v);
  }
  void push_i64(Col c, std::vector<std::int64_t> &dst, std::int64_t v) {
    if (v == kMissingI64) {
      ++sentinel_counts_[col_index(c)];
    }
    dst.push_back(v);
  }
  // A string column has no numeric sentinel; EMPTY is its "not resolved" state
  // (the vendor never writes an empty key), and it is counted the same way.
  void push_str(Col c, std::vector<std::string> &dst, const std::string &v) {
    if (v.empty()) {
      ++sentinel_counts_[col_index(c)];
    }
    dst.push_back(v);
  }

  std::vector<std::string> okey_tk_;
  std::vector<std::int64_t> okey_yr_;
  std::vector<std::int64_t> okey_mn_;
  std::vector<std::int64_t> okey_dy_;
  std::vector<double> okey_xx_;
  std::vector<std::string> okey_cp_;
  std::vector<std::string> und_tk_;
  std::vector<double> u_bid_;
  std::vector<double> u_ask_;
  std::vector<double> u_prc_;
  std::vector<double> bid_prc_;
  std::vector<double> ask_prc_;
  std::vector<std::int64_t> bid_sz_;
  std::vector<std::int64_t> ask_sz_;
  std::vector<double> sr_prc_;
  std::vector<double> sr_vol_;
  std::vector<double> de_;
  std::vector<double> ga_;
  std::vector<double> th_;
  std::vector<double> ve_;
  std::vector<double> rh_;
  std::vector<double> ph_;
  std::vector<double> vo_;
  std::vector<double> va_;
  std::vector<double> de_decay_;
  std::vector<double> rate_;
  std::vector<double> sdiv_;
  std::vector<double> ddiv_;
  std::vector<double> years_;
  std::vector<std::string> date_;
  std::vector<std::string> timestamp_;
  std::vector<std::string> trading_date_;
  std::vector<double> error_;
  std::array<std::size_t, kColumnCount> sentinel_counts_{};
};

// ── The cumulative census ───────────────────────────────────────────────────
//
// A STREAMED export has no whole-table object left to interrogate at the end —
// each board's storage is released the moment its row group is on disk — so the
// census has to outlive the chunks. Every written chunk folds into this before
// it is dropped, which keeps the reported figure EXACT rather than sampled: a
// census that under-counted would be worse than none, because the whole point of
// the -99 convention is that a silent sentinel is a lie.
struct ExportCensus {
  std::size_t rows{0};
  std::array<std::size_t, kColumnCount> sentinels{};

  // Fold one written chunk in. Call it ONLY for a chunk that reached the file:
  // the census describes the output, never the attempt.
  void merge(const ExportColumns &chunk) noexcept {
    rows += chunk.rows();
    for (std::size_t c = 0; c < kColumnCount; ++c) {
      sentinels[c] += chunk.sentinels(static_cast<Col>(c));
    }
  }

  [[nodiscard]] std::size_t sentinel_total() const noexcept {
    std::size_t n = 0;
    for (const std::size_t c : sentinels) {
      n += c;
    }
    return n;
  }
};

// ── The streaming sink ──────────────────────────────────────────────────────
//
// One Parquet row group per underlier, written as that underlier completes.
//
// WHY, precisely: the accumulate-then-write path built one Arrow table over the
// whole universe while STILL holding every assembled row, and on the 2026-08-21
// full board (1,636,354 rows across 2,834 fitted symbols) it died with
// "build table: Out of memory" after 164 s of work, leaving no file at all.
// Here the live set is one board's rows plus one board's Arrow arrays, so peak
// memory is bounded by the LARGEST board rather than by the universe.
//
// The row-group boundary is the underlier, matching the convention
// `pull_opra_hive.py::_write_date_file` established for this repo's hive files.
// A group then holds exactly one `okey_tk`, so that column's min == max within
// it and a downstream per-symbol read prunes on the file's own statistics
// instead of decoding the board.
//
// ORDER IS THE CALLER'S: `write_board` emits in call order and nothing is
// buffered across calls or reordered, so a driver that hands boards over in a
// fixed order gets a byte-identical file however many workers produced them.
//
// Ownership / thread-safety: owns the open file. ONE writer at a time and no
// internal locking — concurrent producers must funnel through a single instance,
// which is also what makes the row-group order independent of the worker count.
// Rule of Zero: the members are the resources.
class ChainExportWriter {
public:
  // Open `path` (parent dirs created) and fix the file's schema to the full
  // column contract. An empty `ExportColumns` is exactly that contract: names,
  // order and physical types with no rows.
  [[nodiscard]] atx::core::Status open(const std::string &path) {
    const ExportColumns schema;
    return writer_.open(schema.write_columns(), path);
  }

  // Write one underlier's rows as ONE row group, fold them into the census and
  // RELEASE their storage.
  //
  // An EMPTY board (a symbol that dropped) writes nothing and counts nothing —
  // an empty row group would only make the file's group count misdescribe it.
  //
  // @param rows BORROWED for the call; nothing here retains a reference.
  // @return the write error, if any. On failure the census is NOT advanced.
  // BASIC GUARANTEE: on failure the file is left for `close()` to finish or the
  // caller to discard; the sink's own buffer is cleared either way.
  [[nodiscard]] atx::core::Status write_board(std::span<const ExportRow> rows) {
    chunk_.clear();
    chunk_.reserve(rows.size());
    for (const ExportRow &row : rows) {
      chunk_.append(row);
    }
    const atx::core::Status wrote = writer_.write_row_group(chunk_.write_columns());
    if (wrote.has_value()) {
      census_.merge(chunk_);
    }
    chunk_.clear(); // the board is on disk (or lost): it is not ours to hold
    return wrote;
  }

  // Write the footer. The file is INCOMPLETE until this returns success.
  [[nodiscard]] atx::core::Status close() { return writer_.close(); }

  [[nodiscard]] const ExportCensus &census() const noexcept { return census_; }

  // Rows the sink is still holding — zero between boards, by construction. The
  // memory bound this class exists for is exactly this staying flat.
  [[nodiscard]] std::size_t buffered_rows() const noexcept { return chunk_.rows(); }

  [[nodiscard]] std::int64_t row_groups_written() const noexcept {
    return writer_.row_groups_written();
  }

private:
  atx::core::io::ParquetRowGroupWriter writer_;
  ExportColumns chunk_; // reused across boards; capacity survives, rows do not
  ExportCensus census_;
};

// ── --pin-curve: the pinned family, and the family actually SERVED ──────────
//
// The tool normally fits each symbol exactly the way its stored
// `SymbolFitConfig` says, which leaves the curve FAMILY to the library's
// auto-router. `--pin-curve <kind>` overrides that for the whole run so one
// chain parquet is produced under ONE named family and its provenance says
// which.
//
// Both seams live here rather than in the driver TU so `atx-vol-tests` can
// reach them without spawning the CLI, and they answer two DIFFERENT questions:
//
//   * `apply_curve_pin` — what was ASKED for. An unset pin is a no-op, so the
//     default run stays bit-identical to today's auto-routed one.
//   * `tally_served_curves` — what was actually SERVED, which is not the same
//     thing. A pinned risk candidate the independent geometry oracle refuses is
//     NOT substituted with another family: both the validation fallback ladder
//     and the strict ConvexDense recovery rung are gated on the fit being
//     auto-routed (or on a ConvexDense pin), so a rejected `spline-vol` pin
//     leaves the risk surface unserved and the row is priced off the MARK arm
//     instead. Without this census a spline-fitted row and a rescued one are
//     indistinguishable in the output, and a pin that never got served would
//     read as "the change did nothing".

// Force `cfg` onto the pinned family; no-op when `pin` is unset.
//
// Applied to the RESOLVED per-symbol config, so it covers both the stored
// SurfaceDb config and the `symbol_config_from_preset(FitPreset::Populate)`
// fallback a symbol missing from the manifest gets — otherwise that symbol
// would silently keep auto-routing and the run would mix two families.
//
// Only `CurveConfig::kind` is replaced. The parametric mirror and the
// family-specific knobs stay whatever the stored config (or the preset) carried,
// so this changes the FAMILY and nothing else.
inline void apply_curve_pin(const std::optional<VolCurveKind> &pin,
                            SymbolFitConfig &cfg) noexcept {
  if (!pin.has_value()) {
    return;
  }
  cfg.pin_curve = true;
  cfg.curve.kind = *pin;
}

// One past the last `VolCurveKind` enumerator — the tally's array width. Sized
// off the enum, exactly like the driver's drop-reason histogram, so a widened
// enum cannot overflow the array; a kind appended AFTER `SplineVol` still has to
// be named here, and until it is, `ServedCurveTally::add` drops it rather than
// writing out of bounds.
inline constexpr std::size_t kCurveKindCount =
    static_cast<std::size_t>(VolCurveKind::SplineVol) + 1u;

// Fitted-slice counts per curve family for ONE served surface, indexed by
// `VolCurveKind`. Aggregates by `merge` so the run census can report the same
// shape over the whole universe.
struct ServedCurveTally {
  std::array<std::size_t, kCurveKindCount> by_kind{};

  void add(VolCurveKind kind, std::size_t n = 1u) noexcept {
    const auto i = static_cast<std::size_t>(kind);
    if (i < by_kind.size()) {
      by_kind[i] += n;
    }
  }

  void merge(const ServedCurveTally &other) noexcept {
    for (std::size_t i = 0; i < by_kind.size(); ++i) {
      by_kind[i] += other.by_kind[i];
    }
  }

  [[nodiscard]] std::size_t count(VolCurveKind kind) const noexcept {
    const auto i = static_cast<std::size_t>(kind);
    return i < by_kind.size() ? by_kind[i] : 0u;
  }

  [[nodiscard]] std::size_t total() const noexcept {
    std::size_t n = 0;
    for (const std::size_t v : by_kind) {
      n += v;
    }
    return n;
  }

  // Census text: "spline-vol:12" / "essvi:9 svi:1", ascending by enumerator.
  // "-" when nothing was served, which is a REFUSAL to report a family, not a
  // family — a caller must not read it as eSSVI.
  [[nodiscard]] std::string describe() const {
    std::string out;
    for (std::size_t i = 0; i < by_kind.size(); ++i) {
      if (by_kind[i] == 0u) {
        continue;
      }
      if (!out.empty()) {
        out += ' ';
      }
      out += to_string(static_cast<VolCurveKind>(i));
      out += ':';
      out += std::to_string(by_kind[i]);
    }
    return out.empty() ? std::string("-") : out;
  }
};

// The family of every served slice of `curves`, or — when `curves` is nullptr —
// `n_essvi_slices` eSSVI slices.
//
// nullptr is NOT "unknown": `VolaSession` installs a polymorphic `CurveSurface`
// override only for a non-eSSVI family, so the default path having no override
// is exactly the statement that it served eSSVI slices (session.hpp,
// `curve_override`).
[[nodiscard]] inline ServedCurveTally tally_served_curves(const CurveSurface *curves,
                                                          std::size_t n_essvi_slices) {
  ServedCurveTally out;
  if (curves == nullptr) {
    out.add(VolCurveKind::Essvi, n_essvi_slices);
    return out;
  }
  for (const std::unique_ptr<IVolCurve> &slice : curves->slices()) {
    if (slice != nullptr) {
      out.add(slice->kind());
    }
  }
  return out;
}

[[nodiscard]] inline ServedCurveTally tally_served_curves(const VolaSession &session) {
  return tally_served_curves(session.curve_override(), session.expiries().size());
}

} // namespace atx::vol::chainexport
