#pragma once

#include <string_view>

#include "stages.hpp"

namespace atx::impl {

// Checkpoint 15: point-in-time equity universe over the 2013-2019 archive segments.
// Frozen design: atx-engine/reviews/2026-09-20-iteration15-point-in-time-universe-design.md
// (§2 data contract, §4 outputs, §5 subcommand, §6 pre-registration, §14/§15 rulings).
//
// The stage streams one sealed `.seg` per session into data::PitUniverseBuilder
// (§3), ranks on the §3.5 median of trailing-63-session dollar volume, and emits
// membership for top_n {1000, 2000, 3000} x band {0.00, 0.10} side by side, with
// churn, per-year coverage and union sizes, an inferred delisting table and a
// survivorship lower bound (§4). It fits nothing, forecasts nothing and claims no
// alpha (§1.3): the output is membership lists and counts.
//
// W1-D5: default --universe-rule common-stock-v2 requires --instrument-types,
// a sealed atx-instrument-types-v1 JSON projection with original source hashes,
// verified publication/vintage clocks, and validity endpoints known under those
// clocks. Availability is strictly before the rank session; unknown/unverified/
// conflicting type cannot trade. Inclusive raw price $5 / median ADV $5m floors,
// excluded_instruments.csv, copied type projection, and ATXPITU2 membership are
// bound in an atx-equity-universe-v2 manifest. Explicit legacy-v1 retains cp15.
//
// The ledger line it appends (§5.6) has purpose "point-in-time-universe-construction"
// and trial_count_declared 0 — accepted only because that purpose is on the
// trial_ledger.hpp non-trial allow-list (§5.7, R15-3).

// §5.8 / ruling R15-16: the SHA-256 of the FROZEN design note, EMBEDDED as a constant
// and never read at runtime (the same mechanism as stage_equity_ic.cpp:49-59, ruling
// I-2). The definitions live in stage_equity_universe.cpp because the runner
// (`build-equity/audits/iteration15_run_equity_universe.py`) scans THAT file for exactly
// one `kEquityUniverseDesignNoteSha256 = "<64 hex>"` and refuses to launch unless the
// on-disk note hashes to the same value. The digest is written into request.json,
// manifest.json and the ledger's `design-note` parent. A later edit to the note is a
// NEW freeze: re-embed, re-review, new ledger line.
extern const std::string_view kEquityUniverseDesignNoteRelativePath;
extern const std::string_view kEquityUniverseDesignNoteSha256;

// §5. Validates the frozen flag allow-list (§5.2), enumerates and seal-checks every
// segment (§5.4), binds each `.seg` to its `_ingestion.manifest.json` digest and each
// preparation manifest to the ingestion binding (§5.5 step 2), selects the monthly rank
// sessions (DR15-8), preflights the memory budget (§7), reserves a fresh `--out`
// (§5.3), appends the pre-registration ledger line (§5.6) and only then streams the
// segments. Every exit after the pre-registration line — error or exception — appends
// the terminal `completed` / `failed` line (§5.5 step 4, cp14 ruling C-1 shape).
[[nodiscard]] atx::core::Result<StageResult> run_equity_universe(const RunConfig &config);

} // namespace atx::impl
