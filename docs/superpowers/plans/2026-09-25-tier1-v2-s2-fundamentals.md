# atx-db Tier-1 v2 — Sprint 2: Fundamentals Materialization, PIT Correctness, Vintages, Fundamental Characteristics

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan
> task-by-task. Steps use checkbox (`- [ ]`) syntax. **No TDD** (user directive): acceptance is a real-data
> measurement; tests only where the index §5.4 allows.

**Goal:** Materialize the fundamentals ladder (statement points → standardized → derived metrics) on the real
warehouse inside the memory doctrine, remove the known look-ahead and wall-clock defects from fundamentals readers and
routing, add the vintage layer that makes atx-db a Compustat-Snapshot analog, prove standardization quality against
SEC FSDS at scale, and compute the first research wave of new accounting characteristics.

**Architecture:** One generic `bucketed_publication_stage` turns every one-transaction fundamentals stage into a
ledgered batch stage keyed by `hash(security_id) % K` that writes clustered `__next` tables and publishes by swap.
Derived metrics are evaluated set-based per bucket of ~400 securities instead of per security × per metric. A vintage
module exposes `first_reported`, `as_of(D)` and restatement events. The B1 run ends with a fundamentals lake snapshot
(Parquet), from which research builds a bitemporal `item_vintages` table and computes characteristics by per-year
as-of selection.

**Tech Stack:** Python, DuckDB 1.5.5, PyArrow, `batch_runner` (1.3), research store (1.9).

**Spec:** `docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md` (§3, §4, §5; rulings R-1, R-4, R-5, R-6, R-12,
R-13). Nodes 2.1–2.11.

## Global Constraints

Every task includes the index §3, §4 (M1–M8) and §5. Migration numbers in this sprint: **0330** (vintages, 2.6).
Paths are relative to `C:\atx\atx-db\` unless they start with `.superpowers/` or `docs/`. Commit trailer
`Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.

## Review Focus

1. **Restated firms vanish from history.** As-of readers that filter on today's `is_latest_revision` drop a firm at
   every cutoff between its original number and a later restatement. Expected: the original row is returned.
2. **Window functions crossing buckets.** A bucketed stage is only correct if every window/aggregate partition key
   contains `security_id` (or the bucket key). Expected: each stage asserts this and a 200-issuer single-pass
   comparison is exact.
3. **Hindsight routing.** Industry templates and fiscal-year-end fallbacks chosen from the *latest* state and applied
   to all history (bank/insurer/REIT/utility; FYE changes). Expected: routing valid at `period_end` and known at filing.
4. **Newest-NULL revival.** DuckDB `arg_max(value, clock)` skips NULL values, reviving an older value when the newest
   revision is NULL; paired `arg_max` calls can cite different rows. Expected: `arg_max(struct_pack(...))` /
   `arg_max_null` everywhere.
5. **Survivor-conditioned coverage claims.** Coverage computed over current filers only. Expected: coverage tables
   include delisted issuers and report by fiscal year and size group.

---

### Task 1: [2.1] Bucketed fundamentals publication (statement points → calendarization)

**Lane:** F · **Tokens:** ACT (stage wiring) · **Depends on:** 1.3 · **Peak target:** 1.0 GiB

**Files:**
- Modify: `src/atx_db/_fundamental_publication.py:170-269` (add the bucketed stage; keep the old function for tests)
- Modify: `src/atx_db/fundamentals.py:748-958` (revisions), `src/atx_db/fundamental_statements.py:1011-1290`,
  `:1321-1342` (periods), `:1654-1677` (ttm), `src/atx_db/calendarization.py`
- Modify: `src/atx_db/activation.py:976-1053` (stages call the batch stages)
- Modify: `scripts/refresh_reconciliation_sharded.py:62-70` (shard by security bucket, not non-NULL symbol)

**Interfaces:**
- Consumes: `BatchStage`, `run_slice`, `register_stage` (1.3).
- Produces:
  ```python
  @dataclass(frozen=True)
  class BucketedTable:
      name: str                          # live table, e.g. "fundamental_revisions"
      order_by: tuple[str, ...]          # physical clustering inside each bucket, e.g. ("security_id", "period_end")
      natural_key: tuple[str, ...]       # checked per bucket and at finalize

  def bucketed_publication_stage(
      *, stage_name: str, tables: Sequence[BucketedTable], bucket_count: int,
      build_bucket_sql: Callable[[int, int], Sequence[str]],   # (bucket, K) -> INSERT ... INTO <t>__next ... WHERE hash(security_id) % K = bucket ORDER BY <order_by>
      partition_keys_asserted: Sequence[str] = ("security_id",),
      carry_over_sql: Mapping[str, str] | None = None,         # rows outside the replace scope, copied in bucketed pages
      memory_limit: str = "384MB", threads: int = 2,
  ) -> BatchStage: ...
  ```
  Registered stages: `statement_points_rebuild` (revisions, statement points, shares history), `periods_rebuild`,
  `ttm_rebuild`, `calendarization_rebuild`, each K = 32 (≈ 1.8M revisions per bucket at ~57M facts). Standardized is
  wired by 2.5 (STD owns its SQL) using the same API; reconciliation shards use the same bucket function.
- Finalize per stage: 0 duplicate natural keys (per bucket during build, globally by `GROUP BY … HAVING count(*) > 1`
  at finalize), FC1 floor (0 rows with `available_at` < filing date + 46 h), swap, `CHECKPOINT`, no-WAL assertion. The
  old full-table sort CTAS (`_fundamental_publication.py:103-107`) and the VARCHAR-PK keyset shadow copy (`:118-162`) are
  not used by the bucketed path.

- [ ] **Step 1:** Implement `bucketed_publication_stage`; port each stage's INSERT…SELECT into a bucket-restricted
  form; assert in code that every window partition list includes `security_id`.
- [ ] **Step 2 (equivalence):** on a scratch DuckDB built from the 1.4 Parquet batches for 200 issuers (stratified by
  filing count), run the old single-pass path and the bucketed path; `EXCEPT ALL` both ways = 0 for every output table.
- [ ] **Step 3 (rehearsal, real):** on a prod copy after 1.8 (or a scratch DB with 10% of CIKs), run each stage through
  `run_slices.py --until-complete --job-gb 1.0`; record seconds and peak per bucket (target ≤ 60 s, ≤ 1.0 GiB).
- [ ] **Step 4:** Commit.

**Acceptance:** equivalence exact; every bucket ≤ 1.0 GiB; per-stage resumable after a deliberate kill.

---

### Task 2: [2.2] PIT reader fixes — no `is_latest_revision` in as-of SQL, NULL-safe arg_max, NULL clocks, tie-breaks, entry clock

**Lane:** F · **Tokens:** — · **Depends on:** 0.15, 1.11 (`calendar.decision_cutoff_utc`) · **Peak target:** 0.3 GiB

**Files:**
- Modify: `src/atx_db/asof/metrics.py` (lines ~59, 258, 309, 361, 416, 432, 448, 466, 624, 675, 727, 778, 824),
  `src/atx_db/asof/pricing.py:331` (`UNIVERSE_MEMBERSHIP_ASOF_SQL`), `src/atx_db/asof/_common.py:19-20`,
  `src/atx_db/asof/fundamentals.py:25-28, 36, 67, 98, 128, 159`, `src/atx_db/asof/identifiers.py`, `asof/security.py`,
  `asof/ownership.py`, `asof/estimates.py`
- Modify: `src/atx_db/signal_eval.py:1360-1371`
- Modify: `src/atx_db/derived_compatibility.py:89-90, 118-119, 184-185`, `src/atx_db/fundamental_signals.py:142-240`,
  `src/atx_db/piotroski.py:139-140`, `src/atx_db/quarterly_*.py`, `src/atx_db/cash_profitability.py:133-137, 191-192`,
  `src/atx_db/earnings_surprise.py:150-152`, `src/atx_db/noa_proxy_turnover_change.py`
- Create: `tests/test_asof_no_latest_revision.py` (one static guard test)

**Interfaces:**
- Produces in `asof/_common.py`:
  ```python
  def latest_visible_sql(table: str, key_cols: Sequence[str], cutoff_param: str = "$cutoff",
                         order_cols: Sequence[str] = ("available_at", "source_loaded_at", "id")) -> str:
      """SELECT * FROM (SELECT *, row_number() OVER (PARTITION BY <key> ORDER BY <order> DESC) AS rn
         FROM <table> WHERE available_at IS NOT NULL AND available_at <= <cutoff>) WHERE rn = 1"""
  ```
  and `decision_cutoff(d)` delegating to `calendar.decision_cutoff_utc` (22:00 UTC; replaces the 23:59:59 default).
- Rules: (a) no `is_latest_revision` predicate inside any as-of SQL (the month-end ratio reader
  `FUNDAMENTALS_ASOF_MONTH_SQL`, `asof/metrics.py:62-89`, is the reference pattern); (b) `available_at IS NULL` rows are
  invisible (no `OR available_at IS NULL`); (c) `FUNDAMENTALS_ASOF_SQL` ties break on `available_at`, accession, id;
  (d) value/lineage picks use `arg_max(struct_pack(value := v, id := id, ...), clock)` as `market_daily.py:500-504` does;
  (e) `signal_eval`'s survivorship-safe target enters at the next session (no label with the same anchor as a feature
  clock later than that day's close).

- [ ] **Step 1:** Apply (a)–(e); `git grep -n "is_latest_revision" src/atx_db/asof src/atx_db/signal_eval.py` must return
  only writer-side code.
- [ ] **Step 2:** Static guard test: import every `*_SQL` constant in `asof/` and assert none contains
  `is_latest_revision` or `available_at IS NULL OR`.
- [ ] **Step 3 (retained-data probe):** on the FSDS subset DB (P12), pick 10 issuers with a 10-K/A or 10-Q/A; show that
  `market_cap_asof`-style readers return the original row at a cutoff between the original and the amendment (the full
  20-issuer real check runs in 2.8).
- [ ] **Step 4:** Commit.

**Acceptance:** grep clean; guard test passes; probe shows 0 holes on the 10 issuers.

---

### Task 3: [2.3] Period-valid routing and wall-clock removal in fundamentals ingest and derivation

**Lane:** F · **Tokens:** — · **Depends on:** 2.1 · **Peak target:** 0.4 GiB

**Files:**
- Modify: `src/atx_db/fundamental_statements.py:669-670, 1099-1100`, `src/atx_db/industry_templates.py:311-331, 424,
  458-467`, `src/atx_db/security_master.py:357, 368`, `src/atx_db/calendarization.py:437-445`,
  `src/atx_db/ticker_history_bulk.py:175-201` (explicit `as_of_date` instead of `current_date`),
  `src/atx_db/openfigi_signals.py:211` (receipt clock instead of `now()`)

**Interfaces:**
- Every function that read `current_date`/`now()`/`date.today()` takes `as_of_date: date` (default: the pinned
  snapshot date 2026-09-20 from configuration, never the wall clock).
- Industry template for a period = the template valid at `period_end` and known at the filing's clock (SIC as of the
  filing, from submissions history where available; otherwise the current SIC labeled `current_sic_snapshot`).
- `issuer_fyr` for an unlabeled period = the fiscal year end of the latest annual period **known by that filing**.

- [ ] **Step 1:** Implement; list every remaining wall-clock read in these modules in the report with a justification
  (receipt timestamps only).
- [ ] **Step 2 (real):** from the 1.4/1.6 Parquet stages, build statement points → standardized for 200 CIKs twice on a
  scratch DB (once with the process clock set by `ATX_FAKE_TODAY` if such a hook exists, else twice on different runs);
  row hashes identical. Pick ≥ 10 issuers whose SIC or fiscal-year end changed (from submissions `formerNames` /
  `fiscalYearEnd` history) and show per-period routing.
- [ ] **Step 3:** Commit.

**Acceptance:** identical hashes; routing table for the ≥ 10 change issuers in the report.

---

### Task 4: [2.4] Derived metrics set-based per bucket, with resume and session-calendar windows

**Lane:** F · **Tokens:** DMD, ACT (stage wiring) · **Depends on:** 2.1 · **Peak target:** 1.0 GiB

**Files:**
- Modify: `src/atx_db/derived_metrics.py:219-357`, `src/atx_db/_derived_pit.py`, `src/atx_db/_derived_annual.py`,
  `src/atx_db/derived_dsl.py:412-425`
- Modify: `src/atx_db/activation.py` (derived stage → batch stage `derived_rebuild`)

**Interfaces:**
- Produces batch stage `derived_rebuild`: batches = `hash(security_id) % 33` (~400 securities each); per batch, every
  metric definition is evaluated set-based over the bucket (213 definitions × 33 buckets instead of 213 × 13k × ~6
  statements) into `derived_metric_values__next`, clustered by `(security_id, metric_code, as_of_date)`; finalize =
  uniqueness + swap.
- DSL daily windows (`tret`, `lag` on market columns) use XNYS session windows with the P2 literature minimums
  (≥ 15 of 21, ≥ 45 of 60, ≥ 200 of 252 observed) instead of row counts.

- [ ] **Step 1:** Implement the set-based evaluator (reuse the DSL's SQL lowering; replace the per-security loop).
- [ ] **Step 2 (equivalence):** on 200 securities, per-security path vs bucket path: `EXCEPT ALL` both ways = 0 for
  fundamental windows (`q`, `ttm`, `avg2`); daily windows differ only where the session rule changes them (report counts).
- [ ] **Step 3:** Commit.

**Acceptance:** equivalence as stated; projected full run < 2 h at ≤ 1.0 GiB (measured on the rehearsal DB and
extrapolated in the report).

---

### Task 5: [2.5] Standardization depth — security-id scoping, year-aware fallback chains, EBIT and JKP book equity, templates

**Lane:** F · **Tokens:** STD · **Depends on:** 0.3, 2.1 · **Peak target:** 0.4 GiB

**Files:**
- Modify: `src/atx_db/_standardization_set_based.py:1734-1860` (`_std_symbol_filter` at :1768, :1822 →
  `_std_security_filter`; stage via `bucketed_publication_stage`), `src/atx_db/standardization.py`
- Modify: `src/atx_db/seeds/concept_map.csv`, `statement_map.csv`, `fundamental_items.csv`, `standardization_rules.csv`
- Modify: `docs/measurements/FSDS_BENCHMARK.md`

**Interfaces:**
- Standardized scoping is by `security_id` (archive facts carry `symbol = NULL`, so symbol scoping can drop CIK-only
  issuers).
- New or extended items (rule chains ordered; each chain step keeps its `rule_id` so coverage per step is measurable):

| Item | Chain (first available wins, per period) |
|---|---|
| `revenue` | `Revenues` → `RevenueFromContractWithCustomerExcludingAssessedTax` → `RevenueFromContractWithCustomerIncludingAssessedTax` → `SalesRevenueNet` (pre-2018) → utility/bank/insurer template items |
| `total_liabilities` | `Liabilities` → `LiabilitiesAndStockholdersEquity` − `StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest` − temporary equity (flag `derived_liabilities`) |
| `ebit_best` | `operating_income` → `ebit__1017` → `pretax_income + interest_expense_total` (flag which step) |
| `book_equity_jkp` | SEQ + TXDITC − PSTK with TXDITC, PSTK = 0 if missing; SEQ ← `stockholders_equity` → `common_equity + preferred_stock` → `total_assets − total_liabilities` |
| `operating_lease_liabilities` | `OperatingLeaseLiability` → `OperatingLeaseLiabilityCurrent + OperatingLeaseLiabilityNoncurrent` (kept out of debt; ASC 842 break from 2019) |
| `cash_taxes_paid` | `IncomeTaxesPaidNet` → `IncomeTaxesPaid` |
| `cash_interest_paid` | `InterestPaidNet` → `InterestPaid` |
| `impairments` | `AssetImpairmentCharges` → `GoodwillImpairmentLoss + ImpairmentOfIntangibleAssetsExcludingGoodwill` |
| `restructuring` | `RestructuringCharges` → `RestructuringCostsAndAssetImpairmentCharges` |

- [ ] **Step 1:** Implement scoping + chains; bank/insurer/REIT/utility items route through 2.3's period-valid template.
- [ ] **Step 2 (benchmark):** re-run the P12 50-filer FSDS benchmark; report operating income, gross profit, capex,
  common equity, total liabilities coverage vs the 58% / 52% / 62% / 87% baselines.
- [ ] **Step 3 (digest):** rules not touched keep byte-identical outputs on the FSDS subset DB.
- [ ] **Step 4:** Run the seed tests once (template rebuild ~370 MB, alone); commit.

**Acceptance:** 50-filer mapped ≥ 90%; operating income, gross profit and capex each ≥ 80%; digest proof clean.

---

### Task 6: [2.6] Vintage layer — first-reported, as-of(D) and restatement events (migration 0330)

**Lane:** F · **Tokens:** MIG (0330) · **Depends on:** 2.1 · **Peak target:** 0.8 GiB

**Files:**
- Create: `src/atx_db/fundamental_vintages.py`, `src/atx_db/migrations/bodies_0330.py`
- Modify: `src/atx_db/migrations/registry.py` (MIG), `src/atx_db/schema.py`

**Interfaces:**
- 0330 creates (no PK, no `DEFAULT now()`):
  ```sql
  CREATE TABLE fundamental_restatement_events (
      cik VARCHAR NOT NULL, security_id VARCHAR, item_code VARCHAR NOT NULL, period_type VARCHAR NOT NULL,
      period_start DATE, period_end DATE NOT NULL, unit VARCHAR NOT NULL,
      prior_value DOUBLE, new_value DOUBLE, rel_change DOUBLE, sign_flip BOOLEAN NOT NULL,
      prior_accession VARCHAR NOT NULL, new_accession VARCHAR NOT NULL, new_form VARCHAR NOT NULL,
      restatement_kind VARCHAR NOT NULL,     -- 'amendment' (10-K/A, 10-Q/A) | 'comparative' (later periodic filing) | 'other'
      non_reliance_8k_accession VARCHAR,     -- 8-K Item 4.02 within [-120, +120] days, from submissions items
      available_at TIMESTAMP NOT NULL, created_at TIMESTAMP NOT NULL);
  ```
  and views `v_fundamentals_first_reported` (value from the minimum-clock revision per item-period) and
  `v_fundamentals_latest`, plus `fundamentals_as_of_sql(cutoff) -> str` in `fundamental_vintages.py`.
- A restatement event = a later revision of the same `(cik, item_code, period_type, period_start, period_end, unit)`
  whose value differs by > 0.5% relative or flips sign.
- Built by batch stage `restatement_events_rebuild` (K = 16 CIK-hash buckets) from the revisions table.

- [ ] **Step 1:** Implement the stage and views; register 0330 (template rebuild once).
- [ ] **Step 2 (rehearsal):** on the rehearsal DB, count events by year and kind; list 10 sample events with both
  accessions for manual verification.
- [ ] **Step 3:** Commit.

**Acceptance:** stage bounded (≤ 0.8 GiB, resumable); sample events verified; revisions-per-firm-period distribution
reported (Compustat benchmark: ≥ 5 on average).

---

### Task 7: [2.7] B1 heavy run — fundamentals ladder on the real warehouse, then the fundamentals lake snapshot

**Lane:** OPS · **Tokens:** HEAVY, ACT (invocation) · **Depends on:** 1.8, 2.2, 2.3, 2.4, 2.5, 2.6 · **Peak:** ≤ 1.0 GiB/slice

**Files:**
- Create: `.superpowers/sdd/tier1-v2/receipts/b1-*.jsonl`, `.superpowers/sdd/tier1-v2/receipts/b1-summary.md`
- Create (data): `data/research/lake/fund_<UTC date>/…` via `export_lake_snapshot` (1.9)

- [ ] **Step 1:** Preconditions: pinned export; no WAL; backup `warehouse.duckdb.pre-b1.<UTC>.bak`; migrate to 0330.
- [ ] **Step 2:** For each stage in order, `run_slices.py --until-complete` then `--finalize`: `statement_points_rebuild`
  → `periods_rebuild` → `ttm_rebuild` → `calendarization_rebuild` → `standardized_rebuild` → reconciliation (security
  buckets) → `derived_rebuild` → `item_coverage` → `restatement_events_rebuild`.
- [ ] **Step 3:** Export the fundamentals lake snapshot as a short read-only slice (job 0.8): `fundamental_standardized`,
  revisions (item level), statement periods, `derived_metric_values` (fundamental windows), `fundamental_restatement_events`,
  `entity_classification`, `sec_submissions` columns (cik, accession, form, filing date, acceptance datetime, items),
  item coverage — partitioned by fiscal or filing year.
- [ ] **Step 4:** `b1-summary.md` with row counts, admission stats, peaks, kills and recoveries.

**Acceptance:** all stages published; `fundamental_standardized` and `derived_metric_values` non-empty; distinct valid
latest derived codes ≥ 90% of seeded definitions; lake snapshot manifest written with its sha.

---

### Task 8: [2.8] Fundamentals QA on real data — FSDS 10k-cell benchmark, identities, restatement holes, item gate, coverage report

**Lane:** F · **Tokens:** STD · **Depends on:** 2.7 · **Peak target:** 0.8 GiB (reads the lake snapshot, not prod)

**Files:**
- Create: `scripts/fundamentals_qa.py`, `docs/measurements/FUNDAMENTALS_QA_<date>.md`
- Modify: `docs/measurements/FSDS_BENCHMARK.md`, `docs/ITEM_COVERAGE.md`

- [ ] **Step 1 (FSDS 10k cells):** stratify issuer-periods from the 8 retained FSDS quarters (all quarters if U3/X.4 is
  done) by size (total-assets decile), template (industrial/bank/insurer/REIT/utility) and year; sample ~400
  issuer-periods × ~25 core items (AT, ACT, LCT, CHE, RECT, INVT, PPENT, GDWL, LT, DLC, DLTT, SEQ, SALE, COGS, XSGA, XRD,
  OIADP, XINT, PI, TXT, NI, OANCF, CAPX, DVC, EPS diluted). Compare warehouse standardized vs FSDS face value.
- [ ] **Step 2 (identities):** A = L + E within 0.5%; CA ≤ A; TTM = sum of 4 quarters vs FY−1 + YTD − YTD−1; Q4 = FY −
  9M sign flips; DEI shares vs balance-sheet shares ratio ≈ 1000 (scale errors); DEI shares older than 400 days
  (staleness); negative values on elements that should not be negative (XBRL US DQC_0015 list).
- [ ] **Step 3 (restatement holes, real check of 2.2):** 20 issuers with 10-K/A or 10-Q/A: every as-of reader returns the
  original row at a cutoff between original and restatement.
- [ ] **Step 4 (item gate):** measure ITEM_COVERAGE (110 annual items ≥ 90% coverage, top 3000, FY2015+); publish.
- [ ] **Step 5 (coverage report):** per item × fiscal year × size group (total-assets deciles until 3.8 provides market
  size groups), including delisted issuers; coverage per fallback-chain step.
- [ ] **Step 6:** Commit script and docs.

**Acceptance:** FSDS ≥ 95% of cells mapped and ≥ 98% of mapped cells within 0.5% (failures itemized by item and rule);
A = L + E on ≥ 99% of issuer-periods; 0 restatement holes; item gate result published whatever it is.

---

### Task 9: [2.9] Research item vintages and the Compustat-analog item map

**Lane:** R · **Tokens:** — · **Depends on:** 1.9, 2.7 · **Peak target:** 0.6 GiB

**Files:**
- Create: `src/atx_db/research/items_map.py`, `src/atx_db/research/item_vintages.py`

**Interfaces:**
- `items_map.py`: `COMPUSTAT_ANALOG: dict[str, ItemChain]` mapping mnemonics to atx canonical items (chains in order):
  AT, ACT, LCT, CHE, IVST, RECT, INVT, XPP (prepaid), PPENT, PPEGT, INTAN, GDWL, IVAO (long-term investments), LT, DLC,
  DLTT, TXDITC, PSTK, SEQ, CEQ, MIB, BE (= `book_equity_jkp`), AP, XACC (accrued liabilities), DRC (deferred revenue),
  SALE, COGS, XSGA, XRD, XAD, DP, OIADP (= `ebit_best`), XINT, PI, TXT, IB, NI, OANCF, IVNCF, FINCF, CAPX, AQC
  (acquisitions), DVC, PRSTKC, SSTK, DLTIS, DLTR, DLCCH, TXPD, CSHO, EPSPI, EPSFI. Missing R&D is 0 (JKP convention,
  flagged); other missing items stay NULL.
- `item_vintages.py` produces the lake dataset `item_vintages` (partition `fiscal_year`):
  `owner_id, cik, item, freq ('q'|'ytd'|'fy'|'ttm'|'instant'), period_start, period_end, fiscal_year, fiscal_qtr,
  value, available_at, valid_until, accession, rule_id` — one row per vintage; `valid_until` = the next vintage's
  `available_at`.
- `items_asof(formations: Sequence[date], items: Sequence[str], lags: Mapping[str, Sequence[int]]) -> Iterator[pa.RecordBatch]`
  — per formation-year, an ASOF selection (`available_at <= decision_cutoff(eom)`, newest vintage wins even when NULL)
  returning wide rows `(eom, owner_id, <item>_<freq>_l<k>, …)`; one year resident at a time.

- [ ] **Step 1:** Build `item_vintages` from the 2.7 lake snapshot; implement `items_asof`.
- [ ] **Step 2 (check):** for 50 owners × 12 formations, `items_asof` equals the warehouse as-of reader output (from the
  snapshot) exactly.
- [ ] **Step 3:** Commit.

**Acceptance:** exact match; `items_asof` for one year of formations ≤ 0.6 GiB; dataset size and row count reported.

---

### Task 10: [2.10] Fundamental characteristic wave A (accounting-only) + composition definitions

**Lane:** R · **Tokens:** CAT · **Depends on:** 2.9, 1.10 · **Peak target:** 0.6 GiB per year pass

**Files:**
- Create: `src/atx_db/research/fundamental_chars.py`
- Modify: `src/atx_db/seeds/research_anomaly_catalog.csv` (wave `w2_fund_a` rows; CAT)

**Interfaces:**
- Owner-level features written to the feature store with `basis = owner` (joined to lines in 3.8). Annual items use
  the latest FY known at the formation; "Δ" is versus the same item 12 months earlier (4 quarters for quarterly
  items); scaled by lagged AT (`AT_l1y`) unless stated; growth only when the denominator is positive.

| feature_id | Formula (Compustat-analog items) | Sign | Theme | Population |
|---|---|---|---|---|
| `ppeinv_gr1a` | (ΔPPEGT + ΔINVT) / AT_l1y | − | investment | all |
| `coa_gr1a` | Δ(ACT − CHE) / AT_l1y | − | accruals | classified BS |
| `col_gr1a` | Δ(LCT − DLC) / AT_l1y | + | accruals | classified BS |
| `cowc_gr1a` | Δ[(ACT − CHE) − (LCT − DLC)] / AT_l1y | − | accruals | classified BS |
| `ncoa_gr1a` | Δ(AT − ACT − IVAO) / AT_l1y | − | investment | classified BS |
| `ncol_gr1a` | Δ(LT − LCT − DLTT) / AT_l1y | + | debt_issuance | classified BS |
| `nncoa_gr1a` | ncoa_gr1a − ncol_gr1a | − | investment | classified BS |
| `fnl_gr1a` | Δ(DLTT + DLC + PSTK) / AT_l1y | − | debt_issuance | all |
| `nfna_gr1a` | Δ[(IVST + IVAO) − (DLTT + DLC + PSTK)] / AT_l1y | + | debt_issuance | all |
| `lti_gr1a` | ΔIVAO / AT_l1y | − | investment | all |
| `taccruals_at` | cowc_gr1a + nncoa_gr1a + nfna_gr1a (Richardson et al. 2005) | − | accruals | classified BS |
| `pct_tacc` | (NI − OANCF − IVNCF) / |NI| (Hafzalla et al.) | − | accruals | NI ≠ 0 |
| `be_gr1a` | ΔBE / AT_l1y | − | investment | all |
| `inv_gr1` | INVT / INVT_l1y − 1 | − | investment | inventory_holders |
| `inv_gr1a` | ΔINVT / AT_l1y | − | investment | inventory_holders |
| `drc_gr1a` | ΔDRC / AT_l1y (Prakash–Sinha) | + | accruals | deferred_revenue_reporters |
| `capex_abn` | (CAPX/SALE) / mean(CAPX/SALE, prior 3 FY) − 1 (Titman–Wei–Xie) | − | investment | all (2013+) |
| `cinvest` | (PPENT_q/SALE_q) − mean of prior 3 quarters | − | investment | all |
| `debt_gr3` | log((DLTT + DLC) / (DLTT + DLC)_l3y) | − | debt_issuance | debt > 0 |
| `lgr` | LT / LT_l1y − 1 | − | debt_issuance | all |
| `at_be` | AT / BE | − | low_leverage | BE > 0 |
| `int_debt` | XINT / (DLTT + DLC) | − | low_leverage | debt > 0 |
| `ocf_debt` | OANCF / (DLTT + DLC) | + | low_leverage | debt > 0 |
| `depr_ppe` | DP / PPENT | + | investment | PPENT > 0 |
| `pchdepr` | Δ(DP / PPENT) | + | investment | PPENT > 0 |
| `tangibility` | PPENT / AT | + | quality | all |
| `aliq_at` | (CHE + 0.75(ACT − CHE) + 0.5(AT − ACT − GDWL − INTAN)) / AT_l1y | + | quality | all |
| `intan_at`, `gdwl_at` | (INTAN + GDWL) / AT; GDWL / AT | + | quality | all |
| `ope_be` | (SALE − COGS − XSGA − XINT) / BE (Fama–French RMW) | + | profitability | BE > 0 |
| `op_at` | (SALE − COGS − XSGA + XRD) / AT (Ball et al. 2015) | + | quality | all |
| `cop_at` | (op_at·AT − ΔRECT − ΔINVT − ΔXPP + ΔDRC + ΔAP + ΔXACC) / AT (Ball et al. 2016) | + | quality | all |
| `ocf_at_chg1` | Δ(OANCF / AT) | + | profit_growth | all |
| `tax_gr1a` | (TXT_q − TXT_q−4) / AT_q−4 | + | profit_growth | all |
| `nincr` | consecutive quarters with IB_q > IB_q−4 (0–8) | + | profit_growth | all |
| `ni_ar1` | AR(1) coefficient of IB/AT over 20 quarters | + | quality | 16 of 20 q |
| `earn_smooth` | σ(IB/AT) / σ(OANCF/AT) over 16 quarters | − | quality | 12 of 16 q |
| `rd_cap_at` | Σ_{k=0..4} (1 − 0.2k)·XRD_{t−k} / AT (2014+) | + | investment | rd_reporters |
| `org_cap_at` | perpetual-inventory SG&A capital (15% depreciation, 5 years) / AT (2014+) | + | quality | all |
| `xad_gr1` | XAD / XAD_l1y − 1 | + | investment | advertising_reporters |
| `mna_at` | AQC / AT_l1y | − | investment | all |
| `sale_noa` | SALE / NOA, NOA = (AT − CHE) − (AT − DLC − DLTT − MIB − PSTK − CEQ) | + | quality | NOA > 0 |
| `lsv_sales_rank` | Lakonishok–Shleifer–Vishny 5-year weighted sales-growth rank (weights 5..1) | − | investment | 2014+ |

- Composition definitions (materialized in 3.11 once `me_company` exists): `be_me_jkp` (BE/ME), `netdebt_me`
  ((DLTT + DLC − CHE)/ME), `bev_mev` ((BE + DLTT + DLC + PSTK + MIB − CHE)/(ME + DLTT + DLC + PSTK + MIB − CHE)),
  `tangible_bm` ((BE − GDWL − INTAN)/ME), `intang_bm` ((BE + rd capital + org capital)/ME), `cash_productivity`
  ((ME + DLTT − AT)/CHE), `xad_me` (XAD/ME), `div12m_me` (P8 cash dividends over 12 months × shares / ME),
  `chcsho_12m` (log split-adjusted shares t−6 vs t−18), `compeqiss` (log(ME_t/ME_t−60) − Σ log(1 + r), 2017+).

- [ ] **Step 1:** Implement per-year passes over `items_asof`; write owner-level features to the store.
- [ ] **Step 2:** Catalog rows for the table (wave `w2_fund_a`, `evidence_class=replication` with the cited sign,
  `population`, `jkp_theme`, `publication_year`) and the composition rows (`wave=w3_compositions`, admitted when 3.11
  materializes them). Record the catalog digest.
- [ ] **Step 3 (real, no returns read):** coverage per feature × year on the lake snapshot.
- [ ] **Step 4:** Commit.

**Acceptance:** each feature ≥ 3,000 owners/month over 2012–2025 (conditional ones ≥ 60% of their population; 3- and
5-year windows from their first feasible year); ≤ 0.6 GiB per year pass.

---

### Task 11: [2.11] Accounting-only scores and the FF49 taxonomy option

**Lane:** R · **Tokens:** CAT, FEAT · **Depends on:** 2.10, 1.13 · **Peak target:** 0.6 GiB

**Files:**
- Create: `src/atx_db/research/scores.py`
- Modify: `src/atx_db/research/features.py` (FEAT: `industry_taxonomy` option `ff12` | `ff49` for the industry-neutral
  variant; PIT classification basis labeled), `src/atx_db/seeds/research_anomaly_catalog.csv` (CAT)

**Interfaces:**

| feature_id | Definition | Sign |
|---|---|---|
| `mohanram_g` | 8 binary signals vs FF49-year medians: ROA, CFO/A, CFO > NI, σ(ROA) 16q, σ(sales growth) 16q, R&D/A, CAPX/A, XAD/A | + |
| `ww_index` | Whited–Wu: −0.091·CF/A − 0.062·div dummy + 0.021·DLTT/A − 0.044·log A + 0.102·ind sales growth − 0.035·sales growth | − |
| `sa_index` | Hadlock–Pierce: −0.737·size + 0.043·size² − 0.040·age (size = log AT in 2004 $ capped at $4.5bn; age = years since first 10-K, capped 37) | − |
| `jones_da` | modified-Jones discretionary accruals from FF49-year cross-sectional regressions (≥ 10 firms) | − |
| `dd_aq` | Dechow–Dichev accrual quality: σ of residuals of ΔWC on CFO_{t−1,t,t+1} over 5 years (2015+) | − |

Market-dependent scores (CHS failure probability, QMJ, Stambaugh–Yuan MGMT/PERF, equity duration, KZ) are defined here
and materialized in 3.11.

- [ ] **Step 1:** Implement scores on `items_asof` output (per formation-year cross-sections; ≤ 0.6 GiB).
- [ ] **Step 2:** FF49 option in `features.py` using `reference_classifications` (FF49 verified against French
  Siccodes49; `classification_basis` carried).
- [ ] **Step 3:** Catalog rows (wave `w2_fund_a` addendum); coverage per year; commit.

**Acceptance:** each score ≥ 2,500 owners/month over its feasible window; component coverage reported.
