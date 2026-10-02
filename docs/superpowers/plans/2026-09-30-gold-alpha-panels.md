# Gold alpha panels 2020-2026 (atx-db → atx-impl US equity long/short)

**Spec (binding):** owner goal 2026-09-30: "generate high quality alpha panels in atx-db to support atx-impl US equity
long short strategy, focus on large cross sectional panels for 2020-2026, backfill and bronze/silver layer underlying
data you need to generate gold quality features with high IC predictive power over forward 5, 21, 63 day forward
returns. Use sub agent driven dev." Inherited invariants: `docs/superpowers/plans/2026-09-28-tier1-v3-parity-warehouse.md`
§3 and the rulings D1-D8 in `.superpowers/sdd/tier1-v3/CARRY.md` (both stay binding).

**Layers.** Bronze = raw landings `atx-db/data/raw/**`, `data/cache`, `data/staging`, TickerHistory3. Silver = the
stage lake `atx-db/data/alpha_panel/v1/<stage>/` (point-in-time stages and the daily `panel/`). Gold = three new
stages: `labels/` (forward returns, measurement only), `characteristics/` v2 (raw prior-signed features), `gold/`
(selected, cross-sectionally normalized features and family composites), plus the consumer export.

**State at start (2026-09-30).** Branch `feat/tier1-v3-warehouse`. `panel/` v2 2018-2026 built (189 columns, keyed
`(session_date, security_id)`, universe flag `member_equity` ≈ top-3000 ADV operating equities). `borrow_proxy/`
built. `metrics/` failed with a DuckDB OOM; the lo1 export and consumer acceptance load never ran.
`fundamentals_v10` code is committed but not built. `characteristics/` does not exist on disk; `characteristics.py`
(≈70 features) was written against panel v1 column names (for example `earn_flag`, now `earn_flag_vendor`).
`evaluate.py` is the old TRAIN-only IC screen (1-day and 21-day).

---

## Global Constraints (every task, every review)

1. **Holdout (D6 as amended by this plan's ruling R1, aligned to the consumer's binding OD-1: consumer TRAIN
   = [2020-01-01, 2024-01-01), sealed from 2024-01-01).** No return-based statistic may be computed on any return
   realized after 2023-12-29 (the last 2023 session). A forward label for decision session d and horizon h spans
   sessions d+2 .. d+1+h; it may exist only when session d+1+h ≤ 2023-12-29. Coverage, turnover, correlation and
   other non-return statistics may use any year inside atx-db. **Consumer seal:** nothing delivered to the consumer
   (exports under `export/`) may contain a session dated 2024-01-01 or later.
2. **Windows.** DISCOVERY = decision sessions 2019-01-02 .. 2019-12-31. TRAIN = decision sessions 2020-01-02 .. the
   last session whose label fits rule 1 (the consumer's train years). Gold panel values cover sessions 2020-01-02 ..
   the last panel session (atx-db only; exports stop at 2023-12-31). IC is reported per calendar year as well, so
   2023 can be read separately.
3. **Forward-return convention (identical to the consumer's `close[d+1+h]/close[d+1]-1`, execution delay 1).**
   Decide after the close of d, trade d+1, earn from the close of d+1: `fwd_h(d) = Σ_{k=2..h+1} ln(1 + r[d+k])` over
   calendar sessions, `r` = panel `ret` with `ret_guarded` rows treated as missing. Horizons h ∈ {1, 5, 21, 63};
   5, 21 and 63 are the goal's horizons (and the consumer's IC runner's).
4. **Point in time.** A panel value at `(session_date d, security_id)` is known by 22:00 UTC of d. A characteristic
   at d may use only panel rows with `session_date ≤ d` of the same line and same-session cross sections. No `lead`,
   no window frame with FOLLOWING rows, no whole-history statistic (global mean, full-sample rank) in any
   feature. **IV ruling:** every feature built from `iv_atm_*` uses the value of the line's previous session (lag 1)
   because the vendor delivery clock (03:00-04:00 UTC next day) may be after the 22:00 UTC mark (open issue in
   `atx-db/docs/TIER1_V3_STATUS.md`).
5. **IC definition.** Daily cross-sectional Spearman rank correlation among `member_equity` lines with a non-null
   feature and a non-null label on that session (≥ 100 lines, else the session is skipped). Prior-signed: every
   feature is oriented so that higher = predicted higher return (literature prior), so a useful feature has IC > 0.
   Significance: mean daily IC over its Newey-West standard error with lag = h (h = 1 uses lag 5). Industry-neutral IC:
   the same after demeaning both the feature rank and the label rank within `grp_ff49` on the session (groups with
   < 3 lines dropped). Risk-adjusted IC (the consumer's admission screen residualizes this way): the feature's
   centred rank residualized per session by OLS on the centred ranks of `beta_252`, `vol_63` and `log_adv63`, then
   Spearman against the label. Two universes: `member_equity` (primary, the large cross-section) and `lo` =
   `member_equity AND cik IS NOT NULL AND link_tier <> 'backfill' AND is_issuer_primary AND is_common` (a proxy of
   the consumer's `linked-operating-v1` universe).
6. **Resources.** Every data job runs under the memory guard:
   `cd C:/atx/atx-db && export PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1 && .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb <cap> --wait-minutes 120 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.<module> <args>`.
   Cap ≤ 1.0 GiB (prefer 0.6). DuckDB `memory_limit` ≤ 60% of the cap, threads ≤ 2 (1 for large joins); large
   temporary tables use a file-backed scratch database (`common.connect(..., db_file="<name>.duckdb")`). Guard exit
   137 (low commit stop) and 78 (not admitted) are retryable; 1 is a real failure. The host is shared with other
   sessions (VS Code, an atx-impl sprint): at most two heavy (≥ 0.6 GiB) jobs of this plan at once. Keep C: free
   ≥ 40 GB (`df -h /c`).
7. **Stage conventions** (`atx_db.alpha_panel.common`): `stage_dir`, `connect`, `copy_to_parquet` (atomic
   `.partial` → rename; row groups ≤ 32768 for wide tables), `write_stage_manifest` (schema id, code SHA, per-file
   SHA-256, `input_manifests_sha256` of every input stage manifest read). Year-partitioned output
   `<stage>/year=YYYY/*.parquet`. Every stage is resumable.
8. **Code.** Python 3 in `atx-db/src/atx_db/alpha_panel/`, style of the surrounding modules (dense docstrings, type
   hints). Every rule gets a fixture test in `atx-db/tests/test_alpha_panel_<area>*.py`; tests are offline, < 60 s per
   file, and run with `cd C:/atx/atx-db && PYTHONPATH=src .venv/Scripts/python.exe -m pytest <file> -q`. Files are
   created/edited with the Write/Edit tools.
9. **Commits.** Explicit pathspecs only (the git index is shared with other sessions):
   `git add <paths> && git commit -m "<msg>" -- <paths>`. Never `git add -A`/`.`, never commit data, never switch
   branches, stash, reset or rebase. Message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
   If `.git/index.lock` exists, wait and retry (delete only if older than 2 minutes).
10. **Do not touch** files or stages owned by another task unless the task says so. Never kill processes you did
    not start. Never delete consumer dependencies (`data/alpha_panel/v1/**` published stages,
    `data/raw/finra_short_volume`, `data/research/identity_rehearsal/session8-phased-r4`, TickerHistory3).
11. **Reports.** Each task writes its report to the path given in the dispatch: what was built (modules, stages,
    rows, bytes), each done criterion PASS/FAIL with the measured number and the command that measured it,
    deviations, open issues.

---

### Task 1: S0.1 tail — metrics OOM fix, lo1 export, consumer acceptance load

**Why.** The consumer contract (lo1 aligned export + acceptance load) is the delivery path for every later field.

**Owns:** `atx-db/src/atx_db/alpha_panel/metrics.py`, `atx-db/src/atx_db/alpha_panel/export_impl.py`, their tests,
stages `metrics/`, `export/lo1-fields-v2/`.

**Facts.** `metrics.run()` (line ≈208) calls `measure()`; the query at line ≈103 (`base = con.execute(...)`) raised
DuckDB `OutOfMemoryException` at the 1 GiB guard cap on 2026-09-30 02:38 UTC. `metrics.py:204` opens
`C.connect(memory="600MB", threads=2)` (in-memory). The same failure in `borrow_proxy` was fixed with a file-backed
scratch database and one thread (`borrow_proxy.py:58`: `S.connect("borrow", memory="560MB", threads=1,
db_file="borrow.duckdb")`); `common.connect` also accepts `db_file`.

**Steps.**
1. Make `metrics` fit a 1.0 GiB guard: file-backed scratch (`C.connect(memory="560MB", threads=1,
   db_file="metrics.duckdb")`), and, if the query still exceeds memory, restructure it (materialize per year or per
   month into the scratch database, then aggregate). Keep the output schema and numbers unchanged.
2. Run under the guard (cap 1.0): `-m atx_db.alpha_panel.metrics`; retry on 137/78.
3. Run the lo1 aligned export under the guard (cap 1.0):
   `-m atx_db.alpha_panel.export_impl align --role C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v2-lo1 --out data/alpha_panel/v1/export/lo1-fields-v2`.
4. **Consumer-contract fix in `export_impl.py`** (Task 1 also owns this file): `align()` writes the fields manifest
   `role` block with keys `d`, `n`, `first`, `last` (`export_impl.py:≈356`), but the consumer requires `role.dates`
   and `role.instruments` (`C:/atx-wt/pool-2/atx-impl/src/strategy_ic_admission.cpp:334-336`; the `export()` path at
   `export_impl.py:≈220` already writes them) — make `align` write the same role block as `export()` (keep the old
   keys too only if the consumer tolerates extra keys; check the consumer code). Tighten the seal: `align` and
   `export` refuse any role or window reaching 2024-01-01 (consumer OD-1; today `align` refuses 2025-01-01 at
   `export_impl.py:≈257` and `export` defaults `--end 2024-12-31`: change the default to 2023-12-31). Fixture tests
   for both. Then rerun step 3.
5. Run the consumer acceptance load exactly as written in the last section of
   `atx-db/docs/ALPHA_PANEL_REQUEST_V7_RESPONSE.md` (read it; use the guard for any data-heavy command). Do not edit
   consumer code (atx-impl / atx-engine / `C:/atx-wt/**`); if the load fails, report the exact error and its cause.

**Done when:** metrics exits 0 under the 1.0 GiB guard with the stage manifest written; the export exits 0 with its
manifest whose `role` block carries `dates` and `instruments` equal to the role's; the acceptance load's output is in
the report (PASS, or FAIL with root cause). Tests for the changed metrics and export logic pass.

### Task 2: Fundamentals v10 full build, validation, export v2

**Why.** Largest silver coverage lever for fundamental features: cross-concept Q4/TTM repair, H.10 FX conversion of
non-USD filers (≈5.5% of member cells' money items), more items, FSDS label fallback.

**Owns:** stages `fundamentals_v10/`, `export/fundamental-events-v2/`; fixes inside `fund_*.py` /
`fundamentals.py` only when a build step fails on real data (fixture test required for each fix). Must not modify
the published `fundamentals/` (v9) stage.

**Facts.** The FUND lane's report `.superpowers/sdd/tier1-v3/task-FUND-report.md` holds the exact build, validate
and export commands (`fundamentals build-all` with `ATX_FUND_STAGE=fundamentals_v10`, validate, export v2) and the
sample measurements (missing TTM sale/oi/gp on 400 issuers: v9 2.39/1.38/1.88% → v10 1.16/0.79/1.33%). Every
earlier full-build attempt was stopped by the guard for host commit memory (job peaks 0.19-0.52 GiB), not by a bug.

**Steps.** Read the FUND report; run the v10 build under the guard (cap from the report, ≤ 0.8), resuming after
137/78 stops; run the validation; run the export v2 and its verify step.

**Done when:** `fundamentals_v10/` manifest written with rc 0; validation output in the report with the member-cell
missing shares of `sale_ttm`, `oi_ttm`, `gp_ttm`, `at`, `be`, `ni_ttm`, `cfo_ttm` for 2020-2025 (v9 vs v10, measured
on the same member cells) and the count of `fx_converted` rows; `export/fundamental-events-v2/` written and its
verify step exits 0. v10 missing shares for `sale_ttm`/`oi_ttm`/`gp_ttm` must not exceed v9's.

### Task 3: Labels stage and IC evaluator v2

**Why.** The measurement layer every gold decision rests on; D6 enforced in code.

**Owns:** new `atx-db/src/atx_db/alpha_panel/labels.py`, new `atx-db/src/atx_db/alpha_panel/ic_eval.py`, tests
`atx-db/tests/test_alpha_panel_labels.py`, `atx-db/tests/test_alpha_panel_ic_eval.py`, stages `labels/`,
`validation/ic_v2*.json`. Leaves `evaluate.py` in place.

**labels.py.** Output `labels/year=YYYY/labels.parquet`, columns `session_date DATE, security_id BIGINT, fwd_1,
fwd_5, fwd_21, fwd_63 DOUBLE, n_5, n_21, n_63 SMALLINT` (count of non-missing daily returns in the window). Rules:
Global Constraint 3; a label is NULL when fewer than `ceil(0.9·h)` daily returns exist in its window, except when the
line delists inside the window: then the window stops at the delisting session, includes the delisting return if
the `delisting/` stage provides one for that line (read `delisting.py` for the column), and counts as complete
(CRSP convention: proceeds held in cash, return 0 afterwards). Decision sessions 2019-01-02 onward. **Holdout guard (Global Constraint 1):**
a module constant `LABEL_CUTOFF = date(2023, 12, 29)`; a label is written only when session d+1+h ≤ LABEL_CUTOFF;
the build asserts the maximum label-end session ≤ LABEL_CUTOFF before publishing and never reads panel rows dated
after LABEL_CUTOFF (filter in the scan). Session arithmetic uses `calendar.parquet` (`common.calendar_path()`).

**ic_eval.py.** CLI `python -m atx_db.alpha_panel.ic_eval --features <stage>[:<col,...>] --out <json>`
(`<stage>` = `characteristics` or `gold`). For each feature column and horizon h ∈ {1, 5, 21, 63}, over the
DISCOVERY and TRAIN windows separately (Global Constraint 2) and per calendar year: sessions used, mean names per
session, mean IC, NW t (lag h; h = 1 → 5), share of sessions with IC > 0, industry-neutral and risk-adjusted mean IC
and NW t (Global Constraint 5), for both universes (`member_equity`, `lo`). The risk-adjustment inputs
(`beta_252`, `vol_63`, `log_adv63`) are read from the `characteristics` stage (Task 4 provides them; until then the
option is skipped with a logged notice). `--overlap-with <stage>:<col,...>` adds, per feature, the max mean |Spearman|
(TRAIN month-ends) against the listed columns and which one (used to flag overlap with the consumer's admitted
library). Also, over all sessions 2019-01-02 .. last panel session (non-return statistics): coverage =
mean daily share of `member_equity` lines with a non-null value, per calendar year 2019-2026; rank autocorrelation at
lag 1 and lag 21 sessions (mean of daily Spearman between the feature on d and d-k among lines present on both). Pairwise
redundancy: mean |Spearman| between feature pairs on the last session of each TRAIN month. The module refuses (raises)
if any label row it reads has label-end > LABEL_CUTOFF or decision session > TRAIN end. Memory: process features in
batches (≤ 16 columns per pass), never all columns × all sessions in RAM; must run under a 0.8 GiB guard for 150
features.

**Tests (fixtures, synthetic).** Label arithmetic (h-window sums from d+2, 0.9·h rule, delisting truncation);
D6 cutoff (no row with end > cutoff; build refuses a panel read past cutoff); rank IC equals scipy-free reference
Spearman on a toy cross-section with ties; NW t against a hand-computed value; industry-neutral IC on a toy with a
pure industry effect gives ≈ 0; refusal when a label beyond cutoff is present.

**Done when:** tests pass; `labels/` built for decision sessions 2019-01-02 .. 2023 under the guard (report rows per
year, share of `member_equity` cells with non-null `fwd_5`/`fwd_21`/`fwd_63`, max label-end session ≤ 2023-12-29);
`ic_eval` runs end to end on real data as a smoke test with `--features panel:ret_overnight,ret_intraday` (the
evaluator must also accept `panel` as a feature stage) and its JSON path is in the report.

### Task 4: Characteristics v2 part A — registry, port to panel v2, PIT harness, market/price/volume/options/short/ownership families

**Why.** The raw feature library the gold selection chooses from; must be point in time by construction and by test.

**Owns:** `atx-db/src/atx_db/alpha_panel/characteristics.py` (rewrite allowed), new
`atx-db/src/atx_db/alpha_panel/char_registry.py`, tests `atx-db/tests/test_alpha_panel_characteristics*.py`, stage
`characteristics/`.

**Registry.** `char_registry.py` holds one entry per feature: `name, family, sql_or_callable, prior_sign (+1/-1 applied
already, documented), citation, inputs (panel columns / stages), lookback_sessions, kind ('signal' | 'control'),
fill_zero (bool: absence means "no event" = 0), notes`. Families for part A: `momentum`, `reversal`, `low_risk`,
`liquidity`, `volume`, `options`, `short_side`, `ownership`, `seasonality`, `event_time`, plus `control` (size,
beta, vol, spread, turnover). The stage manifest embeds the registry.

**Port.** Every existing feature in `characteristics.py` (SCORECARD + EXTRA) keeps its name and definition, rewritten
against panel v2 column names (read the schema of `panel/year=2022/panel-01.parquet`). IV features follow the IV
ruling (Global Constraint 4). The vendor `earn_flag_vendor` must not be used for event timing if its `-1` state marks
the session before a future announcement (check the data: if `-1` precedes `0`, it is look-ahead); use the SEC-based
`earn_recent` / `earn_last_reaction_session` / `earn_day_offset` columns instead.

**New features (part A)**, each prior-signed, with citation in the registry:
- momentum: `mom_6_1`, `mom_12_7` (Novy-Marx 2012), `mom_1_0` excluded (that is reversal), `tsmom_12` sign × vol-scaled,
  `frog_in_pan` (Da, Gurun & Warachka 2014: sign(PRET)·(%neg − %pos) days over t-251..t-21, prior sign −),
  `mom_overnight_12` (sum of `ret_overnight` t-251..t-21; Lou, Polk & Skouras 2019), `high_52w` kept,
  `ind_mom_6_1`, `sector_mom_ff12_12_1`;
- reversal: `rev_5`, `rev_21` (kept), `intraday_rev_5` (minus 5-session sum of `ret_intraday`), `ind_adj_rev_5/21`
  (kept), `overnight_intraday_tug_21` (Lou, Polk & Skouras 2019: overnight minus intraday 21-session sums, prior −
  intraday);
- low_risk: `low_beta`, `low_ivol` (21 and 63-session), `low_max` (kept), `low_skew_63` (minus 63-session skewness of
  daily returns; Bali, Engle & Murray), `low_coskew_252`, `downside_beta_252` (prior −), `low_vol_126` (consumer
  wish list `vol_126`), `low_coskew_60m` (Harvey & Siddique 2000 coskewness on 60 monthly returns; consumer wish list
  `coskew_60m`). Features whose lookback exceeds the panel's 2018-01-02 start (60 months, seasonality years 2-5) read
  the `prices/` stage (2012+, same `security_id`, same `ret`/guard semantics) instead of the panel;
- liquidity/volume: `amihud_21` (kept), `abn_turnover` (kept), `vol_of_volume_63` (minus CV of dollar volume;
  Chordia, Subrahmanyam & Anshuman 2001), `volume_trend_21_252`, `abn_volume_5` (Gervais et al. 5-session),
  `zero_return_days_63` (Lesmond), `hl_spread_21` (control);
- options (IV ruling applies): `iv_term_slope`, `iv_change_21`, `iv_rv_ratio`, `iv_rv_spread` (kept), `iv_level_21`
  (minus 21-day ATM IV), `iv_change_5` (minus 5-session change in 21-day ATM IV);
- short_side: `si_ratio`, `si_change`, `dtc`, `si_to_adv`, `short_vol_ratio_5/21` (kept), plus from panel/borrow
  columns: `ftd_to_shares` (minus FTD quantity over shares_out, visible clock as in the panel), `regsho_threshold`
  (minus 1 when the line is on a Reg SHO threshold list at d, fill_zero), `regsho_run_days` (minus log(1+run days)),
  and the `borrow_proxy/` stage's proxy columns joined on (session_date, security_id) if their clock is ≤ d (read
  `borrow_proxy.py` for names);
- ownership (13F, panel `inst_*` columns, clock as published in the panel): `io_ratio` (inst_shares / shares_out),
  `io_change_q` (inst_pct_change), `breadth_change` (inst_d_holders / lagged holders; Chen, Hong & Stein 2002, +),
  `io_concentration` (inst_top10_share);
- seasonality: `seas_same_month` (kept), `seas_annual_avg_2_5` (Heston & Sadka: mean return in the same calendar
  month over years 2-5, only years available at d);
- event_time: `earn_days_since` (control), `ea_window_ahead_5` (1 when the next expected announcement
  `earn_next_expected_date` falls in sessions d+2..d+6, fill_zero; Frazzini & Lamont 2007, Barber et al. 2013,
  prior +) — only if `earn_next_expected_date` is point in time (verify how the earnings calendar stage computes it
  and cite the lines in the report; if it uses future information, drop the feature).
- control: `log_me`, `log_me_line`, `beta_252`, `vol_21`, `vol_63`, `vol_252`, `ivol_21`, `turnover_21`,
  `hl_spread_21`, `log_price` (ln raw_close), `log_adv63` (ln of the panel's `adv63`). Controls are stored with their
  natural sign (not prior-signed) and `kind='control'`.

**PIT harness (test + script).** A function that, for one line bucket and a decision session d, recomputes the
features from the panel rows with `session_date ≤ d` only and compares them with the full build at d: every value
equal (|Δ| ≤ 1e-9 relative) or both NULL. A fixture test runs it on a synthetic panel, including a feature built
with a forbidden `lead` to prove the harness catches look-ahead. A real-data run on 3 decision sessions
(2020-11-02, 2021-06-15, 2022-10-03) and 2 buckets is recorded in the report.

**Build.** Years 2019-2026 under the guard (cap ≤ 1.0); line pass by bucket, year pass per year (existing structure is
fine). Output `characteristics/year=YYYY/characteristics.parquet` keyed `(session_date, security_id)` with `member`,
`member_equity`, `grp_ff49`, `grp_ff12` and every registry feature.

**Done when:** tests pass; ≥ 75 features in the registry after part A; build rc 0 for 2019-2026 with manifest; report
lists per feature the 2020-2026 mean daily coverage of `member_equity` (non-return statistic); real-data PIT harness
PASS on 3 sessions × 2 buckets; `ic_eval` (Task 3) run on `characteristics` with its JSON path in the report.

### Task 5: Characteristics v2 part B — fundamentals, accounting, distress, events, insider families

**Why.** Fundamental and event families carry most documented IC at 21-63 day horizons.

**Owns:** same files as Task 4 (runs after it), plus read-only use of stages `fundamentals/` (or the panel's
fundamental columns), `insider_ext/`, `events/governance`, `events/capital`.

**New features (part B)**, prior-signed, registry entries with citations (the panel's fundamental columns are
already point in time with a one-session lag; any stage read directly must be joined as of its `available_at <
22:00 UTC of d-1`):
- value: `bm`, `ep`, `cfp`, `sp`, `ebit_ev`, `fcfp`, `dvc_yield`, `net_payout` (kept), `ebitda_ev`, `sales_ev`,
  `bm_ind_adj` (bm minus FF49 median on the session);
- profitability/quality: `gpa`, `opbe`, `roa`, `roe_q`, `cfoa`, `fscore` (kept), `cop_at` (cash-based operating
  profitability, Ball et al. 2016), `gross_margin`, `roic`, `accrual_quality` proxy `-(|accruals|)`, `mohanram_g`
  (subset computable from available items; document which of the 8 signals are used), `qmj_profit` (equal-weight z of
  gpa, roe, roa, cfoa, gross_margin, -accruals within the session);
- growth/investment: `asset_growth`, `noa_at`, `capx_at` (kept), `sale_growth_q` (kept, prior −), `inv_growth`
  (minus inventory growth), `ppe_inv_at` (minus Δ(ppegt+invt)/lag at; Lyandres et al.), `d_capx_2y`;
- earnings momentum: `sue` (kept), `sue_sales` (standardized unexpected revenue: (sale_q − sale_q_lag4) / stdev of
  that difference over the last 8 quarters when available, else 4; Jegadeesh & Livnat 2006), `ear` (kept), `droe`,
  `chtax` (kept), `d_gross_margin_q`, `earnings_streak` (count of consecutive positive seasonal EPS changes);
- issuance/payout: `issuance_xbrl`, `issuance_vendor` (kept), `buyback_yield` (prstkc_ttm / me), `net_issuance_12m`;
- distress: `o_score` (Ohlson 1980, prior −), `chs_proxy` (Campbell, Hilscher & Szilagyi 2008 terms computable
  from available fields: NIMTA, TLMTA, EXRET, SIGMA, RSIZE, CASHMTA, MB, PRICE; prior −), `altman_z` (prior +);
- leverage/liquidity (controls or signals per literature): `leverage`, `cash_at` (kept), `current_ratio`,
  `interest_coverage`;
- intangibles: `rd_me` (kept), `rd_sale`, `xsga_at` (Eisfeldt & Papanikolaou organization capital proxy);
- insider (`insider_ext/`, share-based measures only; value sums are polluted): `insider_net_buy_6m` (net shares
  bought by insiders over 6 months / shares_out), `insider_buyers_6m` (number of distinct buying insiders, fill_zero),
  `insider_opportunistic_buy` (Cohen, Malloy & Pomorski 2012 opportunistic net buying, fill_zero);
- events (`events/capital`, `events/governance`; precision not hand-checked, flag in notes): `seo_recent_252`
  (minus 1 if a follow-on offering in the last 252 sessions, fill_zero), `nonreliance_402` (minus 1 on an 8-K Item
  4.02 non-reliance event in the last 252 sessions, fill_zero; consumer wish list), `ceo_change_recent_126` (5.02;
  sign per literature, document), `shelf_recent_126`, `nt_late` (minus 1 when an NT 10-K / NT 10-Q was filed in the
  last 252 sessions, fill_zero; consumer wish list; source: the `sec_filings/` stage — read `sec_filings.py` for the
  form column and clock);
- consumer wish-list fundamentals (build each only if the inputs exist; otherwise record why not): `ceq_iss_5y`
  (Daniel & Titman 2006 composite equity issuance: 5-year log growth of market equity minus 5-year log stock return,
  prior −; reads `prices/` for the 5-year return), `xrd0_ttm` (1 when R&D is reported zero or absent,
  `xrd_reported_zero`), `tax_book` (Lev & Nissim 2004 tax-to-book income ratio from the current/deferred income tax
  items if the fundamentals stages carry them).

**Done when:** tests pass (one fixture test per new formula family, covering NULL/zero-denominator handling and the
as-of join clock); ≥ 130 features total in the registry; rebuild rc 0 for 2019-2026; real-data PIT harness PASS on
the same 3 sessions × 2 buckets; coverage table for the new features; `ic_eval` JSON for all features.

### Task 6: Cover-page build and identity v3 rerun (PIT link coverage)

**Why.** Point-in-time issuer links cover 93.8-94.9% of `member_equity` cells 2019-2025; the gap (mostly foreign
private issuers) blocks fundamental features. Cover-page tagging closes most of it.

**Owns:** stages built by `cover_page.py` and `identity_v3.py` (`identity/link_table_v3.parquet`,
`export/identity-bridge-v3-*`); fixes inside those two modules only with fixture tests.

**Facts.** `.superpowers/sdd/tier1-v3/task-NOTES-report.md` (cover_page build commands, notes/ data landed) and
`task-ID-report.md` (identity_v3 commands, gates) hold the exact commands. Open decision (a) in
`atx-db/docs/TIER1_V3_STATUS.md` about the S2.1 2020 denominator is ruled here: measure the gate on linked CIKs
AND on ever-member CIKs, report both.

**Done when:** cover page stage built rc 0; identity_v3 rerun rc 0; PIT tier coverage of `member_equity` cells per
year 2019-2025 reported (before/after), 0 ambiguous line-days; bridge exports rewritten with manifests.

### Task 7: Gold selection, gold stage, family composites, IC report (first pass on panel v2)

**Why.** Turns the library into the consumer-facing gold panel.

**Owns:** new `atx-db/src/atx_db/alpha_panel/gold.py`, tests `atx-db/tests/test_alpha_panel_gold.py`, stage `gold/`,
`atx-db/docs/ALPHA_PANEL_GOLD.md`, `atx-db/docs/ALPHA_PANEL_IC.md` (generated).

**Selection (reads the Task 3 IC JSON only; TRAIN statistics decide, DISCOVERY confirms).** A `signal` feature is
gold when all hold:
1. coverage (2020-01-02 .. last session, mean daily share of `member_equity`) ≥ 0.50 and ≥ 0.30 in every calendar
   year 2020-2026 (`fill_zero` features count filled zeros as covered);
2. for at least one h ∈ {5, 21, 63}: TRAIN prior-signed mean IC > 0 with NW t ≥ 2.5, using the raw or the
   industry-neutral IC (the gold score takes the variant that passed; if both, the larger t);
3. the DISCOVERY mean IC at that h, same variant, is > 0;
4. not redundant: walking candidates in decreasing max TRAIN t, drop a candidate whose mean |Spearman| with an
   already selected gold feature is ≥ 0.90.
`control` features are always shipped (as `ctl_<name>`), never selected on IC. Selection output
`gold/selection.json` records every candidate's pass/fail per rule.

**Gold scores.** For each session and gold feature, among `member_equity` lines with a value: winsorize at the 1st and
99th percentiles, rank (average ties) to u = (rank − 0.5) / n, z = Φ⁻¹(u); for the industry-neutral variant subtract
the FF49 group mean of z (groups < 3 lines: subtract 0), then re-standardize to unit cross-sectional std. `fill_zero`
features: missing → the score of value 0 in that session's distribution. Column `g_<name>` (FLOAT).
**Composites.** For each family with ≥ 2 gold features: `c_<family>` = mean of the available `g_*` of that family
(≥ half present, else NULL), re-standardized cross-sectionally; `c_all` = mean of available family composites
(≥ 3 present), re-standardized. Equal weights only (no fitted weights).
**Stage.** `gold/year=YYYY/gold.parquet`, rows = `member_equity` lines, sessions 2020-01-02 .. last panel session;
columns `session_date, security_id, cik, grp_ff49, grp_ff12, g_*, c_*, ctl_*`; manifest with the registry subset,
selection SHA, IC JSON SHA and input manifests.

**IC report.** Run `ic_eval` on `gold` (all `g_*` and `c_*`), then generate `docs/ALPHA_PANEL_IC.md`: per feature
and composite, TRAIN and DISCOVERY IC and NW t at h = 5, 21, 63 (raw, industry-neutral, risk-adjusted), the `lo`
universe IC at h = 21, IC per calendar year 2019-2023 at h = 21, coverage, rank autocorrelation (lag 1, 21), family,
and overlap with the consumer's admitted v7.1 library (max |ρ| and which member; the admitted names are in
`C:/atx-wt/pool-2/build-equity/mega-weights-v71-ew/admission.csv`, mapped to the same-named characteristics where
they exist; a feature with max |ρ| < 0.5 is marked `novel`); sorted by family then t. Header states the windows and
the holdout rule.

**Tests.** Selection rules on a synthetic IC JSON (each rule's boundary); normalization (winsorize, Φ⁻¹, ties,
fill_zero, industry demeaning with small groups); composite presence thresholds; the stage never contains a
non-`member_equity` row.

**Done when:** tests pass; `gold/` built rc 0 for 2020-2026; `selection.json` and the IC report written; report
states the number of gold features per family and `c_all` TRAIN IC / NW t at 5, 21, 63.

### Task 8: Panel v3 — bind fundamentals v10 and link table v3; re-assemble

**Why.** Brings Task 2 and Task 6 into the panel so gold features use FX-converted, better-covered fundamentals and
point-in-time issuer links.

**Owns:** `atx-db/src/atx_db/alpha_panel/panel.py` (identity and fundamentals inputs, and an output-stage option),
its tests, new stage dir `panel_v3/`. **Ruling P3:** the gold lane reads `panel/` concurrently, so this task never
writes `panel/`: it assembles into `panel_v3/` (same layout and manifest scheme; output stage selectable by flag or
env, default `panel`). The swap (`panel/` → `_archive_panel_v2/`, `panel_v3/` → `panel/`) and the metrics rerun
happen at the start of Task 9, when no job reads the panel.

**Rules.** Fundamentals input configurable (env or flag) and defaulting to `fundamentals_v10/`; issuer links from
`identity/link_table_v3.parquet` PIT tiers (strict, dated, name) with `link_tier` carried; `backfill` tier no
longer used for issuer joins (survivorship). Column names and types unchanged except new columns appended
(`fx_converted`, `link_basis` if needed). Re-assemble 2018-2021 and 2022-2026 (one heavy job at a time).

**Done when:** `panel_v3/` rc 0 for 2018-2026 with manifest; `panel/` untouched (manifest SHA unchanged); report the
per-year share of `member_equity` cells with non-null `cik`, `be`, `sale_ttm`, `ni_ttm` (`panel/` vs `panel_v3/`);
schema diff (only appended columns); row counts per year equal between the two (same price spine).

### Task 9: Gold v3 rebuild, consumer export, docs and notice

**Why.** Delivers the final gold panel to atx-impl.

**Owns:** reruns of stages `characteristics/`, `gold/`, IC reports; `export_impl.py` (a gold-fields mode only if the
existing `align` cannot carry `g_*`/`c_*` columns), stages `export/gold-lo1-v1/`, `export/gold-2020-2024-v1/`,
`atx-db/docs/ALPHA_PANEL_GOLD.md`, `docs/plans/2026-09-30-atx-db-gold-panel-notice.md`. `export_impl` may read the
`gold/` stage in addition to `panel/` (the smallest change: a `--stage` option on `align`/`export`).

**Steps.** First, with no job reading the panel: swap `panel/` → `_archive_panel_v2/` and `panel_v3/` → `panel/`
(directory renames), rerun `metrics` (guard 1.0) and the lo1 aligned export (Task 1 command); delete
`_archive_panel_v2/` only after this task's rebuilds pass. Then rebuild characteristics (2019-2026) and gold
(2020-2026) on panel v3; rerun `ic_eval` and selection (same
rules); write the before/after IC and coverage comparison; export gold fields (`g_*`, `c_*`, `ctl_*`; names must
match `[a-z_][a-z0-9_]{0,63}`, ≤ 1024 fields, each with `point_in_time: true` and its clock) in the consumer layout
(`atx.research-role-fields/v1`, one `<name>.f64` per field, date-major on the role's axes) (a) aligned to the lo1
role `C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v2-lo1`, and (b) aligned to the consumer's v8 lo1
role (end 2024-01-01, score start 2020-01-01) if one exists by then under `C:/atx-wt/pool-*/build-equity/` (read its
manifest), otherwise exported with its own role through `export_impl export` for sessions 2018-06-01 .. 2023-12-29
with score start 2020-01-02; each with a manifest; no session ≥ 2024-01-01 in any export. Run the consumer acceptance
load on the gold lo1 export; write `ALPHA_PANEL_GOLD.md` (definitions, clocks, IC table, selection rules, known gaps)
and the notice to the mega-alpha controller (what is new, how to load, the IV ruling, the holdout statement).

**Done when:** rebuilds rc 0; both exports written with manifests; acceptance load PASS (or FAIL with root cause);
docs and notice committed.
