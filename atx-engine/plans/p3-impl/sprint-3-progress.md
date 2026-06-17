# p3 S3 — Single-File ORATS History Loader — Sprint Ledger

**Opened:** 2026-06-16 (worktree `atx-wt/p3-s3`, branch `feat/p3-s3-orats-loader`, base `main`).
**Spec:** [`../../../docs/superpowers/specs/2026-06-16-orats-history-loader-design.md`](../../../../docs/superpowers/specs/2026-06-16-orats-history-loader-design.md)
**Plan:** [`../../../docs/superpowers/plans/2026-06-16-orats-history-loader.md`](../../../../docs/superpowers/plans/2026-06-16-orats-history-loader.md)

**Goal:** Pure high-performance C++ loader that streams `tbltickerhistory3_10y.zip` into an on-disk atx-tsdb per-date `.seg` partition keyed on `securityID`, wired into the atx-engine data interface (new multi-segment Panel attach), with corporate actions applied via `cumulReturnFactor`, proven by a full E2E smoke of the unchanged p2 pipeline on the US equity universe. S3 feeds the still-unopened S2 benchmark.

## Locked decisions (kickoff)

1. **Canonical instrument key = `securityID`** (stringified) — stable across renames (AA→HWM) / delistings; survivorship-correct. `ticker_tk`/`todayTicker` → side-car symbology manifest, not segment fields.
2. **Corporate-action model:** canonical adjusted `close` = `close × cumulReturnFactor` (`close` = col 9, as-traded contemporaneous; `closeUnadjPr` is the *lagged prior-day close*, not the adjustment input). Implemented by reusing `adjust_total_return(close, cumulReturnFactor, 0)`. Verified: AAPL 2012-03-26 `606.98 × 0.0299354 = 18.16` ≈ Yahoo present-basis adj close.
3. **Placement:** new sprint **S3**, feeds S2. No new alpha-discovery capability (p3 anti-roadmap).
4. **Panel rail:** per-date `.seg` partition (the atx-tsdb hive format) + new `attach_multi_segment_panel` (the interface expansion). Multi-segment attach **materializes** an owned union Panel (per-date segments have independent instrument axes); zero-copy borrow deferred to backlog.

**Date floor:** ingest only `tradingDate ≥ 2020-01-01` (user constraint).
**Curated 16-field set:** `open, high, low, close, closePr, closeUnadjPr, volume, shares, returnFactor, totalReturn, cumulReturnFactor, gics, earnFlag, atmCenI_21d, atmCenI_126d, nEarnCnt_5d`. Forward-looking cols (`hEMove, iEMove, shD1, lnD1, wkD1, qtrD1, *EMove`) excluded (leak risk).

## Recon (as-built facts the plan builds on)

- **Source file:** `~/Downloads/tbltickerhistory3_10y.zip` → single entry `tbltickerhistory3_10y.txt` (11,084,562,320 bytes, TSV, 71 columns, date-major sorted). ORATS SMV `tbltickerhistory` export (OHLCV + shares + GICS + total-return factors + an implied/historical-vol term-structure surface).
- **miniz** vendored at `atx-core/third-party/miniz/` (static lib `atx_miniz`, PRIVATE to atx-core); streaming via `mz_zip_reader_extract_iter_new/_read/_free`. Precedent: `atx-core/src/external/databento.cpp` (uses `extract_to_heap` — we stream instead).
- **Segment format** = atx-tsdb v2 (`atx-tsdb/include/atx/tsdb/segment.hpp`): dense T×N×F f64 grid + present-bitmap, sealed w/ CRC. `build_from_long(LongColumns, path, created_at_nanos)` pivots → one sealed `.seg`.
- **Seam being extended:** `alpha::attach_segment_panel` maps ONE segment; S3-4 adds `attach_multi_segment_panel` over a per-date hive.
- **Orchestrator to mirror:** `data::build_real_panel` (`atx-engine/src/data/real_panel.cpp:349-418`).
- **Build:** `pwsh -File scripts/dev-build.ps1 -Configure -Build -Test -TestRegex <regex>` (imports VS dev env, `ninja` preset).

## Unit status

| # | Item | Effort | Status | Commits | Notes |
|---|------|--------|--------|---------|-------|
| S3-0 | Marker — worktree, ledger, roadmap, seed spec/plan | Light | ✅ done | cf53140 | spec + plan + ledger + ROADMAP S3 section |
| S3-1 | atx-core `io::ZipEntryReader` — streaming single-entry inflate | Heavy | ✅ done (reviewed) | 40d64e4, 7c35dc1 | miniz pImpl (PRIVATE to atx-core); 2/2 + suite green |
| S3-2 | `load_orats_history` — zip TSV → per-date `.seg` + side-cars | Heavy | ✅ done (reviewed) | b578238, 63267ef | `process_line` extraction, date-guard-before-floor, parse_f64 strictness, manifest errors; securityID key |
| S3-3 | `orats_total_return_close` — TRI via `adjust_total_return` reuse | Moderate | ✅ done (reviewed) | fc3edc3 | AAPL split spot-check 18.17 ≈ Yahoo |
| S3-4 | `attach_multi_segment_panel` — union per-date segments → owned Panel | Heavy | ✅ done (reviewed) | ec452db, 375ef63 | owned union Panel; directory_iterator ec check + error-path tests |
| S3-5 | `build_history_panel` orchestrator + shared `digest_panel` | Heavy | ✅ done (reviewed) | ae2b394, 24aca6f, 5a9bb06, b39d47c | digest extraction byte-identical (golden `0x2a22a873483d9157`); cumReturnFactor 15-char rename + consteval guard; review fixes (dead-init, explicit fields) |
| S3-6 | Guarded E2E smoke through unchanged `RobustResearchDriver` | Heavy | ✅ done (reviewed) | 26a5f36, 45a1da9 | guarded real-partition smoke; date_to_nanos helper + GTEST_LOG |
| S3-final | Final whole-branch review fix wave | — | ✅ done (re-reviewed, Approved) | c9f7479, d39230c | synthetic always-run E2E (driver runs over history Panel, digest `0x884dac53dc47fe01`), operator caller (`ATX_ORATS_ZIP`), deterministic symbology sort, resolve_header decouple, comment |
| S3-close | Docs, `sprint3.md`, ledger, `--no-ff` merge | Light | 🔄 in progress | — | data-ingestion §7, sprint3.md, this table |

**Final suite:** 2548/2548 passed, 0 failed (engine), output pristine. New S3 tests:
DataOratsHistory (×5), DataHistoryPanel, DataHistoryTri (×3), OratsE2ESmoke (×3:
Synthetic always-runs, RealPartition + OperatorOratsZip skip in CI). Golden real-panel
digest `0x2a22a873483d9157` unchanged by the shared-`digest_panel` extraction.

**Reviews:** every unit task-reviewed (Approved after fixes); broad final whole-branch
review (Opus) returned "merge with fixes" — 0 Critical, 2 Important (CI never ran the
driver over a history Panel; no committed loader caller) + 3 Minor, all closed by the
S3-final fix wave and re-reviewed **Approved — ready to merge**.

**Operator data-build:** DEFERRED (user choice at close). The headline full-US-universe
E2E is the manual 11 GB build — see `data-ingestion-reference.md` §7.6 for the one-command
`OratsE2ESmoke.OperatorOratsZip` invocation. The code path is proven by the synthetic
always-run integration test in the interim.

## Backlog residuals → ROADMAP

- Borrowed/zero-copy multi-segment attach via a global build-time instrument axis (perf lever; S3-4 materializes for correctness).
- Parse-parallelism with chunk-boundary line stitching (S3-2 single parse thread first per the spec).
- IV-surface alpha operators consuming `atmCenI_*`/`nEarnCnt_5d` (p4 — p3 ingests, adds no new search surface).
- `closePr` / `closeUnadjPr` column-semantics verification before S2 screens on them.
- GICS single-digit encoding → `sector_code` mapping table.
- Operator full-US-universe 11 GB data-build (deferred manual step; §7.6).
- `smoke_tmpdir()` shared between the two guarded operator tests (cosmetic; both skip in CI) — give each its own temp dir if a second guarded test is added.
