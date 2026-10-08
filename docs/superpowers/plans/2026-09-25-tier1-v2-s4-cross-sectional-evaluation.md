# atx-db Tier-1 v2 — Sprint 4: Cross-Sectional Evaluation at Scale

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan
> task-by-task. Steps use checkbox (`- [ ]`) syntax. **No TDD** (user directive): acceptance is a real-data
> measurement; tests only where the index §5.4 allows.

**Goal:** Grade the whole characteristic library (≥ 250 hypotheses across waves w0–w5) on real, survivorship-safe data
under the frozen policy v4; validate the data against external factor libraries before trusting any result; build
theme and P5 composites and multivariate evidence; ship the alpha-ready monthly panel and a factor-zoo report.

**Architecture:** Evaluation runs per feature in parallel bounded workers over the Parquet feature store and the final
label matrix, caching cells by `(feature_sha, label_sha, eval_spec_sha)` so adding a feature costs minutes. Gating uses
policy v4 (replication vs discovery classes, investable co-primary cell, JT calendar-time long-horizon evidence,
holdout opened once per wave). Everything else (capped-VW terciles, costs, spanning alphas, decay curves,
earnings-day decomposition, publication splits) is reported, never gating.

**Tech Stack:** Python, DuckDB 1.5.5 (256MB per worker), NumPy (`OPENBLAS_NUM_THREADS=1`), research stats core
(EWC fixed-b, BH/Holm, HLZ, DSR), research store (1.9).

**Spec:** `docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md` (§3, §4, §5; rulings R-5, R-6, R-7, R-12).
Nodes 4.1–4.8.

## Global Constraints

Every task includes the index §3, §4 (M1–M8) and §5. Research never opens the warehouse (ruling R-5). Workers:
DuckDB `memory_limit=256MB`, `threads=1`, ≤ 0.6 GiB each, ≤ 2 concurrent unless no HEAVY slice is running (then ≤ 3).
Paths are relative to `C:\atx\atx-db\` unless they start with `.superpowers/` or `docs/`. Commit trailer
`Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.

## Review Focus

1. **Looking at returns before registration.** Expected: every wave is registered (catalog digest + policy sha) in the
   trial registry before its first label join; a second holdout open is refused.
2. **Microcap-driven significance.** An equal-weighted IC over all names is dominated by the ~60% of names below the
   NYSE 20th percentile. Expected: the investable co-primary cell passes too, and VW/capped-VW results are shown next to
   EW with verified-cap coverage.
3. **Multiple-testing bookkeeping.** Expected: the gating BH family is the primary cells over eligible hypotheses,
   cumulative across waves through the trial registry; DSR `n_trials` comes from the registry, not from variant ×
   horizon multiplication.
4. **Data bugs mistaken for alpha.** Expected: the external validation gate (4.2) passes, or its failures are fixed,
   before any result is interpreted or exported.
5. **Holdout leakage through composites and multivariate models.** Expected: composite constituents and model weights
   use selection-sample evidence only; holdout formations are scored once, after freezing.

---

### Task 1: [4.1] Evaluation additions (reported-only metrics)

**Lane:** R · **Tokens:** EVAL, FEAT (beta-neutral variant) · **Depends on:** 1.10 · **Peak target:** 0.5 GiB per worker

**Files:**
- Modify: `src/atx_db/research/evaluation.py`, `src/atx_db/research/factor_returns.py` (spanning for every cell),
  `src/atx_db/research/features.py` (beta-neutral variant; FEAT)

**Interfaces (new cell columns; none of them gate):**
- Portfolios: JKP terciles and deciles with breakpoints from non-microcaps (ME above the NYSE 20th percentile),
  weights EW, VW and **capped-VW** (ME winsorized at the NYSE 80th percentile); ≥ 5 stocks per leg.
- Fama–MacBeth: controls log ME, log BE/ME, `ret_12_1`, `gp_at` (gross profitability), `at_gr1` (asset growth); OLS
  and WLS (ME weights); HXZ K-month averaged slopes for K ∈ {3, 6, 12}; NW t with lags max(h − 1, ⌊4(T/100)^{2/9}⌋).
- IC decay by signal age: IC_k = corr(x_{t−k}, r_{t+1}), k = 0…11; half-life.
- Costs: one-sided turnover with drifted weights; half-spread per stock-month from `bidask_ar_21d` (Abdi–Ranaldo) with
  Corwin–Schultz fallback; net L/S; **break-even cost** = mean gross / mean turnover; buy/hold-spread variant (enter
  decile 10, exit below decile 8); flag turnover > 50%/month; flat 10/25/50 bp kept for comparability.
- Capacity: $ traded at 1% of ADV per decile.
- Spanning: α, NW t(α), R² of the L/S vs atx P4 factors (CAPM, FF5 + UMD analogs); vs external French FF5 + UMD and q5
  when X.3 is loaded.
- Earnings-announcement-day decomposition (P3 sessions): share of the L/S return earned in [−1, +1] around
  announcements (Engelberg–McLean–Pontiff).
- Pre/post publication split (catalog `publication_year`), replication table (atx t vs the original paper's t from the
  OSAP `SignalDoc.csv`, X.3), minimum detectable effect per cell, JKP-style empirical-Bayes shrunk IC for replications.
- Beta-neutral variant (residualize on `beta_ew_252d` per formation).

- [ ] **Step 1:** Implement; verify the policy v4 sha is unchanged (these are reported-only).
- [ ] **Step 2 (real):** recompute the opened `w1_price` cells from the cache inputs and confirm gating fields are
  byte-identical, new fields populated.
- [ ] **Step 3:** Commit.

**Acceptance:** every w1 cell has the new columns; gating fields unchanged; per-feature evaluation ≤ 0.5 GiB.

---

### Task 2: [4.2] External validation gate (data-trust gate)

**Lane:** R · **Tokens:** — · **Depends on:** X.3, 3.8, 3.10, 3.11 · **Peak target:** 0.4 GiB

**Files:**
- Create: `src/atx_db/research/benchmarks.py`, `docs/research/EXTERNAL_VALIDATION_<date>.md`

**Interfaces:**
- Loads the X.3 files from the lake (`bench_french_ff5_umd_monthly`, `bench_french_me_breakpoints`, `bench_jkp_us_factors`,
  `bench_osap_portfolios`, `bench_q5`).
- Runs the P4 factor build (`research/factor_returns.py`) on the final labels and spine.

- [ ] **Step 1:** Correlate atx monthly factors with Ken French 2013-01 → 2023-12 (selection period only).
- [ ] **Step 2:** For ~40 characteristics that overlap JKP (e.g. `be_me_jkp`, `at_gr1`/`asset_growth`, `ret_12_1`,
  `gp_at`, `ope_be`, `noa_at`, `oaccruals_at`, `chcsho_12m`, `rvol_21d`, `ami_126d`, `beta_bab_1260d`…), build the JKP
  construction (capped-VW terciles, non-microcap breakpoints) and correlate with JKP US factor returns; same for OSAP
  portfolios where they overlap.
- [ ] **Step 3:** Every failure becomes a data-bug finding with an owner (identity, returns, fundamentals item chain,
  timing) and is fixed or ruled before 4.3's results are interpreted.

**Acceptance:** Mkt ρ ≥ 0.99; SMB and UMD ≥ 0.90; HML ≥ 0.85; RMW and CMA ≥ 0.75 (construction differences documented);
JKP overlap sign agreement ≥ 80% and median ρ ≥ 0.6. If X.3 is not granted (U6), this task records `external_validation:
not_run` in the manifest and 4.3 results are labeled unvalidated.

---

### Task 3: [4.3] Full-family real evaluation, qualification ledger v4 and QUALIFIED_SIGNALS

**Lane:** R · **Tokens:** EVAL · **Depends on:** 4.1, 4.2, 2.10, 2.11, 3.8, 3.9, 3.10, 3.11 · **Peak:** 0.6 GiB × 2 workers

**Files:**
- Modify: `scripts/research_run_wave.py`, `docs/research/QUALIFIED_SIGNALS.md` (regenerated), `docs/research/`
  ledger docs

- [ ] **Step 1:** `register_wave` for `w0_existing` (175 catalog rows built in 3.11), `w2_fund_a` (2.10 + 2.11),
  `w3_compositions` (3.10), `w4_events` (3.9) — each with its frozen catalog digest and the policy v4 sha — before any
  label join.
- [ ] **Step 2:** Evaluate selection samples with 2 workers (cells cached); then `open_holdout(wave)` once per wave on
  final labels; grade under policy v4 (replication vs discovery; investable co-primary; JT K = 6, 12 for long-horizon
  claims; coverage against the catalog population).
- [ ] **Step 3:** Regenerate `QUALIFIED_SIGNALS.md`: per wave and theme, grade counts (`qualified`, `candidate`,
  `insufficient_coverage`, `failed`), and the full per-feature table (coverage, IC, ICIR, EWC z, investable IC, decile
  and tercile L/S EW/VW/capped-VW with t, JT-12 t, FF6 α t, decay half-life, turnover, break-even cost).
- [ ] **Step 4:** Commit.

**Acceptance:** ≥ 250 hypotheses graded; every feature has cells for h ∈ {1, 3, 6, 12}; ledger seals verify; trial
registry shows one holdout open per wave; full re-evaluation ≤ 4 h with 2 workers (cached re-run minutes).

---

### Task 4: [4.4] Theme composites, P5 composites and multivariate incremental evidence

**Lane:** R · **Tokens:** — · **Depends on:** 4.3 · **Peak target:** 0.6 GiB

**Files:**
- Modify: `src/atx_db/research/composites.py`
- Create: `src/atx_db/research/multivariate.py`, `docs/research/COMPOSITES_AND_MULTIVARIATE_<date>.md`

**Interfaces:**
- Theme composites: for each of the 13 JKP themes, the equal-weighted average of signed `rank_u` over **all** catalog
  features in the theme (membership fixed by the catalog, not by performance) → evidence class `replication_composite`.
- P5 composites (performance-selected, policy `p5-composite-v2` or a re-frozen v3 if the catalog changed) on real data:
  constituents from selection-sample R4 evidence only; `composite_post_selection` stamp; discovery gate.
- Multivariate: joint Fama–MacBeth of each selection-qualified signal against all other selection-qualified signals plus
  the FF6-type characteristics; elastic net cross-validated on train (≤ 2019), scored on validation (2020–2023), never
  on the holdout; streamed per formation (a full 160 × 9k × 250 float32 matrix would be ~1.4 GB).

- [ ] **Step 1:** Implement and run; register composite waves (`w6_composites`) before their holdout.
- [ ] **Step 2:** Null-simulation check for P5 selection on the real feature set (as in the session-3 P5 sim).
- [ ] **Step 3:** Commit report.

**Acceptance:** theme composite table (13 rows); P5 composites graded; multivariate annex lists signals that keep
|z| ≥ 2 jointly and the validation out-of-sample IC/R² of the combined model.

---

### Task 5: [4.5] Ownership and short-interest wave on real 13F / FINRA data

**Lane:** R · **Tokens:** CAT · **Depends on:** X.1, X.2, 3.6, 0.5 · **Peak target:** 0.6 GiB

**Files:**
- Modify: `src/atx_db/research/ownership_features.py` (reads the lake snapshots of X.1/X.2), catalog rows (`w5_ownership`)

**Interfaces:** P9 v2 features plus: RIO (Nagel residual institutional ownership: logit(IO) residual on log ME and
log ME²), Δbreadth on the same filer set in both quarters (Chen–Hong–Stein), IO-constrained short interest
(Asquith–Pathak–Ritter), days to cover (SI / average daily volume, Hong et al.); clocks: 13F at acceptance (≤ 45 days
after quarter end; value units $ thousands before 2023-01-03, $ after), FINRA at publication date (7 business days
after settlement).

- [ ] **Step 1:** Build features; coverage per quarter / settlement; survivor-conditioning label from P9.
- [ ] **Step 2:** Register `w5_ownership`, evaluate selection, open holdout once, grade.
- [ ] **Step 3:** Commit.

**Acceptance:** IO ≥ 2,500 owners per quarter 2013–2025; SI ≥ 4,000 lines per settlement; ledger rows graded.

---

### Task 6: [4.6] Alpha-ready export — JKP-layout monthly panel, characteristic metadata, factor returns, API dataset

**Lane:** R/Q · **Tokens:** — · **Depends on:** 4.3 · **Peak target:** 0.8 GiB

**Files:**
- Create: `src/atx_db/research/alpha_export.py`, `docs/research/ALPHA_PANEL_DICTIONARY.md`
- Modify: `src/atx_db/publication.py`, `src/atx_db/api/catalog.py` (dataset `research_characteristics_monthly`,
  eligibility capped at `candidate`)

**Interfaces:**
- `data/research/exports/<release_id>/panel/year=YYYY.parquet`: `perm_security_id, perm_company_id, eom, me,
  me_company, ret, ret_exc, ret_exc_lead1m, size_grp, common, exch_main (NULL + basis label where unknown),
  primary_sec, obs_main, ff49, <feature>_raw, <feature>_signed, <feature>_rank` for qualified + candidate features,
  `label_h1…h12`, feature clocks, manifest digest.
- `characteristics.parquet` (metadata): feature_id, theme, sign, evidence_class, paper, publication_year, original t
  (OSAP), horizon, inputs, fallback chain, population, grade, coverage by year.
- `factors.parquet`: per characteristic and month, tercile capped-VW and decile VW/EW L/S returns.
- `links.parquet`: the 3.3 link table.

- [ ] **Step 1:** Implement; write the dictionary doc.
- [ ] **Step 2 (measure):** total size, load time and peak for a reader loading the full panel with DuckDB.
- [ ] **Step 3:** Register the API dataset (candidate); commit.

**Acceptance:** ≤ 1 GB for 2013–2026; loads in < 30 s at < 800 MB; manifest digest equals the ledger manifest.

---

### Task 7: [4.7] Factor-zoo report

**Lane:** R · **Tokens:** — · **Depends on:** 4.3 (4.4, 4.5, 4.8 appended when done) · **Peak target:** 0.4 GiB

**Files:**
- Create: `docs/research/FACTOR_ZOO_<date>.md`

- [ ] **Step 1:** Sections: sample and power (the 1.10 MDE table, effective formations, post-publication share); data
  validation (4.2); per-theme summaries; the per-feature table; decay half-lives; costs and break-even; spanning alphas;
  earnings-day shares; replication table vs published t; pre/post publication; survivorship sensitivities (4.8); what
  qualified and why most signals are `candidate` (power, not economics, where that is the case).
- [ ] **Step 2:** Commit.

**Acceptance:** every number traces to a ledger cell or a receipt (cell ids cited).

---

### Task 8: [4.8] Survivorship delta study

**Lane:** R · **Tokens:** — · **Depends on:** 4.3 · **Peak target:** 0.5 GiB

**Files:**
- Create: `docs/research/SURVIVORSHIP_DELTA_<date>.md`

- [ ] **Step 1:** Per feature, IC and L/S on (a) linked owners via current tickers only vs (b) owners newly linked by
  RI1 (high+medium), (c) high-only tier; price features on linked vs unlinked lines; terminal-policy sensitivity
  (`unknown_neutral` vs Shumway on CIK-less unknown cessations).
- [ ] **Step 2:** Flag features with |ΔIC| > 0.01; add the flag to the ledger annex; commit.

**Acceptance:** ΔIC table for every graded feature; flagged list published.
