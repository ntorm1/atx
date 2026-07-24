#pragma once

// Date-partitioned OPRA "hive v2" loader — the production centerpiece that turns
// a `date=<YYYY-MM-DD>/data.parquet` directory tree into an `OpraBatchResult`.
//
// Where `load_opra_daterange` (opra_batch.hpp) reads the OLD per-symbol layout
// (`<root>/<symbol>/<date>.parquet`, one file per (symbol, date)), this loader
// reads the NEW layout: one Parquet file per DATE holding every underlying's
// rows (the 8-column OPRA schema — ts, underlying, symbol, instrument_id,
// bid_px, ask_px, bid_sz, ask_sz). It enumerates every calendar date in an
// inclusive [date_lo, date_hi] range, reads each present date file ONCE, and
// splits it per underlying through the in-memory-table seam
// `load_opra_cbbo_from_table` (opra_panel.hpp) — so a v1 tree and a v2 hive
// built from the SAME rows yield cell-for-cell equal panels.
//
// ## Symbol selection
//
//   * `spec.symbols` non-empty  — load exactly those underliers, in the given
//     order, for every date (a date whose file lacks a requested symbol yields
//     an Err entry for it, exactly as the table seam reports a zero-match filter).
//   * `spec.symbols` empty      — DISCOVER: the effective symbol list is the
//     sorted distinct UNION of `underlying` across every present, readable date,
//     resolved in a SERIAL pre-pass (one materialized read per date, cached and
//     reused by the panel pass — never re-read). EVERY date then spans the FULL
//     union, so the entry grid is rectangular and globally deterministic
//     (date-major × union), independent of which date carries which symbol. A
//     symbol in the union but ABSENT from a given date's file is a VISIBLE
//     coverage hole: it is fed through the table seam exactly like an explicit
//     request and returns a zero-match `Err` (bumping `n_error`), never a silent
//     gap. If NO date in range is readable the union is empty and each date
//     contributes a single anonymous entry (see below).
//
// ## Missing / corrupt dates (non-fatal)
//
//   * A calendar date with no `date=<d>/data.parquet` is NON-fatal: it yields one
//     `Err(NotFound)` per EFFECTIVE symbol (the requested list, or the discovered
//     union), bumping `n_missing`. Only in the degenerate discovery case where the
//     union is empty does a missing date contribute a single anonymous
//     `Err(NotFound)` (`symbol == ""`).
//   * A date file that EXISTS but fails to read/parse (corrupt/truncated) is NOT
//     missing: each of that date's cells gets the loader's `Err(...)` and bumps
//     `n_error`. The whole batch is still `Ok`.
//
// The counters partition `entries`: n_loaded + n_missing + n_error == n_total.
// The ONLY top-level `Err`s are a MALFORMED spec: an empty `root_dir`, an
// unparseable `date_lo`/`date_hi`, a `date_hi` before `date_lo`, or a
// `yc_pillar_t`/`_r` length mismatch (all `InvalidArgument`). An empty
// `symbols` is NOT malformed — it selects discovery.
//
// ## Ownership / thread-safety
//
// A pure function over by-value RAII aggregates (Rule of Zero). It borrows the
// files under `spec.root_dir` for the call and returns an owning result. Per-DATE
// work fans out over `spec.n_threads` workers (W4.3): each date owns a
// pre-sized, disjoint slot range and writes only its own slots after pure reads
// (a fresh Arrow read per date, or the reused discovery read; the table seam
// holds no shared mutable state), so the result — entry order, per-panel row
// counts, and every counter — is IDENTICAL for any thread count (0 = auto,
// 1 = serial). The optional progress callback fires synchronously on the calling
// thread in a serial post-join pass (monotonic `done`), one in-order call per
// cell.

#include <string>
#include <vector>

#include "atx/vol/opra_batch.hpp" // OpraBatchResult, OpraBatchProgress, CorpusMarketInputTable,
                                  // MissingMarketInputPolicy
#include "atx/vol/opra_panel.hpp" // OpraProvenanceMode
#include "atx/vol/types.hpp"      // Result

namespace atx::vol {

// Spec for `load_opra_hive`. Field names/defaults are contractual (downstream
// callers construct this directly).
struct OpraHiveSpec {
  std::string root_dir;         // base dir holding date=<YYYY-MM-DD>/data.parquet
  std::string date_lo;          // inclusive "YYYY-MM-DD" lower bound
  std::string date_hi;          // inclusive "YYYY-MM-DD" upper bound
  std::vector<std::string> symbols; // empty = discover every underlying per date

  // Per-date snapshot stamp = `date + snapshot_suffix`, passed verbatim as
  // `OpraLoadSpec.snapshot_iso`. Default = the 19:55Z pre-close minute.
  std::string snapshot_suffix{"T19:55:00Z"};

  double r{0.0}; // fallback flat continuously-compounded rate

  // Optional term-structure pillars applied to every cell absent a per-cell
  // market-input override (two or more strictly-ascending year-fractions build a
  // YieldCurve; empty or one pillar => flat `r`). The two arrays must be equal
  // length (a mismatch is a malformed-spec top-level Err).
  std::vector<double> yc_pillar_t, yc_pillar_r;

  CorpusMarketInputTable market_inputs{};
  MissingMarketInputPolicy missing_market_inputs{MissingMarketInputPolicy::UseFallback};
  OpraProvenanceMode provenance_mode{OpraProvenanceMode::Compatibility};

  // Per-date load fan-out (W4.3). 0 = auto (hardware concurrency), 1 = serial.
  // A PERF-ONLY knob: the result is byte-identical for any value.
  unsigned n_threads{0};
};

// Load every (date, symbol) cell of a date-partitioned OPRA hive v2 tree.
//
// @param spec     the hive spec (see field docs above).
// @param progress optional sink, fired once per cell in a serial post-join pass
//                 with a monotonic `done` (1..n_total).
// @return the full `OpraBatchResult` (Ok even when some/all dates are missing or
//         some files are corrupt) unless the spec is malformed: `InvalidArgument`
//         for an empty `root_dir`, an unparseable `date_lo`/`date_hi`, a `date_hi`
//         before `date_lo`, or a `yc_pillar_t`/`_r` length mismatch; `Unavailable`
//         when `missing_market_inputs == Error` and a required cell is absent.
//
// Entries are ordered date-major then symbol-major. Thread-safety: see the header
// contract — pure, and byte-identical for any `spec.n_threads`.
[[nodiscard]] Result<OpraBatchResult> load_opra_hive(const OpraHiveSpec& spec,
                                                     const OpraBatchProgress& progress = {});

} // namespace atx::vol
