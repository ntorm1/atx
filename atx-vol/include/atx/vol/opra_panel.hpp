#pragma once

// OPRA cbbo-1m (NBBO) option-chain Parquet loader for atx-vol.
//
// Reads a real Databento OPRA cbbo-1m slice (produced by atx-core
// `pull_opra_cbbo_1m_to_parquet`) into an `atx::vol::QuoteFrame` ready for
// `data_install`. Parses OSI/OCC 21-char option symbols, and — absent an
// explicit override — implies the underlying spot from put-call parity on the
// front expiry's co-terminal call/put mids.
//
// ## Input Parquet schema (single-minute snapshot)
//
//   ts         Timestamp(ns) or Int64(ns) — one snapshot minute; many rows.
//   underlying String  — e.g. "XOM" (absent on the non-consolidated bbo pull).
//   symbol     String  — OSI/OCC 21-char symbol, e.g. "XOM   260619C00110000".
//   bid_px     Int64   — fixed-point 1e-9 dollars; UNSET = INT64_MIN.
//   ask_px     Int64   — fixed-point 1e-9 dollars; UNSET = INT64_MIN.
//   bid_sz     Int64   — size.
//   ask_sz     Int64   — size.
//
// The whole slice is treated as one snapshot (see the loader contract below);
// `spec.snapshot_iso` is the authoritative stamp and the first row's `ts` is
// recorded into `frame.snapshot_ts_ns`.
//
// ## Ownership / thread-safety
//
// `OpraPanel` owns its `QuoteFrame` by value (RAII, Rule of Zero). The loader
// is a pure function: it borrows the parquet file at `spec.path` for the
// duration of the call and returns an owning panel. No shared mutable state.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/vol/data.hpp"  // QuoteFrame
#include "atx/vol/fit_policy.hpp" // FitContext
#include "atx/vol/rates_curve.hpp" // DividendEvent
#include "atx/vol/types.hpp" // Result, Side
#include "atx/vol/vol_time.hpp" // TimeSpec

// Forward-declared to keep this public header Arrow-free: the in-memory-table
// seam (load_opra_cbbo_from_table) takes it by const reference. The full
// definition lives in atx/core/io/parquet.hpp, included by opra_panel.cpp.
namespace atx::core::io {
class ParquetTable;
} // namespace atx::core::io

namespace atx::vol {

// Parsed OSI/OCC option symbol: ROOT + YYMMDD + {C|P} + STRIKE(8 digits,
// price x 1000, zero-padded). The trailing 15 chars are the fixed part; the
// root is the space-trimmed prefix.
struct OsiSymbol {
  std::string root;       // e.g. "XOM" (leading/trailing spaces trimmed)
  std::string expiry_iso; // "YYYY-MM-DD", year = 2000 + YY
  Side side = Side::Call;
  double strike = 0.0; // dollars (8-digit field / 1000)
};

enum class OpraProvenanceMode : std::uint8_t {
  Compatibility = 0,
  Strict = 1,
};

// Convention for turning a bare OSI expiry DATE into an expiry INSTANT.
//
//   MidnightUtc     — the historical behavior: the bare "YYYY-MM-DD" is parsed as
//                     00:00:00Z, so a same-afternoon snapshot sees the front
//                     expiry only a few hours out. DEFAULT (bit-identical to every
//                     existing OPRA load / golden).
//   UsEquityPmClose — U.S. listed equity/ETF options are PM-settled: they expire
//                     at 16:00 America/New_York on the expiry date. When selected,
//                     the loader stamps each expiry ISO with the DST-correct
//                     Eastern close offset (EDT = UTC-4 from the 2nd Sunday of
//                     March to the 1st Sunday of November, else EST = UTC-5), e.g.
//                     "2018-04-27T16:00:00-04:00" -> 2018-04-27T20:00:00Z. This is
//                     the correct time-to-expiry for near-dated slices (a 1-DTE
//                     earnings expiry becomes ~1.0 day out, not ~4 hours). Opt-in
//                     so existing loads keep their exact expiry instants.
//   UsIndexAmOpen   â€” standard cash-index monthlies settle at 09:30 ET. This
//                     requires explicit product policy because historical OPRA
//                     definitions may carry only an expiry date.
enum class ExpiryCloseConvention : std::uint8_t {
  MidnightUtc = 0,
  UsEquityPmClose = 1,
  UsIndexAmOpen = 2,
};

// One date-scoped Databento source identity. Raw OSI text is dictionary-owned
// once per instrument id; observations carry only the aligned numeric id plane.
struct OpraInstrumentIdentity {
  std::uint32_t instrument_id{0};
  std::string raw_symbol{};

  [[nodiscard]] bool operator==(const OpraInstrumentIdentity &) const = default;
};

struct ExternalInputTag {
  std::string source{};
  std::string as_of{};

  [[nodiscard]] bool operator==(const ExternalInputTag &) const = default;
};

enum class DividendTreatment : std::uint8_t {
  EscrowedForward = 0,
};

struct OpraMarketInputProvenance {
  ExternalInputTag spot{};
  ExternalInputTag rates{};
  ExternalInputTag dividends{};
  ExternalInputTag fit_context{};
  DividendTreatment dividend_treatment{DividendTreatment::EscrowedForward};
  std::uint64_t fingerprint{0};

  [[nodiscard]] bool operator==(const OpraMarketInputProvenance &) const = default;
};

// Parse an OSI/OCC option symbol (variable root padding tolerated).
//
// The last 15 chars are the fixed field: YYMMDD (yy -> 2000+yy) + {C|P} +
// 8-digit strike (price x 1000). Everything before it is the root (trimmed).
//
// @param sym the option symbol, length >= 15.
// @return InvalidArgument if `sym` is shorter than 15 chars or the strike is
//         non-positive; ParseError if the date or strike fields are not
//         numeric / out of range.
[[nodiscard]] Result<OsiSymbol> parse_osi_symbol(std::string_view sym);

// Is `root` (an OSI/OCC option root, as produced by `parse_osi_symbol`) the wire
// encoding of the equity ticker `ticker` (the universe / `underlying` spelling)?
//
// THE POLICY, stated once for the whole repo:
//
//   A trailing digit in an OSI root denotes a DIFFERENT DELIVERABLE, therefore a
//   DIFFERENT INSTRUMENT. It is not punctuation and must never be normalised
//   away. The only tolerated difference between an equity ticker and its OSI
//   root is punctuation (`.`).
//
// So `("BRKB", "BRK.B")` is true — the OSI root namespace cannot express `.`, so
// those are two spellings of ONE identity, a pure encoding artifact — while
// `("AAPL1", "AAPL")` is FALSE. After a corporate action an `AAPL1` contract's
// deliverable is not 100 shares of AAPL, so its options are not comparable to the
// vanilla chain at the same strike and the fitter has no deliverable model with
// which to tell them apart. Merging them is a silent mispricing, not a lost
// symbol.
//
// THIS IS DELIBERATELY NARROWER THAN THE PYTHON PRODUCERS, AND THAT ASYMMETRY IS
// A SAFETY PROPERTY, NOT DRIFT. `tools/pull_opra_hive.py` strips a trailing digit
// from the root before mapping it to a universe symbol, so an adjusted `AAPL1`
// row can reach disk carrying `underlying = "AAPL"`. This predicate refuses that
// pair, which is exactly what makes `load_opra_cbbo_parquet`'s per-row identity
// guard fail LOUD on it instead of silently merging an adjusted deliverable into
// the vanilla chain. Do NOT widen this to match the producers; if the producers
// are to change, that is its own decision with its own argument.
//
// ASYMMETRY, stated because it is sharp: dots are skipped in `ticker` only, never
// in `root`. `("BRK.B", "BRK.B")` is therefore FALSE — this is not reflexive over
// dotted strings. That is sound because the premise of the rule is that the root
// namespace cannot express punctuation, so a dotted root is not a thing OPRA
// emits; and it is the behaviour of both copies this function replaces, preserved
// exactly rather than quietly widened. It is pinned by a test.
//
// A PREDICATE, deliberately not a normaliser. There is no `osi_root_of(ticker)`
// counterpart and there must not be: a normaliser invents a third namespace that
// callers can key by, and it would allocate on a path that runs once per kept
// quote row (millions per hive load). This is allocation-free.
[[nodiscard]] bool osi_root_matches_ticker(std::string_view root, std::string_view ticker) noexcept;

// Load spec for `load_opra_cbbo_parquet`.
struct OpraLoadSpec {
  std::string path;                     // parquet file to read
  std::string underlying;               // keep rows whose `underlying` equals this
                                        // (empty = keep all, frame uid = first seen).
                                        // ALSO the loaded frame's identity: on a filtered load
                                        // this exact string becomes `frame.uid` AND every
                                        // `QuoteRow::uid` (see the uid-namespace note on
                                        // load_opra_cbbo_parquet step 4).
  std::string snapshot_iso;             // "YYYY-MM-DD" or full datetime; stamped on the frame
  double r = 0.0;                       // continuously-compounded rate for the spot implication
  double spot_override = 0.0;           // if > 0, use this spot instead of implying from PCP
  std::vector<DividendEvent> cash_divs; // optional discrete divs, carried on the frame

  // Optional term-structure yield curve (P2-3). When non-empty, these
  // continuously-compounded zero-rate pillars replace the flat scalar `r`:
  // `yc_pillar_t` holds year-fractions (strictly ascending), `yc_pillar_r` the
  // matching zero rates; the two must be equal length. Two or more pillars
  // build a Fritsch-Carlson monotone-Hermite `YieldCurve`, and each maturity T
  // (the per-front-expiry PCP forward back-out, and each expiry's source rate)
  // is queried at its OWN interpolated `r(T)`. Empty (the default) OR a single
  // pillar reduces to a FLAT rate — the historical `{T=1, r}` behavior,
  // bit-identical — because a 1-pillar curve does not interpolate flat.
  std::vector<double> yc_pillar_t; // pillar year-fractions (empty => flat r)
  std::vector<double> yc_pillar_r; // pillar zero rates (same length as _t)
  OpraProvenanceMode provenance_mode{OpraProvenanceMode::Compatibility};
  // Expiry-instant convention (see ExpiryCloseConvention). Default MidnightUtc
  // is bit-identical to every historical OPRA load; UsEquityPmClose stamps the
  // DST-correct 16:00-ET close onto each expiry so near-dated slices carry their
  // true (PM-settled) time-to-expiry.
  ExpiryCloseConvention expiry_close{ExpiryCloseConvention::MidnightUtc};
  // American preserves the historical equity/ETF path. Cash-index loaders
  // explicitly select European; do not infer it from an absent CFI value.
  ExerciseStyle exercise_style{ExerciseStyle::American};
  FitContext fit_context{};
  OpraMarketInputProvenance market_input_provenance{};
  // T convention governing every year-fraction this loader computes: the PCP
  // spot-implication forward T, the 0DTE/expired-contract drop filter, and
  // (when yc_pillar_t/_r are supplied) the per-row rate_source term-curve
  // query T. Default Calendar365 is BIT-IDENTICAL to the historical
  // `year_fraction`-derived behavior (see vol_time.hpp). The loader stamps this
  // onto the returned frame (`QuoteFrame::time`), which `data_install` reads
  // for `Chain::T` — so the plain `data_install(u, panel.frame)` call every
  // production consumer already makes yields chains under THIS convention
  // automatically; there is nothing to thread and nothing to get wrong. A
  // session-building caller must additionally set `SessionInputs::time` (the
  // session's retained copy); `VolaSession::from_frame` fails loudly
  // (InvalidArgument) if it mismatches the frame's.
  TimeSpec time{};
};

// Result of loading one OPRA cbbo-1m slice.
struct OpraPanel {
  QuoteFrame frame;                       // ready for data_install (flat r, or the spec's term
                                          // yield-curve pillars when supplied)
  double implied_spot = 0.0;              // spot used (override or PCP-implied)
  std::string snapshot_iso;               // the stamp applied to the frame
  std::size_t n_contracts = 0;            // rows kept
  std::size_t n_expiries = 0;             // distinct expiries kept
  std::size_t n_dropped = 0;              // rows skipped (bad symbol / unset px / crossed)
  std::uint32_t source_schema_version{1}; // 1=legacy, 2=instrument_id
  // Content hash of the source rows/identities only; intentionally
  // TimeSpec-independent (see opra_panel.cpp's `source_fingerprint`) -- two
  // panels loaded from the same rows under different T conventions share it.
  std::uint64_t source_fingerprint{0};
  bool provenance_complete{false};
  // Column-presence metadata. A numeric zero is a valid observed count and
  // must not be conflated with a source that did not carry the field.
  bool bid_size_available{false};
  bool ask_size_available{false};
  bool volume_available{false};
  bool open_interest_available{false};
  std::vector<std::uint32_t> source_instrument_ids;      // aligned with frame.rows
  std::vector<OpraInstrumentIdentity> source_identities; // id ascending
  FitContext fit_context{};
  OpraMarketInputProvenance market_input_provenance{};
  // Convenience mirror of `OpraLoadSpec::time` (== `frame.time`, the
  // authoritative copy `data_install` reads). Read it when building a session
  // off this panel: set `SessionInputs::time` from it so `VolaSession::
  // from_frame`'s mixed-convention guard passes. `data_install(u, frame)`
  // itself needs nothing from here — the frame already carries the convention.
  TimeSpec time{};
};

// Load an OPRA cbbo-1m Parquet slice into a QuoteFrame.
//
// Behavior:
//   1. read_parquet(spec.path); InvalidArgument (with the reader's message) on
//      failure.
//   2. Per row: skip when the `underlying` filter is set and mismatched; drop
//      (counted) when bid_px/ask_px is the INT64_MIN sentinel or null, when the
//      OSI symbol fails to parse, or when the quote is crossed (bid > ask). A
//      zero bid or ask is kept (unquotable legs are dropped downstream).
//   3. Multi-symbol guard (P2-2): reject ambiguous input up front — an empty
//      `underlying` over a parquet carrying more than one distinct `underlying`,
//      an `underlying` filter over a parquet with no `underlying` column, or a
//      filter that matches zero rows (all InvalidArgument).
//   4. Assemble the frame: uid = the underlying (or first root), snapshot_iso,
//      snapshot_ts_ns from the first `ts`, the yield-curve pillars (the spec's
//      term pillars when supplied, else the flat {T=1, r=spec.r}), the kept
//      rows, then build_uid_list + build_expiry_inputs. With supplied pillars,
//      each kept row's source rate is stamped with the curve rate at its expiry.
//
//      UID NAMESPACE (one rule, both derivations): a row's uid and the frame's
//      uid both come from the `underlying` COLUMN — the OSI root is used only as
//      a fallback when the file carries no such column. The dotted universe
//      spelling is therefore canonical end to end (`--symbols` -> hive discovery
//      -> `frame.uid` -> `QuoteRow::uid` -> `Universe::intern_ticker` ->
//      `CorpusBoard::symbol` -> the manifest/archive's `canonical_symbol`, which
//      preserves dots). Deriving the two from different sources is what made a
//      punctuated ticker (`underlying = "BRK.B"`, OSI root `BRKB`) intern as TWO
//      underliers and lose all of its quotes.
//
//      That rule is only sound while the column really does name the same
//      underlier the row's OSI symbol does, so it is CHECKED per row, not
//      assumed: a row whose `underlying` and OSI root differ by anything other
//      than punctuation (dots, which the OSI namespace cannot express) is a hard
//      InvalidArgument — two underliers, and merging them would be a silent
//      pricing error rather than a lost symbol. The live case is the
//      adjusted-deliverable class (`AAPL1` rows whose column says `AAPL`).
//   5. Spot: spec.spot_override if > 0, else imply from the earliest expiry with
//      a co-terminal call/put pair (both mids > 0) via imply_forward_atm_pcp at
//      the curve's rate r(T_front), then implied_spot = F * exp(-r(T_front) *
//      T_front). Unavailable if no pair exists.
//
// @return InvalidArgument on a read failure, an ambiguous multi-symbol input, a
//         term-pillar length mismatch, or a frame-assembly (install
//         precondition) failure; Unavailable when no co-terminal pair exists to
//         imply the spot and no override was supplied.
[[nodiscard]] Result<OpraPanel> load_opra_cbbo_parquet(const OpraLoadSpec &spec);

// Load an OPRA cbbo-1m slice from an ALREADY-MATERIALIZED in-memory table.
//
// This is the table-driven seam under `load_opra_cbbo_parquet`: both delegate
// to the identical panel-construction core, so a `table` holding the same rows
// the file at `spec.path` would decode yields a BYTE-IDENTICAL `OpraPanel`
// (rows, implied spot, snapshot stamp, source fingerprint, provenance). Use it
// to feed an in-memory OPRA table — e.g. one date-partition file's rows split
// by `underlying` — through the same validated path without re-reading parquet.
// No file is read here: `spec.path` is consulted ONLY for error-message context.
//
// Behavior: first validates that `table` carries the 8 canonical OPRA columns
// (`ts`, `underlying`, `symbol`, `instrument_id`, `bid_px`, `ask_px`, `bid_sz`,
// `ask_sz` — the hive-v2 schema), then delegates to the shared core. Per-row
// filtering, put-call-parity spot implication, term-curve rate stamping, and
// frame assembly are exactly as documented for `load_opra_cbbo_parquet`.
//
// @return InvalidArgument if any required column is absent (the message names
//         the missing column and `spec.path`), on an ambiguous multi-symbol
//         input, a term-pillar length mismatch, or a frame-assembly failure;
//         Unavailable when no co-terminal pair implies the spot and no
//         `spec.spot_override` was supplied.
//
// Thread-safety: a pure function over borrowed inputs. It mutates no shared
// state and returns an owning panel; concurrent calls on distinct arguments are
// safe, and `table` is only read.
[[nodiscard]] Result<OpraPanel> load_opra_cbbo_from_table(const atx::core::io::ParquetTable &table,
                                                          const OpraLoadSpec &spec);

// ── P-01: one column scan + one row index per TABLE, not per underlying ──────
//
// A hive v2 date file is ONE table holding EVERY underlying's rows, and the seam
// above builds ONE underlying's panel out of it. Called per symbol, that costs
// O(rows of the TABLE) each time — every column is re-materialized (`strings()`
// and `null_mask()` each build a fresh n_rows-long vector) and every row of every
// other underlying is re-visited and skipped. On a 102-name production date that
// is 102 passes over ~208k rows, and it is why `load` scaled super-linearly in
// symbol count (0.49 s at 10 names, 4.46 s at 102).
//
// The memory shape was worse than the time. Every per-symbol scratch buffer was
// sized for the WHOLE table — `rows.reserve(n_rows)` on a ~224-byte QuoteRow is
// a ~47 MB reservation for a symbol that owns ~2k rows — and the over-reserved
// vector is then MOVED into the returned panel, so it stays live for the rest of
// the build. 102 names => ~4.8 GB of COMMITTED (untouched, so never resident)
// address space, which is exactly the intermittent `std::bad_alloc` that killed
// 102-symbol builds while their working set was 123 MB and the box had GB free:
// the process hit the system COMMIT limit, not its own RSS.
//
// `OpraTableScan` moves that work to once per table. It materializes each column
// once and builds a per-`underlying` row index, so a split visits only its own
// rows and sizes every buffer for them. Time becomes O(table) + O(rows of the
// symbol); the panel costs its own rows and nothing more.
//
// LIFETIME: the string/column views BORROW `table`. A scan must not outlive the
// table it was built from, and the table must not be mutated meanwhile (it
// cannot be — `ParquetTable` is immutable once read).
//
// Thread-safety: building a scan touches only its own storage and reads `table`;
// scans of DISTINCT tables are independent, and a built scan is immutable, so
// any number of threads may split off one concurrently.
struct OpraTableScan {
  std::size_t n_rows{0};
  std::int64_t first_ts_ns{0}; // the table's first `ts`, in epoch ns (0 if absent)

  // Column views. `symbols`/`underlyings` borrow the table's string buffers; the
  // int64 spans alias its numeric buffers; the null masks are owned copies (the
  // reader has no aliasing accessor for validity).
  std::vector<std::string_view> symbols;
  std::vector<std::string_view> underlyings; // empty when `has_underlying` is false
  std::span<const std::int64_t> bid_px, ask_px, bid_sz, ask_sz;
  std::span<const std::int64_t> instrument_ids; // empty when !has_instrument_id
  std::vector<std::uint8_t> bid_null, ask_null;
  bool has_underlying{false};
  bool has_instrument_id{false};

  // Per-`underlying` row index, present iff `has_underlying` (and the table fits
  // a 32-bit row id). `rows` holds every row id grouped by underlying and
  // ASCENDING within each group, so a split visits its rows in the same order a
  // full-table scan would — which is what makes an indexed panel byte-identical
  // to an unindexed one.
  bool indexed{false};
  std::unordered_map<std::string_view, std::uint32_t> slot_of; // underlying -> group
  std::vector<std::uint32_t> offsets;                          // slot_of.size() + 1
  std::vector<std::uint32_t> rows;

  [[nodiscard]] std::span<const std::uint32_t>
  rows_for(std::string_view underlying) const noexcept {
    if (!indexed) {
      return {};
    }
    const auto it = slot_of.find(underlying);
    if (it == slot_of.end()) {
      return {};
    }
    const std::uint32_t lo = offsets[it->second];
    const std::uint32_t hi = offsets[it->second + 1u];
    return std::span<const std::uint32_t>{rows.data() + lo, static_cast<std::size_t>(hi - lo)};
  }
};

// Scan an already-materialized hive-v2 OPRA table once: validate the 8 canonical
// columns, materialize them, and index the rows by `underlying`.
//
// `path_for_errors` is used ONLY for message context and matches
// `load_opra_cbbo_from_table`'s `spec.path` role byte-for-byte, so a caller that
// scans once per date and reports the scan's error on every cell of that date
// produces exactly the errors the per-symbol seam would have.
//
// @return InvalidArgument if a required column is absent or is not of the
//         expected type.
[[nodiscard]] Result<OpraTableScan> scan_opra_cbbo_table(const atx::core::io::ParquetTable &table,
                                                         std::string_view path_for_errors);

// Build ONE underlying's panel from a scan. Byte-identical to
// `load_opra_cbbo_from_table(table, spec)` over the same table and spec — the
// index only changes WHICH rows are visited, never their order or contents — but
// costs O(rows of `spec.underlying`) rather than O(rows of the table).
//
// `spec.underlying` must be set: an empty filter has no group to look up and is
// rejected as InvalidArgument (use the table seam for a whole-table load). A
// symbol the table does not carry returns the seam's exact zero-match Err,
// `"underlying '<sym>' not found in parquet"`, so a coverage-hole classifier
// keyed on that message is unaffected.
[[nodiscard]] Result<OpraPanel> load_opra_cbbo_from_scan(const OpraTableScan &scan,
                                                         const OpraLoadSpec &spec);

} // namespace atx::vol
