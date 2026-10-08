# atx-db Tier-1 v2 — Sprint 3: Market, Identity, Survivorship and Returns Materialization

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan
> task-by-task. Steps use checkbox (`- [ ]`) syntax. **No TDD** (user directive): acceptance is a real-data
> measurement; tests only where the index §5.4 allows.

**Goal:** Materialize the market side of the warehouse — price metrics, market-daily panel, corporate-action
adjustments, a CRSP/CCM-style identity layer, complete delisting terminals and survivorship-safe labels — within the
memory doctrine, prove it on real data, and hand research the final labels, the owner→line spine with company market
equity, and the market-dependent characteristics.

**Architecture:** The remaining one-shot market stages become ledgered batch stages (bucketed time-series passes plus
per-year cross-sectional passes). Identity gains permanent, never-recycled security and company ids, a dated link table
with basis/tier/primary flags and a names history. Delisting evidence uses reconstructed links with validity windows and
EDGAR 8-K items; Shumway imputation needs positive performance evidence. Labels are month-end, next-session entry,
stitched per cessation cluster, and every dropped anchor is counted. The B2/B3 run ends with a market lake snapshot
that research turns into final labels and compositions.

**Tech Stack:** Python, DuckDB 1.5.5, PyArrow, `batch_runner` (1.3), canonical calendar (1.11), research store (1.9).

**Spec:** `docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md` (§3, §4, §5; rulings R-1, R-5, R-6, R-8, R-11,
R-12). Nodes 3.1–3.11.

## Global Constraints

Every task includes the index §3, §4 (M1–M8) and §5. Migration number in this sprint: **0331** (identity layer +
delisting severity; one MIG owner, 3.3 then 3.4 in sequence). Paths are relative to `C:\atx\atx-db\` unless they start
with `.superpowers/` or `docs/`. Commit trailer `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.

## Review Focus

1. **Fabricated losses on acquisition targets.** A delisted merger target with no CIK link is classed `unknown` and gets
   Shumway −30%. Expected: Shumway only with performance evidence; merger/voluntary cessations get last-price policies;
   CIK-less unknowns get a neutral labeled policy with Shumway as a published sensitivity.
2. **Silently dropped windows.** A label window spanning a cessation without a terminal is dropped today and the
   critical DQC still passes. Expected: every cessation-spanning anchor is published or counted and gated.
3. **Preferred and warrant lines priced as common.** Expected: non-common lines never carry an issuer's DEI shares or
   fundamentals; multi-class companies sum ME across **common** classes only.
4. **Mixed adjustment bases.** Picking the newest bar across sources, or reading restated bars without `bars_asof_sql`,
   mixes adjustment bases across dates. Expected: one declared source per line; historical cutoffs unchanged by later
   restatements.
5. **Stray bars on closures.** Bars exist on 2012-10-29/30, 2018-12-05, 2025-01-09. Expected: sessions come from the
   canonical calendar; stray bars are excluded and counted.

---

### Task 1: [3.1] `equity_price_metrics` as a four-pass bucketed stage with a daily incremental mode

**Lane:** K · **Tokens:** — · **Depends on:** 1.3, 1.11 · **Peak target:** 1.0 GiB

**Files:**
- Modify: `src/atx_db/equity_price_metrics.py:302-578`

**Interfaces:**
- Batch stage `price_metrics_rebuild`, passes as separate stages sharing a run key:
  - **A** `price_metrics_a` — per `hash(security_id) % 16` bucket (~2M bars): returns, dollar volume, drawdown and
    per-security trailing windows on XNYS sessions (minimum-observation rules as in 1.12) → `equity_price_metrics_a__next`.
  - **B** `price_metrics_b` — EW market return per `trade_date` (~3.6k rows) from A.
  - **C** `price_metrics_c` — per bucket: beta, correlation, idiosyncratic volatility vs B.
  - **D** `price_metrics_d` — per `trade_date` year (15 batches of ~2.2M rows): the 5 cross-sectional percentile ranks.
  - finalize: assemble `equity_price_metrics__next` per bucket, clustered `(security_id, trade_date)`, uniqueness, swap.
- Daily incremental: `update_price_metrics(conn, trade_date)` recomputes only the new date using a 252-session lookback
  per security (used by 5.3).

- [ ] **Step 1:** Split the 20-window CTAS into the four passes (one blocking window per statement).
- [ ] **Step 2 (equivalence):** 200 securities: new vs old output equal except where session-calendar windows change
  values (report counts by column).
- [ ] **Step 3 (rehearsal):** full run on a prod copy with one kill; record per-batch time and peak.
- [ ] **Step 4:** Commit.

**Acceptance:** rows = bars rows with equal bounds; every batch ≤ 1.0 GiB; resumable.

---

### Task 2: [3.2] `market_daily` ledgered rebuild, adjustment-factor ledger, PIT bar reads, single price source

**Lane:** K · **Tokens:** — · **Depends on:** 1.3, 0.13, 0.14 · **Peak target:** 0.8 GiB

**Files:**
- Modify: `src/atx_db/market_daily.py:500-504, 808-830, 1028-1036, 1380-1518`
- Modify: `src/atx_db/daily_adjustments.py:36-80`, `src/atx_db/asof/pricing.py` (`DAILY_PANEL_ASOF_SQL`, `selected_bars_sql`),
  `src/atx_db/universe_us_listed.py` (bar reads only)

**Interfaces:**
- Batch stage `market_daily_rebuild`: ~171 batches of 200 line ids into `market_daily_metrics__next` (no DELETE on the
  live table), swap at finalize. The owner bridge is built once per run; measure its Python memory (34k lines) and
  report it.
- Row-count windows at `market_daily.py:808-830` → session windows with minimum observations.
- `latest_by_code` tie-break (`:1028-1036`) uses an explicit numeric rule priority from the standardization seed, not
  lexical `source`/`rule_id`/`basis` strings.
- Daily adjustment factors: no more one ~32M-row snapshot per `as_of_date`. Provide
  `daily_adjustment_factors_asof_sql(cutoff) -> str` computed from `adjustment_factor_history` (event level, with P8
  per-step clocks) and `bars_asof_sql`; materialize only the latest as a batch stage if a consumer needs speed.
- Every PIT bar reader goes through `bars_asof_sql` (P14 restatements move original bars to
  `equity_daily_bar_revisions`).
- One declared price source per line; a return window that would mix sources returns NULL with reason
  `mixed_price_source` (the bulk source `bulk_bars_2015plus` with `adjusted_close = close` must never splice into
  TickerHistory3 windows).

- [ ] **Step 1:** Implement; keep `market_daily` outputs identical for single-source lines on the 200-line sample.
- [ ] **Step 2 (real replay):** replay one real P14-style restatement on a scratch DB: historical as-of market_daily and
  labels unchanged; `daily_adjustment_factors` growth per refresh ≤ number of new events.
- [ ] **Step 3:** Commit.

**Acceptance:** sample equality; replay invariant holds; batches ≤ 0.8 GiB.

---

### Task 3: [3.3] Identity layer — permanent ids, CCM-style link table, names history, company ME (migration 0331 part 1)

**Lane:** K · **Tokens:** MIG · **Depends on:** 0.6, 0.12 · **Peak target:** 0.8 GiB

**Files:**
- Create: `src/atx_db/identity_links.py`, `src/atx_db/migrations/bodies_0331.py`
- Modify: `src/atx_db/migrations/registry.py`, `src/atx_db/schema.py` (MIG), `src/atx_db/identity_reconstruction.py:1317`
  (next weekday → `calendar.next_session`), `src/atx_db/market_owner_bridge.py` (high-only tier sensitivity as a distinct
  labeled source; `tier_at(frame_clock)`), `src/atx_db/publication.py`/`api/catalog.py` (replace the current-only
  `v_security_master_public` from `bodies_0307.py:28-60` with a historical view)

**Interfaces:**
- 0331 tables (no PK/ART on large ones; uniqueness via DQC):
  ```sql
  CREATE TABLE security_permanent_ids (perm_security_id BIGINT NOT NULL, security_id VARCHAR NOT NULL,
      first_trade_date DATE, last_trade_date DATE, created_at TIMESTAMP NOT NULL);  -- never recycled
  CREATE TABLE company_permanent_ids (perm_company_id BIGINT NOT NULL, cik VARCHAR, basis VARCHAR NOT NULL,
      created_at TIMESTAMP NOT NULL);                                                  -- survives name changes
  CREATE TABLE security_company_links (
      perm_security_id BIGINT NOT NULL, perm_company_id BIGINT NOT NULL, cik VARCHAR,
      link_start DATE NOT NULL, link_end DATE,
      link_basis VARCHAR NOT NULL,   -- 'current_ticker_verified' | 'reconstructed_high' | 'reconstructed_medium' | 'strict_dated'
      link_primary VARCHAR NOT NULL, -- 'P' primary common line | 'J' joiner common class | 'N' non-common / secondary
      tier VARCHAR, available_at TIMESTAMP NOT NULL, evidence_ids VARCHAR, created_at TIMESTAMP NOT NULL);
  CREATE TABLE security_names_history (
      perm_security_id BIGINT NOT NULL, ticker VARCHAR, issuer_name VARCHAR, venue VARCHAR,
      valid_from DATE NOT NULL, valid_to DATE, source VARCHAR NOT NULL, available_at TIMESTAMP NOT NULL);
  ```
- `links_asof_sql(cutoff, tiers=("high","medium")) -> str` — PIT links using `tier_at(cutoff)` semantics.
- `me_company` = Σ over the company's common lines (`link_primary IN ('P','J')`) of line ME at the date; non-common
  lines never contribute.

- [ ] **Step 1:** Implement the builder as a batch stage `identity_links_rebuild` (inputs: RI1 evidence, bridge rules
  1–5, vendor ticker intervals, submissions `formerNames`, Nasdaq directory snapshot).
- [ ] **Step 2 (rehearsal on retained data):** build into a scratch DB from the retained inputs.
- [ ] **Step 3:** Commit.

**Acceptance (rehearsal):** all 25,760 vendor lines have a permanent id with first/last trade dates; ids unique and never
reused; ≥ 88% of operating-company lines carry a labeled CIK link; 50 known delisted names (sample across 2012–2025)
carry the right CIK and dates; the 331 multi-line CIKs get `me_company` summed over common classes only.

---

### Task 4: [3.4] Delisting completeness and a critical survivorship gate (migration 0331 part 2)

**Lane:** K · **Tokens:** MIG · **Depends on:** 0.6, 1.8, 3.3 (same MIG owner, sequential) · **Peak target:** 0.6 GiB

**Files:**
- Modify: `src/atx_db/delisting_evidence.py:71, 336-366`, `src/atx_db/delisting.py:52, ~944, ~1107`,
  `src/atx_db/quality/checks_survivorship.py:219-231`, `src/atx_db/migrations/bodies_0331.py` (severity changes via the
  swap helper — the checks registry has `DEFAULT now()` columns)

**Interfaces:**
- CIKs for a delisted line come from `links_asof_sql(last_trade_date)` (3.3) and dated `security_identifier_history`
  rows valid at `last_trade_date` — never the current-ticker map without a validity window.
- Reason evidence adds EDGAR 8-K items from `sec_submissions.items` (1.6 kept them): 1.03 bankruptcy, 2.01 completed
  acquisition, 3.01 delisting notice / deficiency; plus Form 25/25-NSE, Form 15, merger proxies (DEFM14A/S-4 with
  target-side forms).
- Terminal policies (`TERMINAL_RETURN_POLICY_ROWS`):

| Policy | When | Terminal leg |
|---|---|---|
| `merger_last_price` | merger evidence (2.01, DEFM14A, S-4 target-side) | 0% vs last close (cash consideration from 8-K 2.01 later) |
| `voluntary_last_price` | Form 15 / voluntary deregistration without performance evidence | 0% vs last close |
| `shumway_nyse_amex` | performance evidence (25-NSE without merger form, 1.03, 3.01, price < $1 for 30 sessions) and venue NYSE/AMEX | −30% |
| `shumway_nasdaq` | performance evidence and venue Nasdaq | −55% |
| `shumway_unknown_venue` | performance evidence, venue unknown | −30% (labeled) |
| `unknown_neutral` | no CIK or no evidence | 0% vs last close; Shumway result published as sensitivity |

- Severity `critical` for `delisting_events_without_terminal_return` and `cik_less_unknown_performance_policy_rows`.

- [ ] **Step 1:** Implement evidence + policies; migration part 2.
- [ ] **Step 2 (rehearsal):** on the retained/rehearsal data, count cessations by reason × policy; 50 hand-verified
  2013–2020 cash-merger targets: none gets a Shumway policy and each terminal equals last price × (1 + policy).
- [ ] **Step 3:** Commit.

**Acceptance:** every cessation has exactly one terminal policy row; reason × policy table in the report; 50/50
merger targets correct.

---

### Task 5: [3.5] Forward labels v4 — month-end formation, next-session entry, cessation-cluster stitching, counted drops

**Lane:** K · **Tokens:** — · **Depends on:** 1.11, 3.2, 3.4 · **Peak target:** 0.8 GiB

**Files:**
- Modify: `src/atx_db/_forward_return_publication.py` (`_CALENDAR_SQL`, `:86-136` staging names, `:352-361`,
  `:370-392`, `:391`, `:447-460`), `src/atx_db/research/labels.py` (consumers)

**Interfaces:**
- Formation default `month_end_next_session` (ruling R-11); `every_session` opt-in.
- Label: anchor = formation date `eom`; entry = close of `next_session(eom)`; exit = close of the session h months
  later; total return from repaired adjusted close; one declared price source per line.
- Stitching per cessation **cluster** (a relisted line keeps later anchors), not only the earliest cessation.
- Receipt adds `dropped_anchors_by_reason` (`no_endpoint_bar`, `halt_over_31_sessions`, `mixed_price_source`,
  `no_terminal`) and a `last_price_within_window` basis variant.
- Publisher gate: expected-minus-published cessation-spanning anchors = 0, else the release is `candidate` (critical).
- Contract bump: `forward_return_publication_v4`; update every consumer in the same task (grep).

- [ ] **Step 1:** Implement as batch stage `labels_rebuild` (formation batches of 100k, checkpoint every 400k).
- [ ] **Step 2 (rehearsal):** dropped-anchor histogram by liquidity decile; stitched-anchor equality check.
- [ ] **Step 3:** Commit.

**Acceptance:** expected-minus-published = 0 on the rehearsal; histogram in the report; no consumer left on v3.

---

### Task 6: [3.6] B2/B3 heavy run — identity, corporate actions, market, universe, delisting, labels; market lake snapshot

**Lane:** OPS · **Tokens:** HEAVY, ACT (invocation) · **Depends on:** 2.7, 3.1–3.5 · **Peak:** ≤ 1.0 GiB/slice

**Files:**
- Create: `.superpowers/sdd/tier1-v2/receipts/b2b3-*.jsonl`, `b2b3-summary.md`; data `data/research/lake/mkt_<UTC date>/`

- [ ] **Step 1:** Preconditions: pinned export; migrate to 0331; backup `pre-b2`.
- [ ] **Step 2 (B3 pre-step, mandatory):** `--only symbol_directory --force` then post-checks (0 latest rows with
  `as_of` 2026-09-20; latest bars 2026-09-18); `security_master --as-of-date 2026-09-20`.
- [ ] **Step 3:** In order (batch stages via `run_slices.py`, small stages via the ladder under the guard at 0.8):
  `identity_reconstruction` (RI2) → `identity_links_rebuild` → corporate actions (P8 events, `adjustment_factor_history`)
  → `market_daily_rebuild` → `price_metrics_a/b/c/d` → listing events/status → `universe_us_listed` → delisting
  evidence/events/terminals → `trading_calendar` (canonical) → `labels_rebuild` → classification (P6 FF12/FF49) →
  `stage_foreign_filer_reasons` (P11).
- [ ] **Step 4:** Market lake snapshot (read-only slice): `market_daily_metrics`, repaired daily returns, identity tables,
  names history, universe flags, delisting events and terminals, labels, P8 events, classifications.
- [ ] **Step 5:** `b2b3-summary.md`.

**Acceptance:** every stage published; no WAL; lake manifest written; counts reported.

---

### Task 7: [3.7] Market and survivorship QA on real data + strict-basis viability

**Lane:** K · **Tokens:** — · **Depends on:** 3.6 · **Peak target:** 0.8 GiB (reads the market lake snapshot)

**Files:**
- Create: `scripts/market_qa.py`, `docs/measurements/MARKET_SURVIVORSHIP_QA_<date>.md`

- [ ] **Step 1:** Measure: operating-company attrition (target ≤ 12.0%; high-only reported); cessations without a
  terminal (target 0) and the reason × policy table; dropped anchors by reason × liquidity decile; calendar
  reconciliation (0 missing sessions; stray bars excluded and counted); `me_company` vs DEI-based cap on a stratified
  200-issuer sample (within 0.2–5×, outliers itemized); P8 tie-out ≤ 1e-6; late splits resolved at their clocks; VA1
  check (PAYX Jan-2021 monthly return −5.63% repaired vs −8.74% vendor).
- [ ] **Step 2 (strict viability, RS-17):** strict-basis names per month by year; declare the testable strict window
  (≥ 36 formations with ≥ 1,000 names) or record that the release is reconstructed-basis only (ruling).
- [ ] **Step 3:** Commit.

**Acceptance:** report published with every number; failures routed to owners as findings.

---

### Task 8: [3.8] Final labels, owner→line spine with company ME and size groups, price-wave holdout

**Lane:** R · **Tokens:** — · **Depends on:** 3.6, 1.13 · **Peak target:** 0.8 GiB

**Files:**
- Modify: `src/atx_db/research/spine.py`, `src/atx_db/research/label_matrix.py`
- Create: `docs/research/WAVE1_PRICE_FINAL.md`

**Interfaces:**
- Final label matrix (L3) from the market lake labels (contract v4); `label_sha` with `provisional=false`.
- Spine columns added: `owner_id` (via `links_asof_sql(eom)`, default tiers high+medium, high-only as a labeled
  sensitivity), `primary_sec` (one line per owner by 30-day dollar volume, R2a rule), `me_company`, `size_grp`
  (JKP: mega > NYSE p80, large p50–p80, small p20–p50, micro p1–p20, nano < p1) from Ken French NYSE ME breakpoints if
  X.3 is done, else the PIT venue buckets, else a labeled tercile fallback), `common`, `obs_main`, `ff49`.

- [ ] **Step 1:** Build final labels and the extended spine.
- [ ] **Step 2:** Re-evaluate `w1_price` selection cells on final labels (report deltas vs provisional); compare
  session-based `ret_12_1` with the warehouse seed `momentum_12_1` (rank correlation ≥ 0.98; discrepancies by gap bucket).
- [ ] **Step 3:** `open_holdout("w1_price")` exactly once; grade under policy v4; write `WAVE1_PRICE_FINAL.md`.
- [ ] **Step 4:** Commit.

**Acceptance:** trial registry shows the w1 holdout opened once; ledger rows graded with `grade_basis=final_labels`.

---

### Task 9: [3.9] Event feature wave — EDGAR filings, corporate actions and earnings events

**Lane:** R · **Tokens:** CAT · **Depends on:** 1.8, 2.9 · **Peak target:** 0.6 GiB

**Files:**
- Create: `src/atx_db/research/filing_events.py`
- Modify: `src/atx_db/seeds/research_anomaly_catalog.csv` (wave `w4_events`)

**Interfaces:** owner-level features from the fundamentals lake `sec_submissions` columns (form, acceptance datetime,
items), P8 events (market lake) and P3 events (E1 source); event clock = acceptance + 46 h, usable at the next session.

| feature_id | Definition | Sign |
|---|---|---|
| `late_filer_12m` | any NT 10-K / NT 10-Q in the past 12 months | − |
| `auditor_change_12m` | 8-K Item 4.01 in the past 12 months | − |
| `non_reliance_24m` | 8-K Item 4.02 in the past 24 months | − |
| `amend_10k_24m` | count of 10-K/A with XBRL in the past 24 months | − |
| `filing_delay_chg` | days(period end → 10-K acceptance) minus prior year's | − |
| `shelf_seo_12m` | S-3 / 424B equity offering in the past 12 months | − |
| `activist_13d_6m` | SC 13D filed on the company in the past 6 months | + |
| `firm_age` | years since first 10-K | + |
| `ipo_age_le3` | first 10-K/S-1 within 3 years | − |
| `div_init_6m` / `div_omit_6m` | first cash dividend after ≥ 24 months without / omission after a regular payer (P8) | + / − |
| `split_12m` | confirmed forward split in the past 12 months (P8) | + |
| `ear_m1p1`, `runup_m21_m2`, `sue_ni_event` | P3 events through E1 (already cataloged in 0.10) | per catalog |

- [ ] **Step 1:** Implement; catalog rows; coverage per year.
- [ ] **Step 2:** Reconcile event counts per form-year against the submissions table (dedupe correctness) and spot-check
  20 events against the submissions JSON in `submissions.zip`.
- [ ] **Step 3:** Commit.

**Acceptance:** indicator features cover all owners (flagged); spot checks 20/20; coverage table published.

---

### Task 10: [3.10] Compositions, market-dependent scores and industry features

**Lane:** R · **Tokens:** CAT · **Depends on:** 3.8, 2.10, 2.11 · **Peak target:** 0.6 GiB per year pass

**Files:**
- Modify: `src/atx_db/research/fundamental_chars.py`, `src/atx_db/research/scores.py`, `src/atx_db/research/price_natives.py`
- Modify: `src/atx_db/seeds/research_anomaly_catalog.csv` (wave `w3_compositions`)

**Interfaces:**
- Materialize the composition definitions from 2.10 (`be_me_jkp`, `netdebt_me`, `bev_mev`, `tangible_bm`, `intang_bm`,
  `cash_productivity`, `xad_me`, `div12m_me`, `chcsho_12m`, `compeqiss`) using `me_company` at the formation.
- Market-dependent scores: CHS failure probability (NI/MTA, TL/MTA, cash/MTA, excess return, σ, relative size, price;
  CHS 2008 coefficients), QMJ (profitability, growth, safety with beta; Asness–Frazzini–Pedersen), Stambaugh–Yuan MGMT
  and PERF (average rank of legs), equity duration (Dechow–Sloan–Soliman, 10-year fade), KZ index (with Tobin's Q).
- Industry features: industry momentum (FF49 VW 6-1), industry-adjusted (`*_ia`: FF49-demeaned `be_me`, `cfo/p`,
  asset-turnover change, margin change, capex growth, size), sales Herfindahl (FF49), sin-stock flag (Hong–Kacperczyk
  SIC list), industry-adjusted 1-month reversal.

- [ ] **Step 1:** Implement; catalog rows; coverage per year.
- [ ] **Step 2:** Commit.

**Acceptance:** composition features ≥ 3,000 owners/month (2012–2025, 2017+ for 5-year windows); scores ≥ 2,500;
industry features ≥ 4,000; `classification_basis` carried.

---

### Task 11: [3.11] Existing catalog features (wave `w0_existing`) into the Parquet store from the lake snapshots

**Lane:** R · **Tokens:** PANEL · **Depends on:** 1.9, 2.7, 3.6 · **Peak target:** 0.6 GiB per formation-year

**Files:**
- Modify: `src/atx_db/research/panel.py` (a Parquet input mode: the same R2a selection SQL over lake tables instead of
  the attached warehouse), `src/atx_db/research/feature_store.py` (writer glue)

**Interfaces:**
- For every eligible row of the 175-row catalog (seed metrics, formation compositions, panel natives): per formation,
  select the PIT state with the R2a rules (FQ1 newest revision visible at the cutoff, `DEFAULT_MAX_AGE_DAYS = 200`,
  400 for annual, newest-NULL wins), compute compositions, apply the R2b transforms (imported, not forked), write
  `features/<basis>/<feature_id>/<feature_sha>.parquet`.
- Inputs: `derived_metric_values` and statement tables from the fundamentals lake (2.7), `market_daily_metrics` and
  the spine from the market lake (3.6/3.8).
- Panel slimming (RS-03): only catalog-referenced metrics, composition legs and daily operands are selected (~85 dollar-
  level building blocks dropped from the default feature set); dense-row reason accounting kept.

- [ ] **Step 1:** Implement the Parquet input mode; per formation-year passes.
- [ ] **Step 2 (oracle):** for 24 formations (2016–2017), the Parquet path equals the DuckDB R2a/R2b path on a lake-
  attached scratch DB (values exact; reason codes equal).
- [ ] **Step 3 (real):** build all w0 features 2012-04 → 2026-08; coverage per feature × year; total size and time.
- [ ] **Step 4:** Commit.

**Acceptance:** oracle equality; ≤ 0.6 GiB per pass; store size for w0 ≤ ~5 GB; coverage table published (no returns
read).
