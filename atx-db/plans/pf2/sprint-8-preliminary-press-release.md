# Sprint PF2-S8 — Press-release / preliminary→final + estimate-basis

**Goal:** capture the earnings figure the market actually saw on announcement day. Ingest 8-K Item 2.02 EX-99 press-release facts (non-XBRL) before the 10-Q/10-K, model a preliminary→final overwrite that retains the preliminary vintage and populates the `rdq`/`pdate` flash, and tag every EPS GAAP-vs-street so surprise is computed on a matched basis. End reserved migrations **0121–0123**.

**Mandate / Owns:** NEW `db/press_release.py` (8-K Item 2.02 EX-99 ingestion + preliminary→final overwrite), a `db/estimates.py` basis-tag extension (`est_actual` gains a `basis` column + basis-aware `est_surprise`), `db/tests/test_press_release.py`.

**Must NOT touch:** the ratio/formula engine (`fundamental_ratios.py`, `formula_registry`), the standardization engine (`standardization.py`, PF2-S3), or the fiscal/statement-map regions of `fundamental_statements.py` (PF2-S5/S6). This sprint *reads* `sec_company_facts` and `refresh_fundamental_periods`' output; it populates `rdq`/`pdate` through its own reconciliation surface, it never rewrites `refresh_fundamental_periods`' derivation SQL.

**Depends on:** pf1-S8's four-date model in `fundamental_periods` (`datadate`/`rdq`/`pdate`/`fdate`/`ldate`, from `refresh_fundamental_statements`), pf1's `est_actual` / `est_surprise` (`db/estimates.py`), and **PF2-S4**'s vintage split (`db/pit_snapshot.py`, `as_first_reported` / unrestated vs most-recently-restated) — the preliminary vintage ties into S4's as-first-reported lineage. **PARALLEL-SAFE** (disjoint NEW module) with the S5→S6 chain and S7; **sequential-only** against any other `estimates.py` toucher.

---

## Baseline / where the cycles go

The four-date model exists but is fed only by filings; the number the tape saw first never enters the warehouse. Measured 2026-07-03 against the live DB (default, no injectable corpus loaded).

1. **`est_actual` sources *only* from XBRL companyfacts.** `EstimateActualsDataset` (`dataset_id="est_actual"`, `source_name="sec_company_facts"`) `SELECT ... FROM sec_company_facts` with `form IN ('10-Q','10-K','8-K','10-K/A','10-Q/A')` and stamps `available_at` from the companyfacts filing availability. But earnings hit the tape via an **8-K Item 2.02 EX-99.1 press release** days-to-weeks earlier, and those exhibits are text/HTML, **not XBRL-tagged** — so they are absent from `sec_company_facts` and invisible to `est_actual`. **est_actual = 1,240 rows, all GAAP-as-filed; zero preliminary facts.**
2. **`est_surprise`'s "originally-reported" is the 10-Q, not the flash.** `EstimateSurpriseDataset` (`source_name="est_surprise_srw_drift"`, Foster-Olsen-Shevlin 1984 SRW-with-drift SUE) defines originally-reported as `row_number() OVER (PARTITION BY security_id, measure_code, fiscal_year, fiscal_period ORDER BY available_at ASC NULLS LAST)` with `rn=1`. Because the preliminary press number is missing, `rn=1` resolves to the **10-Q filing availability**, not announcement day. **est_surprise = 1,222 rows** all keyed off the final filing — the flash timestamp the event study needs is gone.
3. **`rdq`/`pdate` are derived from filing dates, never from the press release.** `refresh_fundamental_periods` builds `rdq` as `coalesce(report_date, filing_date, CAST(acceptance_datetime AS DATE))` from the `rdq_candidates` CTE, bounded `r.rdq >= period_end AND r.rdq <= fdate`. The Compustat `pdate` (preliminary) / `fdate` (final) columns are present in the `INSERT INTO fundamental_periods` list but `pdate` is never set to an actual press-release release time — it stays blank until a filing hits.
4. **No GAAP-vs-street basis tag on actuals.** `est_guidance` already carries a `basis` column (`_guidance_basis` → `"GAAP"` / `"NON_GAAP"` / `None`, normalized upper), but `est_actual` has **no `basis` column at all**. IBES/street actuals exclude items IBES judges non-operating *ex-post*; Compustat/XBRL EPS is GAAP. Computing `surprise = (GAAP actual − street consensus)` is a **basis mismatch → ~5-8% silent error**. Nothing marks which basis an EPS is on.
5. **The injectable text path exists but is unused for actuals.** `EstimateGuidanceOptions(source_file=..., source="est_guidance_injectable", fetch=, parse=, min_confidence=0.70)` already parses local 8-K Item 2.02/7.01 text (`_guidance_source_item` → `"8-K_2.02"`/`"8-K_7.01"`/`"8-K_2.02_7.01"`; `GUIDANCE_VALUE_RE`; `_guidance_scale`; `_guidance_confidence`), but **est_guidance = 0 rows** on the default DB and it only ever emits *guidance*, never a preliminary *actual*.

**Already good — do not regress:**
- **The four-date scaffold.** `fundamental_periods` already carries `datadate`/`rdq`/`pdate`/`fdate`/`ldate` + `first_available_at`/`latest_available_at`/`is_latest_revision`. This sprint *populates* `pdate`/`rdq` from the press release; it does not add or rename the columns.
- **The injectable+offline extraction pattern.** `EstimateGuidanceOptions`' `source_file`/`fetch`/`parse` + `min_confidence` + `run_id` design, `GUIDANCE_VALUE_RE`, `_guidance_scale`, `_guidance_confidence`, and the `_guidance_source_item` 2.02/7.01 cue logic are reused wholesale — `press_release.py` mirrors them, it does not reinvent parsing.
- **`est_actual`'s carried `available_at`.** The PIT rule "`available_at` CARRIED from `sec_company_facts`, do NOT restamp to `now()`" stays; the basis column is additive and the preliminary path stamps its *own* release-time `available_at`.
- **`est_surprise`'s rn=1 first-reported semantics.** The earliest-`available_at` selection is exactly right — once the preliminary fact exists with an earlier `available_at`, `rn=1` picks it *for free*. Do not change the SUE math.

---

## PIT / determinism + production contract

ROADMAP clauses **(A)** bitemporal correctness, **(B)** append-only catalogued migrations, **(C)** offline/no-network tests, **(D)** determinism apply in full; **(G)** quality-gating applies to S8-3's guard. Proven on the operator-run **~1-year recent proof slice**, fixtures only in pytest.

- **(A)** Every preliminary fact stamps `available_at = the 8-K Item 2.02 acceptance/release timestamp` (the flash), never the 10-Q date and never `now()`. As-of readers gate `available_at ≤ as_of_ts`; a preliminary fact is **invisible before its release**. The final 10-Q vintage keeps its own later `available_at`; no lookahead in either direction.
- **(B)** Migrations **0121–0123** only. `0121` `press_release_facts` (+ `table_catalog`/`field_catalog`); `0122` the preliminary→final reconciliation surface + `pdate`/`rdq` populate path; `0123` `ADD COLUMN IF NOT EXISTS basis` on `est_actual` + basis catalog rows. Split schema-vs-index per the S5g/S5k WAL precedent; timestamped DB+WAL backup before any live apply; never edit a landed migration.
- **(C)** All tests run against in-memory / template-copy DuckDB with hand-built fixture 8-K EX-99 text (a preliminary release at day 0, a matching 10-Q at day +30, one GAAP vs street EPS pair). No SEC/EDGAR network in pytest; the live corpus is an injectable `source_file`; live smoke is operator-run and recorded in the ledger.
- **(D)** The extraction transform is pure (fixture text in → long DataFrame out), unit-tested independent of DuckDB, and records `input_codes_json`/evidence lineage. Same corpus + same run params → same preliminary rows, same basis tags, same reconciliation. Conservative by construction: below `min_confidence`, emit nothing rather than a guess.

---

## Tasks

### S8-0 — 8-K Item 2.02 EX-99 preliminary-fact ingestion *(the big one)*

**Root cause:** `est_actual` reads only `sec_company_facts` (XBRL), so the EX-99.1 press-release figures — text/HTML, non-XBRL — never enter the warehouse and the market-observed announcement-day number is unrecoverable. `est_surprise`'s `rn=1` therefore anchors on the 10-Q filing, not the flash.

**Fix:** NEW `db/press_release.py` with a `PressReleaseDataset` that ingests an **injectable** 8-K Item 2.02 EX-99 corpus, mirroring `EstimateGuidanceOptions` (`source_file` / `fetch` / `parse` / `min_confidence` / `run_id`, `source="press_release_injectable"`). Conservatively extract preliminary **revenue / EPS / operating income / net income** (reuse `GUIDANCE_VALUE_RE`, `_guidance_scale`, `_guidance_confidence`; add `_preliminary_*` extractors) with per-fact `extraction_confidence`, `evidence_text`, and `source_file_sha256`. Resolve `source_item` via the existing `"8-K_2.02"` / `"8-K_2.02_7.01"` cue logic. Emit PRELIMINARY facts into `press_release_facts` (**migration 0121**: `security_id, measure_code, fiscal_year, fiscal_period, period_end, value, unit, basis, extraction_confidence, evidence_text, source_file_sha256, source_item, accession_number, available_at, as_of_date, is_preliminary, run_id, source`) with **`available_at` = the 8-K release timestamp** and `is_preliminary = TRUE`.

**PIT:** (A) `available_at` = 8-K acceptance/release; a preliminary fact is invisible before it. (C) offline fixture EX-99 text. (D) pure extractor; sub-`min_confidence` ⇒ no row.

**Accept-with-fixture:** a fixture Item 2.02 EX-99 releasing "Q3 revenue of $X and diluted EPS of $Y" yields a preliminary revenue and EPS row at confidence ≥ `min_confidence`, each carrying `evidence_text` + `source_file_sha256`; an as-of read at `release_ts − 1s` returns **zero** press-release facts (no lookahead).

### S8-1 — preliminary→final overwrite with vintage retention + RDQ populate

**Root cause:** `est_actual` uses `INSERT OR REPLACE INTO est_actual (...)` on its natural key, so a later 10-Q GAAP row would clobber the preliminary value with no trace, and `refresh_fundamental_periods` derives `rdq`/`pdate` from filing dates only — the press-release flash never lands even though the `pdate` (preliminary) / `fdate` (final) columns exist for exactly this Compustat model.

**Fix:** in `press_release.py`, a preliminary→final reconciliation surface (**migration 0122**, e.g. `press_release_reconciliation`) that joins each `press_release_facts` preliminary row to its final `sec_company_facts`/`est_actual` counterpart and **retains BOTH vintages** — the preliminary vintage ties to PF2-S4's `as_first_reported` (`pit_snapshot.py`), the final carries `is_latest_revision = TRUE`. Populate `fundamental_periods.pdate` = the preliminary release date and set `rdq` to that flash when it precedes the filing-derived `rdq` (via an additive reconciliation write, **not** by editing `refresh_fundamental_periods`' SQL). Never mutate the preliminary `est_actual`/`press_release_facts` row on overwrite.

**PIT:** (A) preliminary vintage keeps its earlier `available_at`; final keeps its later one; `est_surprise`'s `rn=1` now selects the preliminary for free. (B) 0122 catalogued.

**Accept-with-fixture:** preliminary (8-K, day 0) + final (10-Q, day +30) for one period ⇒ reconciliation stores both vintages, `pdate`/`rdq` populated to day 0, and after the "final overwrite" both the preliminary row and its `available_at` still exist (vintage preserved).

### S8-2 — GAAP-vs-street EPS basis tag + basis-correct surprise

**Root cause:** `est_actual` has no `basis` column (unlike `est_guidance`, which already has one via `_guidance_basis`), so `est_surprise` silently subtracts a GAAP actual from a street consensus — a ~5-8% basis mismatch, since IBES chooses street components ex-post.

**Fix:** **migration 0123** `ADD COLUMN IF NOT EXISTS basis` on `est_actual` (catalogued; default `'GAAP'`). Extend `EstimateActualsDataset` to stamp `basis='GAAP'` on companyfacts-sourced rows and to carry `basis='STREET'`/`'NON_GAAP'` where a `press_release_facts` extraction's `_guidance_basis`-style signal marks the figure adjusted/non-GAAP. Make `EstimateSurpriseDataset` **basis-aware**: compute `surprise`/`surprise_pct` only where `actual.basis` matches the consensus basis; on mismatch, emit a `basis_mismatch` flag instead of a silently-wrong number. Backfill `basis='GAAP'` on the existing 1,240 `est_actual` rows.

**PIT:** (D) basis derivation is deterministic from source concept + evidence text; no lookahead — basis is known at first report.

**Accept-with-fixture:** all `est_actual` rows carry a non-null `basis`; a GAAP-actual × street-consensus fixture yields a `basis_mismatch` (no `surprise_pct`), while a matched-basis pair computes `surprise` normally.

### S8-3 — preliminary-capture coverage + gated quality check

**Root cause:** nothing measures how many periods got a preliminary capture before their 10-Q/K, and nothing guards that a preliminary fact never leaks before its release `available_at`, that a final overwrite never drops the preliminary vintage, or that every EPS carries a basis.

**Fix:** a coverage reader (share of `fundamental_periods` whose period has a `press_release_facts` row with `available_at < fdate`) plus a **gated** quality check (clause G) that fails if (a) any `press_release_facts` row is visible before its release `available_at`, (b) any preliminary vintage was lost on overwrite, or (c) any `est_actual` EPS lacks a `basis`. Wire counts into the `quality_check` emission + `DatasetLoadResult` details so PF2-S10's orchestrator and `PARITY_GAP.md` surface them.

**PIT:** (A) coverage computed as-of, no lookahead. (C) fixtures with a planted leak / lost vintage / missing basis. (G) the guard is severity-tagged and gate-wired.

**Accept-with-fixture:** coverage reported on the ~1yr slice; the gated check is **red** on each of the three planted-defect fixtures and **green** on the live proof slice.

---

## Sequencing & expected compounding

**S8-0 → S8-1 → S8-2 → S8-3.** S8-0 (ingestion) is load-bearing — without preliminary facts there is nothing to overwrite, tag, or cover. S8-1 (reconciliation + `rdq`/`pdate`) needs the preliminary rows S8-0 emits and the PF2-S4 vintage surface. S8-2 (basis) is independent of the overwrite but stamps the same rows, so it slots third. S8-3 (coverage + gate) records the outcome of all three. The compounding: once a preliminary fact exists with an earlier `available_at`, `est_surprise`'s existing `rn=1` selection **automatically** flips first-reported to announcement day — the SUE surface becomes a true event-study input at zero extra math — and the basis tag turns the surprise from "silently ~5-8% wrong when mixed" into "basis-correct or explicitly flagged."

## Risks / guardrails

- **Extraction false positives.** Text parsing can misread a press release. Mitigate with a conservative `min_confidence`, mandatory `evidence_text` + `source_file_sha256` per fact, and emit-nothing-below-threshold — never a guess. A preliminary fact is only as trustworthy as its recorded evidence.
- **Never lose the preliminary vintage.** The whole point is reconstructing the announced number; the final-overwrite path must be *additive* (retain both vintages, tie preliminary to PF2-S4 `as_first_reported`), never an in-place `UPDATE`/`REPLACE` of the preliminary row.
- **No lookahead on the flash.** `available_at` = 8-K release, strictly earlier than the 10-Q; the as-of gate must hide a preliminary fact before its release. A basis is known at first report, never restamped.
- **Basis mislabeling is worse than silence.** If the source signal is ambiguous, default `est_actual` to `GAAP` (its true origin) and flag surprise as `basis_mismatch` rather than fabricate a `STREET` tag.
- **Blast radius / parallelism.** Land everything behind NEW `press_release.py` + additive `estimates.py`; `basis` is `ADD COLUMN IF NOT EXISTS`; the `rdq`/`pdate` populate is a reconciliation write, not a `refresh_fundamental_periods` rewrite. Disjoint-module PARALLEL-SAFE with S5→S6/S7; sequential-only against any other `estimates.py` toucher. Stay strictly within **0121–0123**.

## Bench / acceptance

- `press_release_facts` populated from an injectable 8-K Item 2.02 EX-99 fixture corpus, each fact carrying confidence + `evidence_text` + `source_file_sha256` and `available_at` = release time.
- Preliminary→final reconciliation retains both vintages; `fundamental_periods.pdate`/`rdq` populated to the flash on the fixture; no preliminary vintage lost on overwrite.
- `est_actual` carries `basis` on all 1,240 rows; `est_surprise` computes on matched basis and flags `basis_mismatch` otherwise.
- No preliminary fact visible before its release `available_at` (as-of gate proven).
- `python -m pytest atx-impl\db\tests\test_press_release.py -q` green, and the full `python -m pytest atx-impl\db\tests -q` suite green before commit.
- **Live smoke** recorded in the ledger: `press_release_facts` row count on the ~1yr proof slice, `est_actual` rows gaining `basis` (1,240), `est_guidance` 0→N on the injected corpus, count of periods with a pre-10-Q preliminary capture, and the `run_id`.
- **`PARITY_GAP.md`** status updated (preliminary/press-release + estimate-basis axis) and a **`WAREHOUSE_PARITY_TRANCHES.md`** row appended (start/end SHA, domains, verification commands, live-DB smoke with exact counts + run_id, caveats/next).

**Process:** never `git add -A` (stage explicit paths); never push unless asked. New module ⇒ new `test_*.py`. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
