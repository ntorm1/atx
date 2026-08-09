# Sprint PF2-S4 — PIT snapshot + as-first-reported vintage

**Goal:** give the warehouse a Compustat-Snapshot *as-of-month* reconstruction and an explicit as-first-reported (unrestated) vs most-recently-restated vintage split, then extend that split into ratio-vintage history and a reader/CLI returning the filing-correct value for any `(security, item/ratio, as_of_month)`. Reserved migrations **0107–0109**.

**Mandate / Owns:** NEW `db/pit_snapshot.py` (bitemporal-facts → end-of-month reconstruction + vintage classifier), a vintage extension to `db/fundamental_ratios.py` (`_ratio_record` / `_ratio_id` / `load_ratio_inputs` / `RATIO_COLUMNS`), an as-of-month reader in `db/asof.py` (+ a `scripts/query_asof.py` view), and `db/tests/test_pit_snapshot.py`.

**Must NOT touch:** the standardization region PF2-S3 owns in `fundamental_ratios.py` (run **after** S3, never concurrently in the same tree — both touch this module in different regions); the valuation-multiple rows, `formula_registry`/`formula_library.py` derivation logic, and the fact-layer `fundamental_fact_revisions` *producer* (S4 reads the revision chain, never rewrites it). Do not renumber or edit a landed migration; append only under **0107–0109**.

**Depends on:** pf1-S8 restatement lineage — `accession_number`/`filed_date` propagated to `fundamental_statement_points` and `fundamental_ratios`, the fact-layer chain in `fundamental_fact_revisions` (`is_latest_revision`, `previous_value`, `value_delta`, `previous_accession_number`, `previous_filed_date`), and the four-date model `pdate ≤ rdq ≤ fdate ≤ ldate`. Also PF2-S3's standardized surface (the inputs S4 vintages). Sequential after PF2-S3.

---

## Baseline / where the cycles go

The gap is precise: Compustat *Point-in-Time / Snapshot* reconstructs the DB as it stood at the **end of any month** (from 1987) and ships two supplemental vintages — **as-first-reported / unrestated** (the FIRST entry for a firm fiscal-year/quarter, preliminary OR final; note *unrestated ≠ unadjusted*) and **most-recently-restated** (the latest re-standardization). That monthly as-of view is what defeats look-ahead and survivorship bias. A raw-XBRL warehouse inherits restatement lineage from re-filed documents but does **not** get the monthly vintage view unless it materializes it — this sprint is that materialization. Measured against the tree today:

1. **Ratios collapse to latest revision.** `_ratio_record` hard-codes `"is_latest_revision": True` (line 255) and `load_ratio_inputs`'s four input CTEs (`ttm`, `bal`, `balx`, `flowx`) each filter `WHERE ... is_latest_revision`. One `fundamental_ratios` row exists per `(security, period)`; the earlier as-reported vintage is gone. The module docstring already flags it: *"v1 stores only the latest-revision vintage … a ratio row per restatement vintage is a planned, non-breaking refinement (the bitemporal columns are already present)."*
2. **No as-first-reported vs most-recently-restated classification.** Nothing tags a fact/ratio as the FIRST reported entry for its fiscal period vs the latest restatement. A basis-correct earnings surprise must join the **as-first-reported** actual (IBES keeps the originally-reported number; Compustat overwrites with the restated one) — today there is no column to select it by.
3. **No as-of-*month* reconstruction; the ratio reader can only return newest.** `FUNDAMENTAL_RATIOS_ASOF_SQL` ends `WHERE r.available_at <= p.as_of_ts AND r.as_of_date <= p.as_of_date AND r.is_latest_revision` (asof.py:3189) — it hard-filters `is_latest_revision`, returning the globally-newest revision regardless of `as_of_ts`, never the vintage a Snapshot user saw at month-end M.
4. **The bitemporal raw material is all present, thrown away one layer too early.** `RATIO_COLUMNS` carries `is_latest_revision`, `as_of_date`, `available_at`, `input_codes_json`, `run_id`; `_ratio_id` hashes `source|security_id|ratio_code|basis|period_end|available_at` (no accession — vintages would collide); `fundamental_statement_points` carries `accession_number`, `available_at`, `as_of_date (= filed_date)`; `fundamental_fact_revisions` already answers "which filing moved this fact, by how much."

**Already good — do not regress:**
- **`compute_ratio_rows` is a pure DataFrame→DataFrame transform**, unit-tested independent of DuckDB. Vintage is a new grouping key + passthrough columns, **not** new I/O in the transform.
- **Each ratio's `available_at = max(input.available_at)`** (compute loop, line 290) — no-lookahead is enforced at emit; S4 preserves it exactly.
- **The fact-layer revision chain** (`fundamental_fact_revisions`) is authoritative; S4 lifts it to ratios/snapshots by reference, never by re-deriving deltas.
- **The as-of reader family** (`FUNDAMENTAL_STATEMENTS_ASOF_SQL`, `FUNDAMENTAL_TTM_ASOF_SQL`) already ranks with `row_number() OVER (PARTITION BY … ORDER BY as_of_date DESC, available_at DESC …) rn … WHERE rn = 1`. The new month reader mirrors that shape; it does not invent a new one.

---

## PIT / determinism + production contract

This sprint is *entirely* about PIT correctness. ROADMAP clauses **(A)–(D)** apply in full.

- **(A) Bitemporal / no lookahead.** Every snapshot and vintage row keeps `as_of_date`, `available_at`, `source_loaded_at`, `run_id`, `is_latest_revision`. A derived value's `available_at` stays `max(input.available_at)`. The as-of-month reader gates on `available_at ≤ month_end_ts` **and** `as_of_date ≤ as_of_date` — a restated value is invisible until its restating filing's `available_at`; a first-reported value is invisible before it was filed.
- **(B) Append-only, catalogued migrations 0107–0109.** `0107` schema — vintage columns on `fundamental_ratios` (`ADD COLUMN IF NOT EXISTS`) + NEW `fundamental_pit_snapshot` table; `0108` index — vintage/month lookup indexes (schema and index never share a migration, per the S5g/S5k WAL precedent); `0109` catalog/reserved — `table_catalog` + `field_catalog` seed for the new surface and any gated no-lookahead check. Idempotent `CREATE … IF NOT EXISTS`.
- **(C) Offline tests.** `test_pit_snapshot.py` runs on in-memory DuckDB with a two-accession restatement fixture (one period, original then restated). No network. The ~1yr proof-slice month reconstruction is operator-run live smoke recorded in the ledger.
- **(D) Determinism + provenance.** The vintage classifier and month reconstruction are pure functions of loaded rows; same inputs + same `as_of_month` → same rows. Every vintage row records its driving `source_accession` + `input_codes_json`.

---

## Tasks

### S4-0 — As-of-month snapshot reconstruction *(the big one)*

**Root cause:** no surface answers "what did the DB hold at end-of-month M?" The only as-of primitive is instant-`as_of_ts` and, for ratios, it is welded to `is_latest_revision` — a month-boundary Snapshot cannot be reconstructed.

**Fix:** NEW `db/pit_snapshot.py` reads the already-bitemporal facts (`fundamental_statement_points`, `fundamental_ttm_points`, `fundamental_fact_revisions`) and reconstructs the DB **as of end-of-month M**: for each `(security_id, canonical_metric, period_end)` pick the newest `accession_number`/`filed_date` vintage whose `available_at ≤ month_end` (last instant of M) — the month-boundary knowledge gate. Materialize NEW `fundamental_pit_snapshot` (migration `0107`: `snapshot_month DATE`, `security_id`, `canonical_metric`, `period_end`, `value`, `vintage_class`, `source_accession`, `filed_date`, `available_at`, `as_of_date`, `run_id`, deterministic `snapshot_id = sha256(…)`) over the operator proof slice; index in `0108`. Keep `compute_pit_snapshot_rows` a pure transform + a `PitSnapshotDataset`/`refresh_pit_snapshot` writer mirroring `FundamentalRatiosDataset`.

**PIT:** (A) the `available_at ≤ month_end` gate *is* the no-lookahead invariant; (D) pure over loaded rows.

**Accept-with-fixture:** a period filed in month M and restated in M+2 yields, at snapshot_month M, the original value; at M+2, the restated value; a metric filed *after* month-end is absent from that month.

### S4-1 — as_first_reported vs most_recently_restated vintage split

**Root cause:** nothing distinguishes the FIRST reported entry for a fiscal period from the latest restatement, so the unrestated actual (needed for basis-correct surprise) is unselectable.

**Fix:** add `vintage_class VARCHAR` (migration `0107`, `ADD COLUMN IF NOT EXISTS`) to `fundamental_ratios` and `fundamental_pit_snapshot`, plus defensive `source_accession VARCHAR` / `filed_date DATE` (`ADD COLUMN IF NOT EXISTS` — no-op where pf1-S8 already added them). A pure classifier in `pit_snapshot.py` tags each `(security_id, period_end)` group: **`as_first_reported`** = the min-`filed_date` / earliest `accession_number` vintage (unrestated; preliminary or final — *not* unadjusted); **`most_recently_restated`** = the max-`filed_date` vintage (== the `is_latest_revision`-true row). Ties broken deterministically by `accession_number`.

**PIT:** (A) reads loaded `filed_date`/`available_at` only; (D) deterministic per group.

**Accept-with-fixture:** the two-accession fixture yields exactly one `as_first_reported` and one `most_recently_restated` row per period; first-reported `filed_date` < restated `filed_date`.

### S4-2 — Ratio-vintage history (additive, non-breaking)

**Root cause:** `_ratio_record` pins `is_latest_revision = True` and `load_ratio_inputs` filters inputs to `is_latest_revision`, collapsing to one ratio per period.

**Fix:** emit **one `fundamental_ratios` row per input vintage**. Extend `_ratio_id` to hash `source_accession` alongside the existing `source|security_id|ratio_code|basis|period_end|available_at` so distinct vintages get distinct `ratio_id`s (no insert collision). Give `load_ratio_inputs` a vintage-aware mode that, instead of `WHERE is_latest_revision`, pivots inputs per accession vintage (newest input available as of each vintage's `filed_date`) and stamps `vintage_class` + `source_accession`; set `is_latest_revision` correctly per `(security, period)` group (max-`filed_date` = latest, earlier = `False`) — **never** blanket-`True`. Add `vintage_class`, `source_accession`, `filed_date` to `RATIO_COLUMNS`. `compute_ratio_rows` stays pure (new grouping key + passthrough only); latest-vintage rows stay byte-identical to today's single row.

**PIT:** (A) each vintage keeps its own `available_at`; `is_latest_revision` becomes a *view* over history, not a destructive filter. (D) same inputs → same vintage rows.

**Accept-with-fixture:** a period restated with revised net income yields two `net_profit_margin` rows, distinct `source_accession`, distinct `value`, exactly one `is_latest_revision = True`; the between-vintage delta matches `fundamental_fact_revisions.value_delta` for the driving fact.

### S4-3 — As-of-month reader + CLI

**Root cause:** `FUNDAMENTAL_RATIOS_ASOF_SQL` hard-filters `is_latest_revision`, so no caller can read a month-vintage.

**Fix:** add `FUNDAMENTAL_RATIOS_ASOF_MONTH_SQL` + `fundamental_ratios_asof_month(as_of_month, …)` and a `pit_snapshot_asof(…)` reader in `db/asof.py`, mirroring the existing `row_number() OVER (PARTITION BY security_id, ratio_code, period_end ORDER BY filed_date DESC, available_at DESC …) rn … WHERE rn = 1` shape and reusing `end_of_day_asof_ts` (applied to month-end), `_register_filter`, `_normalize_symbols`. Gate on `available_at ≤ month_end_ts`; return the newest accession vintage as of that instant — **drop the `is_latest_revision` predicate**. Add a `--view fundamental-ratios-asof-month` (and `pit-snapshot`) branch to `scripts/query_asof.py`, imported and dispatched exactly like the current `fundamental_ratios_asof` branch, with an `--as-of-month` arg.

**PIT:** (A) enforces `available_at ≤ month_end` and picks the latest accession as of that instant — exact Compustat-Snapshot semantics; no lookahead.

**Accept-with-fixture:** with the S4-2 two-vintage fixture, `fundamental_ratios_asof_month(month before restatement)` returns the original value and `(month after)` returns the restated value; exactly one row per `(security, period, ratio_code)` for any single month.

---

## Sequencing & expected compounding

Strictly ordered — each task consumes the prior one's output. **S4-0** (month reconstruction) builds the bitemporal reader everything stands on. **S4-1** (vintage split) classifies the rows S4-0 reconstructs. **S4-2** (ratio-vintage history) stops the ratio writer collapsing to latest so the split has vintages to select. **S4-3** (reader + CLI) exposes it. Compounding: by the end a single `fundamental_ratios_asof_month(M)` reproduces the exact as-reported ratio for month M, a join to `fundamental_fact_revisions` explains *which filing* moved it, and `vintage_class = 'as_first_reported'` gives the unrestated actual a basis-correct surprise needs — closing the monthly-Snapshot parity axis.

## Risks / guardrails

- **Double-counting.** Dropping the `is_latest_revision` input filter carelessly lets a naive aggregate sum every vintage. Mitigate: `source_accession` is an explicit `_ratio_id` key, `is_latest_revision` is set correctly per group, and an as-of test asserts exactly one row per `(security, period, ratio_code)` for any single `as_of_month`.
- **No historical backfill.** Do **not** materialize monthly snapshots from 1987 — pf2 posture is a ~1yr recent proof slice. `fundamental_pit_snapshot` is populated over the operator slice only; deep backfill is a later operator concern.
- **Additive-only.** Latest-vintage rows must be byte-for-byte unchanged (regression guard on a fixture). New columns are `ADD COLUMN IF NOT EXISTS`; readers add a new function, never mutate `FUNDAMENTAL_RATIOS_ASOF_SQL`'s meaning.
- **Migration/WAL safety.** Only `0107–0109`; split schema (`0107`) from index (`0108`); preserve a timestamped DB+WAL backup before any live apply.

## Bench / acceptance

- `python -m pytest atx-impl\db\tests\test_pit_snapshot.py -q` green **offline** (in-memory DuckDB, two-accession restatement fixture, no network).
- `python -m pytest atx-impl\db\tests -q` green before commit.
- Month-reader parity: `fundamental_ratios_asof_month` returns original-before / restated-after across a restatement; `vintage_class` split is exactly one first-reported + one most-recently-restated per period.
- **Live smoke** (operator, recorded in ledger): `python atx-impl\scripts\query_asof.py --view fundamental-ratios-asof-month --as-of-month 2026-05-31 --symbols AAPL` returns the month-M vintage; `--view pit-snapshot` reconstructs the slice; record exact row counts + `run_id`.
- `PARITY_GAP.md` status updated (PF2-S4 axis: monthly as-of + as-first-reported vintage) and a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live-DB smoke with exact counts + `run_id`, caveats/next).

**Process:** never `git add -A` (stage explicit paths — the tree carries unrelated dirty/untracked files); never push unless asked; new module ⇒ new `test_*.py`. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
