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
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/curve.hpp" // DividendEvent
#include "atx/vol/data.hpp"  // QuoteFrame
#include "atx/vol/types.hpp" // Result, Side

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

// Load spec for `load_opra_cbbo_parquet`.
struct OpraLoadSpec {
  std::string path;        // parquet file to read
  std::string underlying;  // keep rows whose `underlying` equals this
                           // (empty = keep all, frame uid = first seen)
  std::string snapshot_iso; // "YYYY-MM-DD" or full datetime; stamped on the frame
  double r = 0.0;           // continuously-compounded rate for the spot implication
  double spot_override = 0.0; // if > 0, use this spot instead of implying from PCP
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
};

// Result of loading one OPRA cbbo-1m slice.
struct OpraPanel {
  QuoteFrame frame;         // ready for data_install (flat r, or the spec's term
                            // yield-curve pillars when supplied)
  double implied_spot = 0.0; // spot used (override or PCP-implied)
  std::string snapshot_iso;  // the stamp applied to the frame
  std::size_t n_contracts = 0; // rows kept
  std::size_t n_expiries = 0;  // distinct expiries kept
  std::size_t n_dropped = 0;   // rows skipped (bad symbol / unset px / crossed)
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
//   5. Spot: spec.spot_override if > 0, else imply from the earliest expiry with
//      a co-terminal call/put pair (both mids > 0) via imply_forward_atm_pcp at
//      the curve's rate r(T_front), then implied_spot = F * exp(-r(T_front) *
//      T_front). Unavailable if no pair exists.
//
// @return InvalidArgument on a read failure, an ambiguous multi-symbol input, a
//         term-pillar length mismatch, or a frame-assembly (install
//         precondition) failure; Unavailable when no co-terminal pair exists to
//         imply the spot and no override was supplied.
[[nodiscard]] Result<OpraPanel> load_opra_cbbo_parquet(const OpraLoadSpec& spec);

} // namespace atx::vol
