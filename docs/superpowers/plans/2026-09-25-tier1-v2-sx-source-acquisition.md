# atx-db Tier-1 v2 — Sprint X: Source Acquisition (parallel lane, user-gated)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan
> task-by-task. Steps use checkbox (`- [ ]`) syntax. **No TDD** (user directive). **Every task here needs its user gate
> (index §8) recorded as granted in the ledger before any network fetch.** Code may be written before the gate opens;
> fetching may not.

**Goal:** Acquire the data families atx-db lacks — 13F holdings, FINRA short interest and short volume, external
benchmark factor files, the full FSDS history, 8-K earnings press releases, strict dated identity evidence and pre-2012
prices — through bounded, resumable fetch workers that never hold the warehouse writer, and load them through ledgered
batch stages.

**Architecture:** Fetch and load are separate processes. Fetch workers use `sec_http` (1.7: approved user agent,
cross-process 5 req/s token bucket) or plain HTTPS for non-SEC sources, write content-addressed files plus a JSONL
fetch ledger under `data/raw/<source>/`, and resume by skipping ledgered URLs. Loaders stream files to Parquet and append
through batch stages (1.3) into `__next` tables, publishing by swap; research gets lake snapshots.

**Tech Stack:** Python, `sec_http` (1.7), `batch_runner` (1.3), PyArrow, DuckDB 1.5.5.

**Spec:** `docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md` (§3 security/sources rules, §4, §5, §8 gates).
Nodes X.1–X.7.

## Global Constraints

Index §3, §4 and §5 apply. SEC requests: user agent exactly `atx-db/0.1 atx-research@example.com`, ≤ 5 req/s across
all workers, bulk files preferred over per-company requests. Never send any other email address to any service; non-SEC
requests use a descriptive user agent without an email. Every downloaded file gets a receipt (URL, retrieved_at UTC,
bytes, sha256). Paths are relative to `C:\atx\atx-db\` unless they start with `.superpowers/` or `docs/`. Commit
trailer `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.

## Review Focus

1. **Rate and identity compliance.** Expected: measured request rate ≤ 5 req/s even with concurrent workers; the only
   email ever sent is the approved placeholder.
2. **Knowledge-time stamping.** 13F holdings are known at acceptance (≤ 45 days after quarter end), FINRA short interest
   at publication (7 business days after settlement), press releases at 8-K acceptance. Expected: never at period end.
3. **Unit and schema breaks.** 13F values are in $ thousands before 2023-01-03 and $ after; FINRA may change schema
   (SR-FINRA-2026-012). Expected: normalized, with the break documented.
4. **Resumability.** Expected: a killed fetch resumes with 0 duplicate requests; a killed load resumes from the ledger.
5. **Splicing vendors.** Pre-2012 prices from another vendor must not be spliced as adjusted series. Expected:
   adjustments rebuilt from raw prices plus corporate actions; overlap reconciliation reported.

---

### Task 1: [X.1] SEC Form 13F data sets, 2013Q2 → latest (gate U2)

**Lane:** X · **Depends on:** 1.3, 1.7, U2 · **Peak target:** fetch 0.5 GiB, load 0.8 GiB

**Files:**
- Modify: `scripts/download_13f.py`, `src/atx_db/thirteenf.py` (default scope is AAPL only at present →
  full universe; `pd.read_csv` chunks at `:515-520` → PyArrow streaming; per-period DELETE at `:464-481` → batch stage)

**Interfaces:** batch stage `thirteenf_rebuild` (one batch per quarter data set) into `thirteenf_*__next`; columns keep
accession, filer CIK, report period, acceptance datetime, CUSIP, title of class, shares/principal, value (normalized to
USD), put/call, amendment type (restatement vs new holdings). CUSIP → security mapping uses `ownership_identity` (P9 v2
rules). Lake export `own_13f_<date>` of per-security-quarter aggregates for 4.5.

- [ ] **Step 1:** Fetch all quarterly data-set zips with `sec_http`; receipts.
- [ ] **Step 2:** Load; unit normalization at 2023-01-03; value/shares × price sanity check per filer-quarter.
- [ ] **Step 3:** Report per quarter: filers, holdings rows, CUSIP mapping rate, IO > 1 cases.
- [ ] **Step 4:** Commit code.

**Acceptance:** every quarter from 2013Q2 loaded; mapping rate ≥ 90% of reported value per quarter (or itemized);
IO ≥ 2,500 owners per quarter after mapping.

---

### Task 2: [X.2] FINRA short interest and daily short volume (gate U2)

**Lane:** X · **Depends on:** 1.3, U2 · **Peak target:** 0.5 GiB

**Files:**
- Modify: `src/atx_db/finra.py` (`:352` pandas concat of all pages → streamed batches; `:87` user agent via a shared
  constant without email), `src/atx_db/short_volume.py`

- [ ] **Step 1:** Check the outcome of SR-FINRA-2026-012 (SEC decision date 2026-08-14) before fixing the schema;
  record it.
- [ ] **Step 2:** Fetch twice-monthly short-interest history as far back as the source provides (record the first
  settlement date) and Reg SHO daily short-volume files; clocks: publication date (settlement + 7 business days).
- [ ] **Step 3:** Load through batch stages; lake export `own_finra_<date>`.
- [ ] **Step 4:** Commit.

**Acceptance:** ≥ 4,000 lines per settlement after symbol mapping (dated bar symbol); coverage by year reported.

---

### Task 3: [X.3] External benchmark files (gate U6 / RX14)

**Lane:** X · **Depends on:** U6 · **Peak target:** 0.2 GiB

**Files:**
- Create: `scripts/fetch_benchmarks.py`, `src/atx_db/research/benchmarks_load.py`

- [ ] **Step 1:** Download to `data/raw/benchmarks/<source>/<UTC date>/` with receipts: Ken French data library (Fama–French
  5 factors 2×3 monthly and daily, momentum factor, NYSE ME breakpoints), JKP US factor returns (jkpfactors.com),
  Open Source Asset Pricing `SignalDoc.csv` and portfolio returns, global-q q5 factors.
- [ ] **Step 2:** Load into lake datasets `bench_french_ff5_umd_monthly`, `bench_french_me_breakpoints`,
  `bench_jkp_us_factors`, `bench_osap_signaldoc`, `bench_osap_portfolios`, `bench_q5`.
- [ ] **Step 3:** Commit code (not data).

**Acceptance:** all six datasets present with receipts; row counts and date ranges reported.

---

### Task 4: [X.4] SEC Financial Statement Data Sets, full history (gate U3 / RX10 extension)

**Lane:** X · **Depends on:** 1.1, 1.7, U3 · **Peak target:** 0.5 GiB

**Files:**
- Modify: `src/atx_db/fsds_baseline.py` (quarter loop over all available quarters; streamed to Parquet, no warehouse)

- [ ] **Step 1:** Fetch every quarterly zip from 2009q2 to the latest with `sec_http`; receipts.
- [ ] **Step 2:** Stream SUB/NUM/TAG/PRE to Parquet per quarter under `data/staging/fsds/`; keep `accepted` datetimes (an
  acceptance-clock sensitivity input) and custom tags (recovery candidates for small filers).
- [ ] **Step 3:** Re-run 2.8's benchmark on the wider sample.
- [ ] **Step 4:** Commit.

**Acceptance:** all quarters staged; benchmark re-run published.

---

### Task 5: [X.5] 8-K Item 2.02 EX-99 earnings press releases (gate U9 / RX15)

**Lane:** X · **Depends on:** 1.7, U9 · **Peak target:** fetch 0.5 GiB, load 0.6 GiB

**Files:**
- Modify: `src/atx_db/press_release.py` (fetch worker from 1.7; parser), loader into `sec_earnings_release_facts`

- [ ] **Step 1:** Fetch the 426,151 candidates (2004–2026) at ≤ 5 req/s with the resumable ledger (≈ 47 h of fetch time;
  run in the background in segments).
- [ ] **Step 2:** Parse EPS and revenue from EX-99 with the existing parser; load via batch append; clock = 8-K acceptance.
- [ ] **Step 3:** Hand P3 a SUE-at-announcement source (contract bump in `research/events.py`, consumers updated).
- [ ] **Step 4:** Commit.

**Acceptance:** fetch ledger complete (or the remaining count stated); parse success rate by year; P3 SUE at
announcement coverage ≥ 2,500 owners per quarter from 2013.

---

### Task 6: [X.6] Strict dated identity evidence and daily directory snapshots (gate U5 / RX13; U10 for the daily job)

**Lane:** X · **Depends on:** 1.7, 3.3, U5 · **Peak target:** 0.5 GiB

**Files:**
- Modify: `src/atx_db/historical_identity_sources.py`, `src/atx_db/symbol_directory.py`, `src/atx_db/sec_submissions.py`
- Create: fetch worker for symbol-bearing filings (uses `sec_http`)

- [ ] **Step 1:** Fetch symbol-bearing 8-K 2.01/3.01, 10-K Item 5 and 8-A12B exhibits for 2012–2018 delisted lines;
  extract stated symbols and dates as `verified_dated` evidence; feed the 3.3 link table (`link_basis='strict_dated'`).
- [ ] **Step 2:** Start daily Nasdaq directory snapshots (nasdaqlisted/otherlisted) as part of 5.3, so strict listing
  history accumulates from now on.
- [ ] **Step 3:** Rebuild the strict `us_listed_v1` universe; measure the `universe_certification` gate.
- [ ] **Step 4:** Commit.

**Acceptance:** strict universe non-empty for 2015+; certification gate measured (not unmeasured); every strict link that
disagrees with an RI1 link listed.

---

### Task 7: [X.7] Pre-2012 price history (gate U4 / RS-15)

**Lane:** X · **Tokens:** HEAVY (load) · **Depends on:** 1.3, U4 · **Peak target:** ≤ 1.0 GiB per slice

**Files:**
- Create: a loader for the chosen vendor (the repo survey `docs/BULK_PRICE_SOURCE_OPTIONS.md` lists flat files from 2003)

- [ ] **Step 1:** Load raw bars 2005-01 → 2012-03 including delisted names as a separate source; never splice adjusted
  series across vendors — rebuild adjustments from raw prices and corporate actions.
- [ ] **Step 2:** Reconcile the overlap with TickerHistory3 (adjusted daily return difference per line).
- [ ] **Step 3:** Extend price natives and labels from 2006; re-run the 1.10 power table.
- [ ] **Step 4:** Commit.

**Acceptance:** ≥ 5,500 common lines per month in 2005–2012; overlap adjusted-return difference median < 1 bp and
p99 < 10 bp; power table updated.
