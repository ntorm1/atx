# Sprint PF2-S6 — Calendarization + fiscal normalization + quarterly-TTM

**Goal:** give the fundamentals spine a real FYE→calendar mapping (Compustat FYR rule), period-length / 52-53-week flags, a calendar-aligned LTM/TTM surface, and — on the ~1yr proof slice with richer duration coverage — unblock the quarterly-TTM stitch that pf1 feasibility-checked and deprioritized. Reserved migrations 0114–0116.

**Mandate / Owns:** NEW `db/calendarization.py` (the FYR rule, period-length/53-week flags, the three period-identification schemes, calendar-aligned TTM builder); a TTM region extension inside `db/fundamental_statements.py` (`refresh_fundamental_ttm_points` gains the calendar-aligned + proof-slice stitch path); NEW `db/tests/test_calendarization.py`.

**Must NOT touch:** the ratio/formula engine (`fundamental_ratios.py`, `formula_registry`), PF2-S3's `db/standardization.py` / `fundamental_standardized`, PF2-S5's `db/industry_templates.py` / the `fundamental_statements.py` statement-map region. This sprint reads the standardized/period surfaces and the `derived_ytd_quarter_points` machinery; it never rewrites what they produce. It does not renumber or edit any migration ≤ 0113.

**Depends on:** PF2-S3's standardized surface (`fundamental_standardized`) as the comparable input; pf1's four-date `fundamental_periods` model (`datadate` / `rdq` / `pdate` / `fdate` / `ldate`, migration S4c) as the period spine; the existing `refresh_fundamental_ttm_points` YTD-difference machinery as the reference algorithm. Runs **sequentially AFTER PF2-S5** — both touch `fundamental_statements.py` (S5 the statement-map region, S6 the TTM region), never concurrently in the same tree.

---

## Baseline / where the cycles go

Companies close their books on different fiscal-year-ends, so you cannot sum or compare two issuers' periods without normalization — and pf1 never built that normalization. Measured this session against the live warehouse and `refresh_fundamental_periods`:

1. **`fundamental_periods` derives its calendar labels naively — scheme (2) only, no FYR rule.** `calendar_year` / `calendar_quarter` / `calendar_period` are computed as `EXTRACT(YEAR FROM period_end)` / `EXTRACT(QUARTER FROM period_end)` — i.e. "the calendar year/quarter *containing* the FYE." There is **no FYR mapping** (Compustat `FYR` = the last calendar *month* of the fiscal year), **no fiscal-year+quarter scheme (1)**, and **no greatest-overlap scheme (3)**. A June-FYE filer's FY is silently mislabelled: the mapping rule is that when the FYE month is June–December (`6 ≤ FYR ≤ 12`) the *containing* calendar year labels the fiscal year, but when it is January–May (`1 ≤ FYR ≤ 5`) the **prior** calendar year is used (the calendar year with greatest overlap). None of that logic exists.
2. **No period-length / 52-53-week flags.** `period_days` exists (`date_diff('day', period_start, period_end) + 1`) and `normalized_period_type` buckets it (`quarter` 70–120d, `semiannual_ytd` 121–220d, `multi_quarter_ytd` 221–329d, `annual` 330–380d). But there is **no `is_53_week` flag and no `week_count`**. 52/53-week retailers run a 4-4-5 calendar ending on a fixed weekday (52 wks = 364 days); a 53rd week is inserted ~every 5–6 years to re-sync. Vendors flag and adjust these so a "53-week quarter" does not distort YoY growth — the warehouse today would let a 371-day fiscal quarter inflate growth silently.
3. **No calendar-aligned LTM/TTM.** `fundamental_ttm_points` (`refresh_fundamental_ttm_points`) sums four *fiscal* quarters to an anchor's `period_end`; there is no re-expression of statements onto **common calendar** period-ends, so two issuers with offset FYEs cannot be TTM-compared on the same calendar date.
4. **The quarterly-TTM stitch is blocked on the current cache — this is the documented wall.** Per `PARITY_GAP.md`: "the cached consolidated duration windows are 3-mo/6-mo/12-mo only (**no ~9-mo YTD bucket**) and the 3-mo facts are sparse (**≤3 securities**), so neither the annual−9mo reconstruction nor a trailing-four-quarter sum yields enough complete TTM windows." The `derived_ytd_quarter_points` CTE already does current-YTD − prior-YTD differencing, but with only a 6-mo and 12-mo YTD present it can synthesize a Q2 and (annual − 6mo) H2, never the clean Q4 = annual − 9-mo YTD. The real lever is a **wider proof-slice universe with richer 10-Q duration coverage**, not new algebra.

**Already good — do not regress:**
- **The four-date PIT model.** `datadate` / `rdq` / `pdate` / `fdate` / `ldate` and the `revision_sequence` / `is_latest_revision` window in `refresh_fundamental_periods` stay exactly as they are. Calendarization is *additive* — new columns/tables, never a rewrite of the period spine.
- **The YTD-difference machinery.** `ytd_points` / `prior_ytd_points` / `derived_ytd_quarter_points` (with its `greatest(availability_ts)` PIT watermark and the `QUALIFY row_number()` latest-prior pick) and the `quarter_source_priority` precedence in `quarter_points` / `visible` are the reference algorithm the stitch extends — reuse them, do not replace them.
- **The 400-day self-join bound.** The `q.period_end > a.period_end - INTERVAL 400 DAY` guard that stops the trailing-TTM self-join from going triangular (O(N²), "spilled tens of GiB once the universe widened to ~1,600 issuers") stays — the proof-slice stitch must not remove it.

---

## PIT / determinism + production contract

ROADMAP clauses **(B)** append-only catalogued migrations, **(C)** offline/no-network tests, **(D)** determinism + provenance apply in full. (A) bitemporal correctness carries through unchanged from the period spine — calendarization derives, it does not re-date.

- **(B)** Migrations **0114–0116** only, forward-only, idempotent (`CREATE … IF NOT EXISTS` / `ADD COLUMN IF NOT EXISTS`). `0114` the calendar-map columns/table + `table_catalog`/`field_catalog` seeds; `0115` the calendar-aligned TTM surface + catalog; `0116` reserved for indexes and the coverage-check catalog rows (split schema-vs-index per the S5g/S5k WAL precedent). Never edit or renumber a landed migration; preserve a timestamped DB+WAL backup before any live apply.
- **(C)** Every test in `test_calendarization.py` runs against in-memory / template-copy DuckDB with hand-built fixture rows: a non-Dec-FYE issuer (e.g. FYR=6), a 53-week fixture (a 371-day fiscal quarter), and a proof-slice fixture carrying a 3-mo + 6-mo + 9-mo + 12-mo duration set. No SEC / companyfacts network in pytest. Live proof-slice counts are operator-run and recorded in the ledger.
- **(D)** `compute_*` calendarization transforms are pure (pandas in → long DataFrame out), unit-tested independent of DuckDB. The FYR label, `period_length_days`, `is_53_week`, and each of the three schemes are deterministic functions of `(period_start, period_end, fyr)`; same inputs → same rows. Every calendarized/TTM row records `input_statement_point_ids_json` + `calculation_method` lineage.
- **Data posture.** Ship the injectable loader + engine + offline fixtures first, then an operator-run **~1-year recent proof slice** with richer 10-Q duration coverage and live counts in the ledger. No large historical backfill — prove the pipeline on the recent slice.

---

## Tasks

### S6-0 — FYE→calendar mapping + period-length / 52-53-week flags *(the big one)*

**Root cause:** `refresh_fundamental_periods` labels calendar periods with a bare `EXTRACT(… FROM period_end)` — scheme (2) "containing calendar period" — with no `FYR` awareness, no fiscal-year+quarter scheme, and no greatest-overlap scheme, so any non-December FYE issuer is mislabelled and cannot be summed or compared against a December filer. There is also no period-length or 53-week signal on the period row.

**Fix:** NEW `db/calendarization.py` implements the FYR rule as a pure transform and a migration surface. Derive `fyr` (FYE calendar month) from each issuer's fiscal `period_end`, then emit **all three period-identification schemes** on a new calendarization surface (a `fundamental_calendar_map` table, migration **0114**, catalogued): (1) fiscal year + fiscal quarter as reported, (2) the calendar year/quarter containing the FYE, and (3) the calendar year/quarter with **greatest overlap** — applying the rule `6 ≤ FYR ≤ 12` ⇒ containing year labels FY, `1 ≤ FYR ≤ 5` ⇒ prior calendar year. Compute `period_length_days` from `(period_start, period_end)` and set `is_53_week` (plus `week_count`) when a fiscal quarter/year runs a full week long (>364d for an annual, the analogous 4-4-5 overrun for a quarter). Every fiscal period must map to **exactly one** calendar label under each scheme.

**PIT:** (B) 0114 seeds `table_catalog`/`field_catalog`. (D) FYR label, `period_length_days`, `is_53_week`, and all three schemes are pure deterministic functions of `(period_start, period_end, fyr)`.

**Accept:** a June-FYE (FYR=6) fixture maps to the greatest-overlap calendar year the rule prescribes and differs from its naive containing-year label; a 371-day fiscal-quarter fixture sets `is_53_week = true` with the right `week_count`; every fixture period resolves to exactly one label per scheme; the pure transforms are unit-tested with no DB.

### S6-1 — Calendar-aligned LTM/TTM surface

**Root cause:** `fundamental_ttm_points` sums four *fiscal* quarters to a fiscal anchor — there is no re-expression of flows onto common calendar period-ends, so offset-FYE issuers are not TTM-comparable on a shared calendar date.

**Fix:** in `db/calendarization.py`, build a calendar-aligned LTM/TTM surface (a `fundamental_calendar_ttm` table, migration **0115**, catalogued) that re-expresses the standardized/statement flows onto the scheme-(3) greatest-overlap calendar quarters and sums the trailing four *calendar* quarters. Reuse the existing `derived_ytd_quarter_points` differencing and the `quarter_source_priority` precedence; carry the same `coverage_days` / `quarter_count` completeness fields and a `calculation_method` = `'calendar_aligned_ttm'` tag. Keep the 400-day trailing self-join bound.

**PIT:** (B) 0115 catalogued. (C) fixture with two offset-FYE issuers. (D) the calendar re-expression is a deterministic join on the scheme-(3) labels.

**Accept:** two fixture issuers with offset FYEs both emit a TTM row keyed to the same calendar quarter-end; `quarter_count = 4` / `coverage_days` within tolerance of 365; a partial-coverage issuer is flagged incomplete, not silently summed.

### S6-2 — Quarterly-TTM stitch on the proof slice

**Root cause:** the stitch is blocked because the cached consolidated durations are 3-mo/6-mo/12-mo only (no ~9-mo YTD) and 3-mo facts are sparse (≤3 securities), so neither `annual − 9-mo` nor a trailing-four-quarter sum completes enough TTM windows.

**Fix:** extend the TTM region of `refresh_fundamental_ttm_points` (in `db/fundamental_statements.py`) so that, on the operator-loaded ~1yr proof slice with richer 10-Q duration coverage, the stitch materializes: **Q4 3-mo = annual − 9-mo YTD** (a new 9-mo bucket in the `derived_ytd_quarter_points` join, sitting between the existing 6-mo and 12-mo windows), and the **trailing-four-quarter sum** (Q1+Q2+Q3+Q4) as the alternate path, tagged `calculation_method` = `'stitched_quarterly_ttm'`. Guard so the path no-ops (does not fail) on the thin default cache where the 9-mo bucket is absent, mirroring the existing sparse-data behavior.

**PIT:** (C) proof-slice fixture with a full 3/6/9/12-mo duration set; the thin-cache fixture proves the no-op guard. (D) reconstruction is a deterministic YTD difference with the `greatest(availability_ts)` watermark preserved.

**Accept:** on the proof-slice fixture, Q4 = annual − 9-mo reconstructs and equals the independently-summed trailing-four-quarter TTM within tolerance; on the thin-cache fixture the path emits zero rows and does not error; no TTM window is double-counted.

### S6-3 — Calendarization coverage report + gated quality check

**Root cause:** once mapping and stitching land there is no report that proves every period got exactly one calendar label, that 53-week quarters were flagged, and that no TTM window was double-counted.

**Fix:** add a calendarization coverage report (in `db/calendarization.py`) and register a gated quality check (catalog rows in migration **0116**) asserting: every `fundamental_periods` / calendar-map row resolves to **exactly one** calendar label per scheme (no unmapped, no double-mapped); every fiscal period with `period_length_days` over the 4-4-5 threshold carries `is_53_week = true`; and no calendar-aligned or stitched TTM window is double-counted (each `(security_id, calendar_period, canonical_metric)` appears once per revision). Wire the counts into the dataset load result so the orchestrator and `PARITY_GAP.md` surface real coverage.

**PIT:** (B) 0116 catalogued; check registered per clause (G). (D) coverage assertions are pure counts over loaded rows.

**Accept:** the coverage check is green on the fixtures and demonstrably red on a planted double-mapped period and a planted unflagged 53-week quarter; the report emits per-scheme mapped/unmapped/double-mapped counts and the 53-week and double-counted-TTM tallies.

---

## Sequencing & expected compounding

**S6-0 → S6-1 → S6-2 → S6-3.** S6-0 (FYR mapping + flags) is load-bearing: every calendar label downstream depends on the greatest-overlap scheme and the 53-week flag, so it lands first. S6-1 (calendar-aligned TTM) consumes S6-0's scheme-(3) labels. S6-2 (proof-slice stitch) reuses the same YTD-difference machinery and is the network-gated payoff — it stays behind an injectable slice so the offline suite never depends on it. S6-3 (coverage/gate) records the outcome of all three. The compounding: once non-Dec-FYE issuers carry a correct calendar label and 53-week quarters are flagged, cross-company comparison and calendar-aligned TTM become trustworthy, and the ratio/valuation surfaces built on TTM inherit a normalized, YoY-safe time axis instead of a fiscal one.

---

## Risks / guardrails

- **Greatest-overlap edge cases.** The FYR=6 boundary (June) and the 1–5 vs 6–12 split are the classic off-by-one — pin them with fixtures at FYR ∈ {1, 5, 6, 12} and assert the label against the hand-computed greatest-overlap year, not against `EXTRACT`.
- **Never let a 53-week period inflate YoY silently.** The `is_53_week` flag must be *set and consumable*, not cosmetic; the coverage check fails if an over-length period is unflagged. Do not "normalize" by clipping days — flag and let the consumer adjust.
- **Proof-slice stitch must not regress the thin cache.** The 9-mo path is additive and guarded; on the default 3/6/12-mo cache it emits zero rows and never errors. Keep the 400-day self-join bound — removing it re-opens the O(N²) tens-of-GiB spill.
- **No double-counting.** Q4 = annual − 9-mo and the trailing-four-quarter sum are two paths to the *same* window — dedupe via `quarter_source_priority`/`calculation_method` so a metric's TTM is emitted once per revision, never twice.
- **Migration/WAL safety.** Split schema and index across 0114/0115/0116 per the S5g/S5k precedent; timestamped DB+WAL backup before any live apply; stay strictly within 0114–0116.

---

## Bench / acceptance

- **FYR mapping proven:** non-Dec-FYE fixtures map to the correct greatest-overlap calendar period under scheme (3), distinct from the naive containing-year label; all three schemes emit exactly one label per period.
- **53-week flagged:** the over-length fiscal-quarter/year fixtures set `is_53_week` + `week_count`; no unflagged over-length period survives the coverage check.
- **Calendar-aligned TTM emits:** offset-FYE issuers produce TTM rows on a shared calendar quarter-end; `quarter_count`/`coverage_days` correct; partial coverage flagged.
- **Quarterly stitch works on the proof slice:** Q4 = annual − 9-mo equals the trailing-four-quarter sum within tolerance; the thin-cache path no-ops; no double-counted windows.
- `python -m pytest atx-impl\db\tests\test_calendarization.py -q` green (and full `atx-impl\db\tests -q` stays green before commit).
- **Live-DB smoke** recorded in the ledger: proof-slice `fundamental_calendar_map` / `fundamental_calendar_ttm` row counts, the count of non-Dec-FYE issuers relabelled, the 53-week-flagged count, the stitched-TTM window count, and the `run_id`.
- **Ledger row appended** to `WAREHOUSE_PARITY_TRANCHES.md` (start/end SHA, domains, verification commands, live-DB smoke with exact counts + run_id, caveats/next); `PARITY_GAP.md` PF2-S6 / quarterly-TTM status updated (the "no ~9-mo YTD bucket" wall marked resolved-on-proof-slice).

**Process:** never `git add -A` (stage explicit paths); never push unless asked. New module ⇒ new `test_*.py`. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
