#pragma once

// Offline data-ingestion for atx-vol: the in-memory quote frame and the
// install-into-universe path.
//
// Ported from the C17 `ats-vol` library (`ats_vol_data.{h,c}`). The C library
// loaded fixture/real-data schemas (CSV via ats-base's `AtsFrame` reader, or a
// bespoke SpiderRock Parquet decoder) into an arena-backed `AtsVolDataSnapshot`,
// then installed that snapshot into an `AtsVolUniverse`. This port keeps the
// snapshot representation (`QuoteFrame`) and the install semantics
// (`data_install`) faithful, dropping the arena + negative-int status channel
// for RAII containers and `Result<T>`/`Status` (agent profile §4).
//
// ## Scope of this port
//
// IN SCOPE (fully ported, tested):
//   - `QuoteFrame` / `QuoteRow` / `ExpiryInputs` — the snapshot representation,
//     including the Sprint-25 per-row SpiderRock source-input plane and the
//     per-(uid, expiry) source-input dedupe table.
//   - `iso_to_ns`, `year_fraction`, `ns_to_iso_date` — the ISO-8601 / civil
//     date kernels (`ats_vol_data_iso_to_ns`, `ats_vol_data_year_fraction`, and
//     the inline civil-from-days used by install's source-vol stamping).
//   - `build_uid_list` (`snapshot_build_uid_list`) — dedupe the row stream's
//     uids into `QuoteFrame::uid_strs`.
//   - `build_expiry_inputs` — the Sprint-25 per-(uid, expiry) source-input
//     dedupe (`sr_populate_snapshot`'s expiry-input table build), decoupled
//     from the Parquet reader so it runs over any frame's row source fields.
//   - `find_expiry_inputs` (`ats_vol_data_snapshot_find_expiry_inputs`).
//   - `data_install` (`ats_vol_data_install`) — the full install path: the
//     yield-curve gate, ticker interning, per-row validation, expiry/strike
//     growth, quote-plane writes (bids/asks/sizes/mids/ts/flags with
//     LOCKED/CROSSED), spot/spot-ts bookkeeping, chain `T` from
//     `year_fraction`, the post-install sort-chains-by-T pass, and the
//     source-side ATM-IV stamp onto each chain.
//
// DEFERRED (see the PORT NOTEs below):
//   - The CSV file loaders (`load_chain` / `load_spot` / `load_yield` /
//     `load_dividends` / `load_snapshot` / `load_synthetic` / `load_real`):
//     they depend on ats-base's `AtsFrame` CSV/frame reader, which atx-core
//     does not provide, and no CSV fixtures are committed to atx-vol. The
//     in-memory `QuoteFrame` + `data_install` is the supported ingestion path
//     (the C tests' in-memory cases drive exactly this).
//   - The SpiderRock Parquet loader — see `load_spiderrock_parquet` below.
//
// ## Ownership / thread-safety
//
// `QuoteFrame` owns its rows/pillars/inputs by value (RAII, Rule of Zero). The
// pointer returned by `find_expiry_inputs` is a non-owning borrow into the
// frame, valid while the frame is alive and its `expiry_inputs` vector is not
// mutated (same contract as the C's arena-borrowed pointer). `data_install`
// mutates the supplied `Universe` and requires exclusive access to it, matching
// the C loader's "single thread (mutates internal state)" contract.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/curve.hpp"    // DividendEvent
#include "atx/vol/types.hpp"    // Result, Status, Side
#include "atx/vol/universe.hpp" // Universe, Uid, Chain
#include "atx/vol/vol_time.hpp" // TimeSpec (data_install's T convention)

namespace atx::vol {

// Per-row uid string capacity (ATS_VOL_DATA_UID_STR_CAP). A uid carried on a
// data row must be strictly shorter than this (i.e. <= 15 chars) — the C's
// fixed 16-byte buffer includes the NUL. Note this is tighter than the
// universe's `kMaxTickerLen` (16); the loader enforces the data-plane cap.
inline constexpr std::size_t kDataUidStrCap = 16u;

// Quote-plane flag bits written by `data_install` (subset of ATS_VOL_QFLAG_*
// the loader sets; the universe stores raw `std::uint8_t` flags).
inline constexpr std::uint8_t kQFlagLocked = 0x01u;  // bid >= ask
inline constexpr std::uint8_t kQFlagCrossed = 0x02u; // bid >  ask

// ── Per-(uid, expiry) source-input completeness mask ────────────────────────
//
// Bit set iff the named SpiderRock source field was present and finite for the
// (uid, expiry) cell. Bit values are bit-for-bit identical to the C
// ATS_VOL_DATA_EXP_INPUT_* macros.
enum class ExpiryInputField : std::uint8_t {
  None = 0x00u,
  Rate = 0x01u,   // SR `rate`
  Sdiv = 0x02u,   // SR `sdiv`
  Ddiv = 0x04u,   // SR `ddiv`
  T = 0x08u,      // SR `years` (vol-time year fraction)
  AtmVol = 0x10u, // SR `atmVol`
};

[[nodiscard]] constexpr ExpiryInputField operator|(ExpiryInputField a,
                                                   ExpiryInputField b) noexcept {
  return static_cast<ExpiryInputField>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr ExpiryInputField operator&(ExpiryInputField a,
                                                   ExpiryInputField b) noexcept {
  return static_cast<ExpiryInputField>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
constexpr ExpiryInputField &operator|=(ExpiryInputField &a, ExpiryInputField b) noexcept {
  a = a | b;
  return a;
}
[[nodiscard]] constexpr bool has_flag(ExpiryInputField value, ExpiryInputField flag) noexcept {
  return (value & flag) != ExpiryInputField::None;
}

// The completeness the oracle pipeline treats as "fully specified": rate + T
// (ATS_VOL_DATA_EXP_INPUT_ALL_REQUIRED).
inline constexpr ExpiryInputField kExpiryInputAllRequired =
    ExpiryInputField::Rate | ExpiryInputField::T;

// Quiet-NaN shorthand for the "feed did not surface this column" sentinel,
// matching the C's NaN-as-unset convention on the source-input fields.
inline constexpr double kDataNaN = std::numeric_limits<double>::quiet_NaN();

// ── Quote row (AtsVolDataRow) ───────────────────────────────────────────────
//
// One (uid, expiry, strike, side) observation. `uid` may be empty, in which
// case the enclosing frame's default `uid` applies (`snapshot_row_uid`). The
// `*_source` fields are the Sprint-25 SpiderRock source-input plane: NaN when
// the feed did not surface the column. They are diagnostics only — never fit
// against them (they tie back to the oracle's own surface).
struct QuoteRow {
  std::string uid;        // optional per-row uid ("" -> frame default)
  std::string expiry_iso; // "YYYY-MM-DD"
  double strike = 0.0;
  Side side = Side::Call;
  double bid = 0.0;
  double ask = 0.0;
  std::int32_t bid_size = 0;
  std::int32_t ask_size = 0;
  double last = 0.0;
  std::int32_t volume = 0;
  std::int32_t open_interest = 0;
  double iv_source = 0.0;      // feed-reported IV (diagnostics)
  double fv_source = kDataNaN; // feed fair value/theo; NaN if absent
  double under_spot = 0.0;     // optional per-row underlying price
  std::int64_t ts_ns = 0;      // optional row timestamp

  // Sprint-25 SpiderRock source-input plane (NaN when absent).
  double rate_source = kDataNaN;    // SR `rate`   — annualized zero rate
  double sdiv_source = kDataNaN;    // SR `sdiv`   — continuous div yield
  double ddiv_source = kDataNaN;    // SR `ddiv`   — discrete-div cash to expiry
  double years_source = kDataNaN;   // SR `years`  — vol-time year fraction
  double atm_vol_source = kDataNaN; // SR `atmVol` — fitted ATM IV
  double vega_source = kDataNaN;    // SR `ve`     — feed-reported vega
  double delta_source = kDataNaN;   // SR `de`     — feed-reported delta

  // ── True expiry instant (G1) ─────────────────────────────────────────────
  // Settlement session for the contract (the AM/PM hook): PM = 16:00 ET is the
  // entire single-name/ETF universe; AM = 09:30 ET is reserved for cash-settled
  // index series. Default PM.
  SettlementSession settle = SettlementSession::Pm;
  // Exercise semantics for this contract. The default preserves the historical
  // single-name/ETF path; index loaders must source this from explicit product
  // policy because historical OPRA definitions do not reliably populate CFI.
  ExerciseStyle exercise_style = ExerciseStyle::American;
  // The TRUE expiry instant in UTC epoch-ns when a dated-contract loader has
  // stamped it (16:00/09:30 ET per `settle`, via `expiry_instant_ns`). 0 means
  // "derive at install from `expiry_iso`": `data_install` then falls back to the
  // legacy midnight-UTC parse, keeping every hand-built / synthetic frame
  // BIT-IDENTICAL to its historical `year_fraction`-derived T. The real OPRA
  // loader always stamps it, so its front / 0DTE (same-session) expiries carry
  // the correct intraday T instead of a ~0.8-trading-day-short midnight one.
  std::int64_t expiry_ns = 0;
};

// ── Per-(uid, expiry) source inputs (AtsVolDataExpiryInputs) ────────────────
//
// One SR snapshot writes the same (rate, sdiv, ddiv, years, atmVol) tuple
// across every strike of an expiry; `build_expiry_inputs` collapses the row
// stream to one cell per (uid, expiry_iso) so downstream consumers read SR's
// term structure without re-walking the rows. Loader contract: at most one
// entry per (uid, expiry_iso).
struct ExpiryInputs {
  std::string uid;
  std::string expiry_iso;  // "YYYY-MM-DD"
  double T_vol = kDataNaN; // SR `years`
  double rate = kDataNaN;
  double sdiv = kDataNaN;
  double ddiv = kDataNaN;
  double atm_vol = kDataNaN;
  ExpiryInputField completeness = ExpiryInputField::None;
};

// ── Quote frame (AtsVolDataSnapshot) ────────────────────────────────────────
//
// The in-memory snapshot a loader builds and `data_install` consumes. Owns all
// storage by value (RAII, Rule of Zero). `yc_pillar_t`/`yc_pillar_r` are the
// yield-curve pillars (used by install only as a non-empty precondition; the
// universe layer holds no curve). `divs` is carried for a faithful snapshot
// representation and is consumed by the curve layer, not by install.
struct QuoteFrame {
  std::string uid;          // first/default uid
  std::string snapshot_iso; // "YYYY-MM-DD" or "YYYY-MM-DD HH:MM:SS.ffffff"
  std::int64_t snapshot_ts_ns = 0;
  double spot = 0.0;
  std::int64_t spot_ts_ns = 0;

  std::vector<std::string> uid_strs; // distinct uids (built by build_uid_list)
  std::vector<QuoteRow> rows;

  std::vector<double> yc_pillar_t; // yield-curve pillar year-fractions
  std::vector<double> yc_pillar_r; // yield-curve pillar zero rates

  std::vector<DividendEvent> divs; // discrete cash-dividend schedule
  std::vector<ExpiryInputs> expiry_inputs;

  // The T convention this frame was BUILT under — the frame's single source of
  // truth for every year-fraction derived from it. A loader that computes any
  // T-dependent quantity while assembling the frame (e.g.
  // `load_opra_cbbo_parquet`: PCP spot implication, 0DTE drop filter, term-rate
  // stamping) must stamp the governing `TimeSpec` here; `data_install` then
  // reads THIS field for `Chain::T`, so the installed chains can never disagree
  // with the loader's own T math — a mixed-convention universe is impossible by
  // construction, with no caller threading required. Default Calendar365 keeps
  // every hand-built frame bit-identical to the historical
  // `year_fraction`-derived behavior.
  TimeSpec time{};
};

// ── ISO-8601 / civil date kernels ───────────────────────────────────────────

// Parse an ISO-8601 date ("YYYY-MM-DD") or datetime
// ("YYYY-MM-DD[ T]HH:MM[:SS[.fffffffff]][Z|±HH:MM]") to epoch nanoseconds.
// Returns 0 on any parse failure (matching `ats_vol_data_iso_to_ns`).
[[nodiscard]] std::int64_t iso_to_ns(std::string_view iso) noexcept;

// Year-fraction between two ISO instants on a 365.25-day year (matching
// `ats_vol_data_year_fraction`). Returns NaN if either input fails to parse.
// Delegates to `time_to_expiry_years` with a default `TimeSpec` (Calendar365)
// for the actual arithmetic (vol_time.hpp `kCalendarYearNs`) — this function
// stays the legacy Calendar365-only, ISO-string entry point; production T
// callers that need the opt-in VolTime convention set `QuoteFrame::time` (or
// call `time_to_expiry_years` directly) instead.
[[nodiscard]] double year_fraction(std::string_view from_iso, std::string_view to_iso) noexcept;

// Civil date "YYYY-MM-DD" for an epoch-nanoseconds instant (the inline
// civil-from-days install uses to key the source-input table). Truncates to
// the UTC calendar day.
[[nodiscard]] std::string ns_to_iso_date(std::int64_t ns);

// TRUE expiry instant in UTC epoch-ns for an OSI/listed expiry DATE
// ("YYYY-MM-DD") under settlement session `settle` (default PM = 16:00 ET; AM =
// 09:30 ET). This is the settlement instant a dated-contract loader stamps onto
// `QuoteRow::expiry_ns` so `data_install` derives `Chain::T` from it — replacing
// the legacy `iso_to_ns(expiry_iso)` midnight-UTC parse that under-states front T
// by ~0.8 trading day and hard-drops same-session (0DTE) contracts. ET->UTC via
// `settlement_instant_ns` (modern-DST, vol_time.hpp). Returns 0 on parse failure
// (matching `iso_to_ns`).
[[nodiscard]] std::int64_t
expiry_instant_ns(std::string_view expiry_iso,
                  SettlementSession settle = SettlementSession::Pm) noexcept;

// ── Frame helpers ───────────────────────────────────────────────────────────

// Populate `frame.uid_strs` with the distinct uids across `frame.uid` and the
// row stream, in first-seen order (`snapshot_build_uid_list`). If `frame.uid`
// is empty and rows carry uids, `frame.uid` is set to the first distinct uid.
// @return InvalidArgument if a row resolves to an empty uid or one whose length
//         is >= kDataUidStrCap.
[[nodiscard]] Status build_uid_list(QuoteFrame &frame);

// Collapse the row stream's per-row source-input fields to one cell per
// (uid, expiry_iso) into `frame.expiry_inputs` (the Sprint-25 dedupe). For each
// field, the first finite row value wins and sets the matching completeness
// bit. Overwrites any existing `expiry_inputs`.
void build_expiry_inputs(QuoteFrame &frame);

// Find the source-input cell for a (uid, expiry_iso) key
// (`ats_vol_data_snapshot_find_expiry_inputs`). Returns nullptr when the frame
// carries no inputs or the cell is absent. The pointer is a non-owning borrow,
// valid while `frame.expiry_inputs` is unmodified.
[[nodiscard]] const ExpiryInputs *find_expiry_inputs(const QuoteFrame &frame, std::string_view uid,
                                                     std::string_view expiry_iso) noexcept;

// ── Install ─────────────────────────────────────────────────────────────────

// Install a quote frame into `u` (`ats_vol_data_install`). Interns each row's
// ticker, grows expiries/strikes, writes the quote plane
// (bids/asks/sizes/mids/ts/flags), tracks spot/spot-ts per underlier, sets each
// chain's `T` from `time_to_expiry_years(snapshot_ns, expiry_ns, frame.time)`
// (instants parsed from `frame.snapshot_iso` / `row.expiry_iso`), sorts each
// touched underlier's chains ascending in `T` (re-issuing `expiry_id` to the
// new positions), and stamps `Chain::source_atm_vol` from the frame's
// expiry-input table where present.
//
// The T convention comes from `frame.time` — the frame carries the convention
// it was built under (see `QuoteFrame::time`), so this install can never
// disagree with the loader's own T math and callers need no extra argument.
// A default-constructed `frame.time` (Calendar365) is BIT-IDENTICAL to the
// historical `year_fraction`-derived `T`.
//
// @return the first/default uid on success.
// @return InvalidArgument if the frame has no yield-curve pillars (the C's
//         ATS_VOL_ERR_NO_YIELD_CURVE gate; message names the yield curve), if
//         no default uid can be resolved, or if any row fails validation
//         (non-finite/non-positive strike, negative/non-finite bid or ask,
//         negative sizes, an empty/over-long row uid, or an unparseable
//         expiry).
[[nodiscard]] Result<Uid> data_install(Universe &u, const QuoteFrame &frame);

// ── SpiderRock Parquet loader (DEFERRED) ────────────────────────────────────

// Load spec for the SpiderRock intraday-options Parquet fixture
// (AtsVolSpiderRockLoadSpec). Retained so the deferred entry point has a stable
// signature; a future port fills a `QuoteFrame` from it.
struct SpiderRockLoadSpec {
  std::string parquet_path;         // Apache Parquet fixture/source path
  std::vector<std::string> symbols; // underlier symbols to load
  std::string snapshot_time;        // "HH:MM" or "HH:MM:SS"; matched at 5m granularity
};

// PORT NOTE (deferred): the C `ats_vol_data_load_spiderrock_parquet`
// (ats_vol_spiderrock_parquet.c) is a bespoke, self-contained Thrift/Parquet
// decoder (~1.4k lines of malloc/free/goto) that this port does not reproduce
// — a byte-level re-decode is incompatible with the atx house rules (no
// malloc/new/goto, Rule of Zero), and no `.parquet` fixture is committed to
// atx-vol to test against (the mirrored C tests all `skip` when it is absent).
//
// A future faithful *behavioral* re-port should build on atx-core's Arrow-
// backed reader `atx::core::io::LazyParquet` (select -> filter -> collect),
// which already handles the file/Thrift decode. The SpiderRock schema mapping
// the re-port must reproduce (physical types in parentheses):
//
//   okey_yr/okey_mn/okey_dy (i64)  -> QuoteRow::expiry_iso "%04d-%02d-%02d"
//   okey_xx                 (f64)  -> QuoteRow::strike            (> 0, finite)
//   okey_cp                 (str)  -> QuoteRow::side  ('P'/'p' -> Put else Call)
//   undSecKey_tk            (str)  -> QuoteRow::uid   (symbol filter key)
//   uPrc                    (f64)  -> QuoteRow::under_spot        (> 0, finite)
//   bidPrc/askPrc           (f64)  -> QuoteRow::bid/ask           (>= 0, finite)
//   bidSz/askSz             (i64)  -> QuoteRow::bid_size/ask_size (clamped >= 0)
//   srPrc  (f64, optional)         -> QuoteRow::fv_source         (NaN if absent)
//   srVol                   (f64)  -> QuoteRow::iv_source         (0 if non-finite)
//   prtVolume               (i64)  -> QuoteRow::volume            (clamped >= 0)
//   timestamp               (str)  -> QuoteRow::ts_ns (via iso_to_ns) + snapshot
//   rate/sdiv/ddiv/years    (f64, optional) -> *_source + yield curve build
//   atmVol/ve/de            (f64, optional) -> atm_vol_source/vega_source/delta_source
//
// Row selection: keep rows whose uid is in `symbols` and whose timestamp
// minute, floored to a 5-minute bucket, equals `snapshot_time` floored the same
// way; every kept row must pass the same core-validity gate `data_install`
// applies. Post-pass: build the yield curve from the primary uid's unique
// (years, rate) pairs sorted ascending in years (<= 16 pillars), then
// `build_expiry_inputs` over the resulting rows. Constraints of the committed
// fixture the C decoder assumed: DataPageV2, uncompressed, PLAIN values, RLE
// definition levels, flat optional columns (no dictionary pages).
//
// @return NotImplemented (the C's ATS_VOL_ERR_UNSUPPORTED maps here) until the
//         re-port lands.
[[nodiscard]] Result<QuoteFrame> load_spiderrock_parquet(const SpiderRockLoadSpec &spec);

} // namespace atx::vol
