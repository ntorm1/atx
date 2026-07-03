# Sprint PF-S8 — Restatement Vintage Lineage + As-Of Parity

**Goal:** propagate `accession_number` + filing_date through `fundamental_statement_points` -> `fundamental_ratios`; enforce the four-date audit chain (`pdate <= rdq <= fdate <= ldate`); answer "which filing changed this ratio"; add quarterly-TTM stitching where the widened PF-S3 coverage now allows. Reserved migrations `0092-0096`.

**Mandate / Owns:** `db/fundamental_statements.py` + `db/fundamental_ratios.py` lineage columns, `db/asof.py` vintage readers, TTM stitching, `db/tests/test_restatement_lineage.py`.

**Must NOT touch:** the valuation-multiple region of `fundamental_ratios.py` concurrently with PF-S6 — run AFTER PF-S6. Do not edit a prior sprint's migration or region; append only under `0092-0096`.

**Depends on:** PF-S1 (canonical item dim), PF-S3 (concept coverage — supplies the quarterly buckets TTM stitching needs), PF-S4 (formula/ratio engine), and PF-S6 for the `fundamental_ratios.py` region-coordination handshake.

---

## Baseline / where the cycles go

This is the last sprint in pf1 and it closes the fourth wall from the ROADMAP: *"facts do not link cleanly, and vintages are lost."* The spine already carries the raw material — the work is to thread it end-to-end, not to invent it.

**Table — what is missing (measured against the current tree):**

| Symptom | Concrete anchor in the code today |
|---|---|
| `accession_number` exists at the fact and statement-point layer but dies before ratios | `fundamental_statement_points` is inserted with `accession_number` (statements module, insert list ~line 553) and `fundamental_periods` carries it (line 797), but `fundamental_ratios.RATIO_COLUMNS` has **no** `source_accession`/`filed_date` column, and `_ratio_record` never reads one. No ratio can answer "which filing produced this?" |
| Four-date invariant not enforced end-to-end | `refresh_fundamental_periods` derives all four dates — `datadate` (= `period_end`, line 845), `rdq`/`pdate` (from the 8-K item-2.02 lateral, lines 896-928), `fdate` (= `max(as_of_date)`, line 865), `ldate` (= `max(available_at)` cast to date, line 937) — but nothing asserts `pdate <= rdq <= fdate <= ldate`. Violations (e.g. an rdq later than fdate from a mis-joined 8-K) pass silently. |
| Ratios store latest-revision only | `_ratio_record` hard-codes `"is_latest_revision": True` (line 569); `load_ratio_inputs` filters every CTE on `WHERE ... is_latest_revision` (ttm, bal, balx, flowx). One ratio row per (security, period) — the earlier vintage is gone. |
| As-of ratio reader cannot return an old vintage | `fundamental_ratios_asof` (asof.py) already exists but its SQL ends `AND r.is_latest_revision` (line 3189) — it can only ever return the newest revision, never the filing-correct vintage for an earlier `as_of_ts`. |
| Quarterly-TTM stitching blocked | TTM is currently built only where annual flows align; the Q1+Q2+Q3 + (annual − 9-mo YTD) reconstruction is impossible because the `multi_quarter_ytd` (9-mo) bucket is sparse and 3-mo facts are thin. PF-S3 widens concept coverage; this sprint gates stitching on bucket completeness rather than assuming it. |

**Already good — do not regress:**

- **Bitemporal columns already present** on `fundamental_ratios` (`as_of_date`, `available_at`, `is_latest_revision`, `run_id`, `input_codes_json`) and on statement points — the schema is ready for a vintage key, only the write path needs to stop collapsing to latest.
- **`fundamental_fact_revisions`** already computes the revision chain per fact: `is_latest_revision`, `previous_value`, `value_delta`, `value_delta_percent`, `previous_accession_number`, `previous_filed_date` (fundamentals module, `refresh_fundamental_fact_revisions`). "Which filing changed this fact, and by how much" is answered **at the fact layer today** — S8 lifts that same answer up to the ratio layer via `source_accession` + delta lineage.
- **`compute_ratio_rows` is a pure DataFrame->DataFrame transform** (fundamental_ratios.py), unit-tested independent of DuckDB. Keep it pure: vintage is a new grouping key + a new passthrough column, not new I/O in the transform.
- The four dates are already *computed* correctly in `fundamental_periods`; S8 enforces and propagates them, it does not recompute them.

---

## PIT / determinism contract

This sprint leans on ROADMAP clauses **(A)** bitemporal correctness, **(B)** append-only catalogued migrations, and **(D)** determinism + provenance — and it is the sprint that makes clause (D)'s provenance promise concrete: *"every derived row records `input_codes_json` and, from PF-S8, `source_accession` / `filed_date`."*

- **Vintage is explicit.** A ratio row now carries `available_at` (already), plus `source_accession` (the driving filing) and `filed_date` (its knowledge time). `available_at = max(input.available_at)` is unchanged; `source_accession` records *which* filing that max-availability input came from.
- **As-of readers return the filing-correct vintage.** For a given `as_of_ts`, the reader returns the ratio computed from the newest *accession available at that instant* — not the globally-newest revision. Two `as_of_ts` straddling a restatement return two different values, both correct for their instant.
- **Four-date invariant is a PIT guardrail, not cosmetics:** `pdate <= rdq <= fdate <= ldate` is the Compustat-Snapshot ordering (preliminary announcement date <= report date of quarter <= final SEC filing date <= last-revised date). A violation means a mis-dated knowledge-time and is a lookahead risk, so it is surfaced as a quality check, not swallowed.
- **Offline.** Every test runs in in-memory DuckDB against restatement fixtures (two accessions for one period with different values); no SEC/network in the test path (clause C).

---

## Tasks — S8-0 .. S8-4

Each task states **Root cause / Fix / PIT / Accept.**

### S8-0 — Propagate provenance (columns + backfill)

**Root cause:** `accession_number` and filing date are present on `fundamental_statement_points` and `fundamental_periods` but are dropped at the ratio boundary — `RATIO_COLUMNS` and `_ratio_record` have no slot for them, so lineage terminates one layer too early.

**Fix:**
- Migration `0092` (schema): `ALTER TABLE fundamental_statement_points ADD COLUMN IF NOT EXISTS source_accession VARCHAR` and `... filed_date DATE` (idempotent); same two columns on `fundamental_ratios`. Seed `table_catalog` / `field_catalog` for the new fields in the same migration (clause B).
- Migration `0093` (index): a lookup index on `fundamental_ratios (security_id, period_end, source_accession)` and on `fundamental_statement_points (security_id, period_end, source_accession)`, split into its own migration number per the S5g/S5k WAL-replay precedent (schema and index never share a migration).
- Backfill: `source_accession`/`filed_date` on statement points come straight from the existing `accession_number` and the fact `filed_date`. On ratios, they are threaded through `load_ratio_inputs` (carry the driving accession into the wide frame) and written by `_ratio_record`. Add `source_accession`/`filed_date` to `RATIO_COLUMNS`.

**PIT:** columns are provenance only; they do not change any existing value or `available_at`. Backfill is a pure re-derivation from already-loaded rows — deterministic (clause D).

**Accept:** every ratio row has a non-null `source_accession` wherever its inputs did; `field_catalog` lists the four new columns; existing ratio values are byte-for-byte unchanged on a re-run (regression guard on a fixture).

### S8-1 — Four-date invariant enforcement

**Root cause:** `refresh_fundamental_periods` computes `datadate`/`rdq`/`pdate`/`fdate`/`ldate` but nothing checks their ordering; a mis-joined 8-K item-2.02 or a clock-skewed `available_at` can produce `rdq > fdate` and go unnoticed.

**Fix:**
- Document the mapping from the existing period fields to the four-date model in a module docstring / comment block: `datadate` = period close (`period_end`), `rdq` = report date of quarter (8-K 2.02 lateral), `pdate` = preliminary/announcement date (currently aliased to `rdq`), `fdate` = final filing/as-of date (`max(as_of_date)`), `ldate` = last-revised date (`max(available_at)::DATE`).
- Add a `quality_check` (via `db/quality.py`) `four_date_invariant` that counts `fundamental_periods` rows violating `pdate <= rdq <= fdate <= ldate` (NULLs skipped, not counted as violations — many periods have no 8-K rdq). Status `passed` at zero violations, `warning` otherwise, with the violating count as `observed_value` and the offending `period_group_id`s in `details`.

**PIT:** the check reads only fields already materialized; it asserts the knowledge-time ordering that clause (A)'s "no lookahead" depends on. No new lookahead surface.

**Accept:** the check runs green on the current live periods (or produces a triaged, documented count if any legacy rows violate); a fixture with a deliberately inverted `rdq > fdate` row trips the check to `warning`.

### S8-2 — Restatement vintage ratio history

**Root cause:** `_ratio_record` pins `is_latest_revision = True` and `load_ratio_inputs` filters inputs to the latest revision, so a period restated by a later 10-K/A overwrites its prior ratio row entirely — the history is unrecoverable and "which filing changed this ratio" is unanswerable.

**Fix:**
- Allow **multiple** ratio rows per (security, period), keyed by input vintage/accession. The vintage key extends the existing `_ratio_id` hash (already includes `available_at`) with `source_accession` so distinct vintages produce distinct `ratio_id`s and do not collide on insert.
- `load_ratio_inputs` gains a vintage-aware mode: instead of `WHERE is_latest_revision`, pivot inputs per accession vintage (the newest input available *as of each accession's filed_date*), so each emitted ratio row reflects a coherent as-reported snapshot. Set `is_latest_revision` correctly per (security, period) group (the max `filed_date` vintage is latest; earlier vintages are `False`) — **do not** blanket-`True`.
- "Which filing changed this ratio" is then answerable by joining a ratio's `source_accession` back to `fundamental_fact_revisions` (`previous_accession_number`, `previous_value`, `value_delta`) on the driving input fact — the delta story already exists at the fact layer and is now reachable from any ratio.

**PIT:** each vintage row keeps its own `available_at` = max input availability at that vintage; no row is visible before its inputs were knowable. The latest-revision flag becomes a *view* over history, not a destructive filter (clause A).

**Accept:** a two-accession fixture (period restated; net income revised) yields two `net_profit_margin` rows with distinct `source_accession`, distinct `value`, one `is_latest_revision = True`; the delta between them matches `fundamental_fact_revisions.value_delta` for the driving fact.

### S8-3 — Quarterly-TTM stitching (gated)

**Root cause:** TTM today only assembles cleanly where annual flows align; the general quarterly reconstruction `TTM = Q1 + Q2 + Q3 + (annual − 9-mo YTD)` is blocked because the 9-mo (`multi_quarter_ytd`) bucket is sparse and standalone 3-mo facts are thin. Quarterly-TTM stitching has therefore been deprioritized until coverage exists.

**Fix:**
- Where PF-S3's widened concept coverage now supplies the buckets, reconstruct TTM flows from quarterly components using the `normalized_period_type` classes already produced by `refresh_fundamental_periods` (`quarter`, `semiannual_ytd`, `multi_quarter_ytd`, `annual`).
- **Gate on completeness:** emit a stitched TTM value *only* where every required bucket (three quarters + the annual and its matching 9-mo YTD) is present and internally consistent; otherwise emit nothing (never a partial or imputed TTM). Record which buckets were used in the row's provenance.
- Document the remaining sparse-data limits in the module: which industries / periods still lack the 9-mo bucket and are therefore left un-stitched. This sprint does not force coverage it does not have — it opens stitching where PF-S3 earned it and states the rest as a known gap.

**PIT:** a stitched TTM's `available_at` = max availability across all component buckets; it is never visible before the last component filed. Deterministic given the same component set (clause D).

**Accept:** a fixture with all four buckets (Q1, Q2, Q3, annual, 9-mo YTD) produces a stitched TTM equal to the hand-computed sum; a fixture missing the 9-mo bucket emits **no** stitched row (gate holds); the documented limits list is present.

### S8-4 — As-of vintage readers (asof.py)

**Root cause:** `fundamental_ratios_asof` ends its predicate with `AND r.is_latest_revision`, so it always returns the newest revision regardless of `as_of_ts` — it cannot reproduce what a ratio *looked like* at an earlier instant, which is the whole point of a Compustat-Snapshot PIT read.

**Fix:**
- Replace the `is_latest_revision` filter with a vintage-recency window: for each (security, period, ratio_code), rank rows by the newest `source_accession`/`filed_date` whose `available_at <= as_of_ts`, and return rank 1. This returns the filing-correct vintage as of that instant (Compustat-Snapshot style), matching the `row_number() OVER (PARTITION BY ... ORDER BY as_of_date DESC, available_at DESC ...)` pattern already used by `fundamental_statements_asof` and `fundamental_ttm_asof`.
- Add matching vintage-aware behaviour to the statement/TTM readers only if needed to keep them consistent with the new ratio vintages; otherwise leave their region untouched (they already rank, they do not hard-filter latest).
- Keep the existing `symbol` / `ratio_code` / `category` filter joins and the `store`-passthrough signature intact (no breaking change to callers).

**PIT:** the reader enforces `available_at <= as_of_ts` and picks the latest accession *as of that instant* — the exact Compustat-Snapshot vintage semantics (clause A). No lookahead: a restated value is invisible until its restating filing's `available_at`.

**Accept:** with the S8-2 two-vintage fixture, `fundamental_ratios_asof(as_of_ts = before restatement)` returns the **original** value and `... (as_of_ts = after)` returns the **restated** value; the newest-vintage read matches the S8-2 `is_latest_revision = True` row.

---

## Sequencing & expected compounding

Strictly ordered — each task consumes the prior one's output:

1. **S8-0 (provenance columns)** — nothing downstream can key on a vintage that isn't stored. Land the columns + backfill first.
2. **S8-1 (four-date invariant)** — enforce the audit chain on the period rows the columns describe; cheap, isolated, and it validates the dates S8-2/S8-4 will trust.
3. **S8-2 (vintage history)** — with columns in place and dates trusted, stop collapsing to latest-revision; write one row per vintage.
4. **S8-3 (TTM stitching)** — orthogonal to vintage but sequenced here because it also writes flow rows that S8-2's vintage key must accommodate; gate strictly.
5. **S8-4 (readers)** — expose the vintage history through as-of; the visible payoff.

**Compounding:** by the end, a single `fundamental_ratios_asof(as_of_ts)` call reproduces the exact as-reported ratio for any instant, and a join to `fundamental_fact_revisions` explains *which filing* moved it *by how much*. That closes Compustat-Snapshot PIT parity — the last of pf1's six axes.

---

## Risks / guardrails

- **Double-counting risk:** if S8-2 drops the `is_latest_revision` input filter carelessly, a naive downstream aggregate could sum every vintage and double- (or n-times-) count a single period. **Mitigate:** the vintage key (`source_accession` in `_ratio_id`) is explicit, `is_latest_revision` is set correctly per group (never blanket-`True`), and an as-of test asserts exactly one row per (security, period, ratio_code) is returned for any single `as_of_ts`.
- **`fundamental_ratios.py` region coordination:** PF-S4, PF-S6, and PF-S8 all touch this module in different regions. **Run S8 strictly after PF-S6**, never concurrently in the same tree. S8 owns the lineage columns (`RATIO_COLUMNS`, `_ratio_record`, `load_ratio_inputs` vintage mode) and the `is_latest_revision` semantics; it does not touch the valuation-multiple rows PF-S6 adds.
- **Migration discipline:** use only `0092-0096`; never renumber or edit a landed migration; split schema (`0092`) from index (`0093`) per the WAL-replay precedent; preserve a timestamped DB+WAL backup before any live apply (clause B).
- **Four-date NULLs:** many periods legitimately lack an 8-K `rdq`; the invariant check must skip NULLs, not flag them — otherwise every non-earnings-8-K filer trips a false violation.
- **Process:** never `git add -A` (the tree carries unrelated dirty/untracked files — stage explicit paths); do not push unless asked.

---

## Bench / acceptance

- Ratio restatement is **traceable to an accession** — every ratio row carries `source_accession` + `filed_date`, joinable to `fundamental_fact_revisions` for the `value_delta` story.
- `pdate <= rdq <= fdate <= ldate` is **enforced** via a `four_date_invariant` quality check (green, or a documented/triaged legacy count).
- As-of readers return the **filing-correct vintage** for a given `as_of_ts` (original before restatement, restated after).
- TTM stitching **emits only where all buckets are complete**; remaining sparse-data limits documented.
- `python -m pytest atx-impl\db\tests\test_restatement_lineage.py -q` green **offline** (restatement fixtures, in-memory DuckDB, no network).
- `python -m pytest atx-impl\db\tests -q` green before commit; `PARITY_GAP.md` status updated; a row appended to `WAREHOUSE_PARITY_TRANCHES.md` (start/end SHA, domains, verification commands, live-DB smoke with exact counts + `run_id`, caveats/next).
- Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`; never `git add -A`.

**This is the last sprint in pf1.** With it, the warehouse's fundamentals spine reaches Compustat-Snapshot vintage/PIT parity: every ratio is traceable to its filing, every restatement is reproducible as-of, and the four-date audit chain is enforced end-to-end.
