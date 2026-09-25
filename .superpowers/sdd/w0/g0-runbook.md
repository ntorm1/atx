# G0 truth-delta run-book (prepared read-only, 2026-09-24)

Prepared for the orchestrator of the alpha-engine swarm, wave W0. It covers the "G0 · Truth-delta report" section of
`C:\atx-wt\pool-1\docs\plans\2026-09-24-alpha-engine-production-swarm.md` (lines 541-551). Nothing was built, run or
written while preparing it, apart from this file.

Sources read:
- the plan, the findings (`docs/plans/2026-09-24-alpha-engine-review-findings.md`), the goal prompt and `.superpowers/sdd/w0/*`, all from `C:\atx-wt\pool-1`;
- the qps archive `C:\atx-wt\pool-1\.superpowers\sdd\w0\qps-archive\pool-{8,10,11}\SWARM_STATUS.md`;
- the artifact manifests under `C:\atx\data` (metadata only; no data row dated 2020 or later);
- the old run scripts and logs in the earlier session scratchpads (`...\claude\c--atx\d4113e57-...\scratchpad`, `...\adf9c313-...\scratchpad`);
- the cells receipts in `C:\atx\.worktrees\equity-platform\build-equity\audits` (read-only; git was never run there).

---

## 0. Read this first

### 0.1 W0 code state when this was written

- Every W0 lane branch (`feat/w0-a0`, `-b0`, `-d0`, `-e0a`, `-e0b`, `-i0a`, `-i0b`, `-l0`, `-r0`) still points at the base commit
  `458d0bef`. Only `feat/w0-o1-l6` has a commit (`1de5814f`, the lane-6 merge).
- None of the new CLI flags exist yet. Every flag marked **`<TBD-I0b>`** / **`<TBD-B0>`** below is inferred from the lane briefs.
  Confirm the exact names from `lane-i0b-report.md` and `lane-b0-report.md` ("Integration notes") before running.
- G0 base = the W0 gate SHA on `feat/w0-integration`. Record it in every artifact name and in the report.

### 0.2 The code that produced the old numbers is already in `main`

| Result | Producing code | In `main` (`2e0d738f`)? |
|---|---|---|
| L9 | `c8d7702c` | yes |
| L10 | `6b949d7f` | yes |
| L7 | `05e83cf4` | yes |

`git diff <lane-head> HEAD` is empty for every file those runs used:
- `stage_equity_mine.*`, `fixtures/alpha101.txt`;
- `fundamental_zoo_test.cpp`, `fixtures/fundamental_zoo.txt`, `fundamental_fields.hpp`;
- the risk sources and `risk_model_real_scorecard_bench.cpp`.

So any old-vs-new delta on these three is W0 plus O1, nothing else.

The cp21 cells and the 2013 baseline were built from the equity-platform worktree. That code was committed into `main` as
`092314d4` "through checkpoint 22", with one difference that matters for cp21 (see G0-3).

### 0.3 Safety and output location

- The goal prompt forbids writing under `C:\atx`, and that includes `C:\atx\data`. The old artifacts all live in `C:\atx\data`,
  but the G0 outputs need a root outside `C:\atx`. Proposal (orchestrator decides):
  ```powershell
  $G0 = 'C:\atx-wt\g0-data'     # outside C:\atx and outside every git worktree
  #  $G0\data\    new artifact dirs, flat (the same sibling layout as C:\atx\data; equity-ic needs it, see G0-3)
  #  $G0\ledger\  fresh trial ledger(s)
  #  $G0\logs\    stdout/stderr/summary per run
  #  $G0\bin\     sha256 of every binary used
  $D  = 'C:\atx\data'           # READ-ONLY inputs
  ```
- Every input below is dated before 2020-01-01; the per-item manifests confirm this. No G0 command touches
  `atx-db\data\warehouse.duckdb` or any `.bak`. The L10 export is **not** re-run.

### 0.4 The 2019 data

Owner ruling R-1 retires the "2019 holdout" label: 2013-2019 is development data. Two old runs read 2019:
- **L9** loaded and scored the 2019 holdout (`--holdout publish --holdout-prior-reads 3`; `gate_report.holdout.status = "reused"`).
- **cp21** had two 2019 cells (`equity_scorecard21_ic_2019_t{1000,3000}_20260922`) inside its pooled headline.

L10, L7 and the 2013 baseline never loaded 2019.

Recommendation:
- **L9:** `--holdout off`. Nothing in G0 needs the 2019 number, and I-24 concerns the *validation* blend, which does not depend on the holdout.
- **cp21:** keep the 2019 cells. The old pooled headline includes them, and dropping them changes the quantity being compared.

The goal prompt says "stop and ask on any data-discipline question", so get a one-line owner acknowledgement first.

### 0.5 Timing and peak-memory wrapper

Only L9 and cp21 recorded peak RAM. Wrap every G0 run in this function (PowerShell 5.1). It is modeled on the old
`l9fix_real_run.ps1`, which logged `exit=` blank because it never touched `$p.Handle`:

```powershell
function Invoke-G0([string]$Name, [string]$Exe, [string[]]$ArgList) {
  $log = "$G0\logs\$Name"; New-Item -ItemType Directory -Force "$G0\logs" | Out-Null
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath $Exe -ArgumentList ($ArgList | ForEach-Object { '"' + $_ + '"' }) -NoNewWindow -PassThru `
       -RedirectStandardOutput "$log.out" -RedirectStandardError "$log.err"
  $null = $p.Handle                      # needed so ExitCode is populated
  $peak = 0
  while (-not $p.HasExited) { try { $p.Refresh(); if ($p.PeakWorkingSet64 -gt $peak) { $peak = $p.PeakWorkingSet64 } } catch {}; Start-Sleep -Milliseconds 500 }
  $p.WaitForExit()
  "exit=$($p.ExitCode) wall_s=$([math]::Round($sw.Elapsed.TotalSeconds,1)) peak_ws_gb=$([math]::Round($peak/1GB,3))" |
    Out-File -Encoding utf8 "$log.summary.txt"
}
```

- Environment variables (L10, L7) set in the same PowerShell call are inherited by `Start-Process`.
- Run `(Get-FileHash <exe>).Hash` for each binary before running, and store it in `$G0\bin\`.

### 0.6 Builds

All builds are Release, preset `equity-rel` (`build-equity-rel`). Follow RULES §1:
- set `$env:CMAKE_BUILD_PARALLEL_LEVEL='2'`;
- wait for at least 2 GB free before each build;
- invoke the wrapper by absolute path;
- build sequentially: the shared `spdlog-build` races Debug against Release (pool-11 note).

| Pool (proposal) | Tree | Targets | Cache notes (read from `CMakeCache.txt`) |
|---|---|---|---|
| `C:\atx-wt\pool-10` | W0 gate SHA, **unpatched** | `atx-impl` (L9, G0-5); `atx-engine-bench` (L7) | `FETCHCONTENT_BASE_DIR=C:/atx-wt/pool-10/deps/equity-rel`; `ATX_BUILD_BENCH=OFF` today, so reconfigure `configure -Preset equity-rel -Bench`, or use O1's `equity-bench` preset if it lands |
| `C:\atx-wt\pool-11` | W0 gate SHA | first `atx-impl-tests atx-shm-worker` (L10); then, **on a throwaway branch `g0/cp21-pin`**, `atx-impl` for cp21 | `FETCHCONTENT_BASE_DIR=C:/atx-cache/deps` (shared) |

```powershell
Set-Location C:\atx-wt\pool-10; $env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File C:\atx-wt\pool-10\scripts\atx-build.ps1 build -Preset equity-rel atx-impl
Set-Location C:\atx-wt\pool-10; $env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File C:\atx-wt\pool-10\scripts\atx-build.ps1 configure -Preset equity-rel -Bench
Set-Location C:\atx-wt\pool-10; $env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File C:\atx-wt\pool-10\scripts\atx-build.ps1 build -Preset equity-rel atx-engine-bench
Set-Location C:\atx-wt\pool-11; $env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File C:\atx-wt\pool-11\scripts\atx-build.ps1 build -Preset equity-rel atx-impl-tests atx-shm-worker
```

### 0.7 W0 flag and default changes that matter to G0

From the lane briefs.

| Change (defect) | Lane | Affects | What G0 must pass to keep "same inputs" |
|---|---|---|---|
| Boolean flag values parsed as true/false; `--config` accepted in every stage or rejected (I-10) | I0b | only `panel --compact-universe true` (contexts are **not** rebuilt in G0); `equity-mine` keeps its own `--no-*` parser | nothing (no G0 command uses a valued boolean) |
| `isfinite` checks on every double flag (I-12) | I0b | all | nothing (all old values are finite) |
| Report costs mandatory (I-11) | I0b | `report` stage / `replay_report.cpp` | `equity-baseline`: the old 2013 run already passes `--replay-trade-bps 5 --replay-annual-borrow-bps 365`. If validation also demands them for the cp19 membership cells, pass the recorded recipe values (5 / 365) |
| As-of membership mask in IC and baseline views (D-12) | I0b | `equity-baseline` and `equity-ic` on `equity_scorecard16_ctx_*` (year-union contexts) | **new input `<TBD-I0b>`**, probably `--membership C:\atx\data\equity_universe_pit_2013_2019_20260920\membership.bin` plus a cut matching the context's `universe_cut` `1000:0.00` / `3000:0.00`; membership sha256 is `776bda80…`. Labels change to `as-of` |
| `--membership` required in `equity-mine` (I-16) | I0b | L9 | already passed (`--membership … --membership-cut 0`) |
| delay < 1 rejected without `--allow-same-close` (B-02) | I0b, B0 | mine (`--delay`, default 1), baseline (`--replay-execution-delay 1`), IC (new delay) | L9 and the baselines already use 1. The old IC effective delay was **0** ("signal-at-t-return-from-t"); the G0 headline uses the new default 1. A control that reproduces old semantics needs `<TBD-I0b: --execution-delay 0> --allow-same-close` |
| `execution_delay` default 1 in `cross_section_ic`; forward return from t+delay; embargo h+delay (E-09) | E0a (+I0b wiring) | IC cells; the **L10 harness** (sets `CrossSectionIcConfig` fields itself and leaves the new field at its default) | IC: see the row above. L10: no knob, so the harness gets delay 1 silently |
| `min_names_per_date` becomes a parameter, default 50; the old `kMinNamesPerDate` was 2 (E-18) | I0b | IC cells | headline: default 50. Old-semantics control: `<TBD-I0b: --min-names-per-date 2>` |
| Block length ≥ 2h or stationary bootstrap; `BlockLenRule::V1` keeps old streams (E-02) | E0a | IC `ic_summary.json` CIs; L10 `rank_ic_h21_lo/hi` | no CLI knob known. The Python scorecard hard-codes block 5, so the cp21 scorecard CIs are **not** affected |
| Terminal-return table interface; 2013 audit no longer mandatory (I-15) | I0b | IC `IncludeAuditedTerminalV1` (the **cp21 headline variant**) | keep supplying the 2013 audit: a byte copy beside the new baseline dirs (G0-3), or `<TBD-I0b: audit/terminal-table flag>` |
| Delisting policy `TerminalReturn` is the new default (Shumway −30%/−55%, flagged); `Abort` stays as an option (B-04) | B0 | the 2013 native baseline replay (G0-5) | headline: new default. Old-semantics control: `<TBD-B0/I0b: delisting policy = Abort>` |
| nw_lags, ic_horizons and ppy recorded (I-23); manifest written before `.pending` is released (I-17) | I0b | mine, baseline | nothing |
| Registry clusters, windows and OOS flag; DSR N = number of clusters (E-01, E-16) | E0b | L9 `trials.*`, `dsr_train` | nothing (new semantics, reported) |
| Rank ties, hump, literal-only scalar slots, flat-window guard, AuditExact sums, canon collision (A-01/02/03/09/13/18) | A0 | every DSL: L9 seeds and search, L10 zoo, cp21 families (`rank(...)` in `mom_resid_vs_blend`), the baseline rank transform | nothing. The ordinal-rank fallback (`RankTies::OrdinalV1`) is a code enum with no flag. IC rank-IC is **not** affected: the stage and the harness pass `IcTieHandling::AverageRanksV1` explicitly |
| `dollar_volume`/`adv{d}`/`vwap` built from `raw_close × volume` (D-01) | D0 | L9 only (`with_alpha101_fields` runs at stage time) | nothing |
| D-03/04/05/06/08/09 (panel construction) | D0 | **none of G0.** All contexts are pre-built `context.bin` files, so these fixes show up only when contexts are rebuilt (out of scope for "same inputs") | — |
| `si_publication_lag` default (D-02) | I0b, D0 | none (no G0 context has `si_*` fields) | — |
| R-03/04/05/06 (lagged exposures, sector ids, D floor ≥ 0.1·median, winsorized cap-weighted z) | R0 | L7 only through `FactorModel::create` (the D floor at `factor_model.cpp:94`, used by the hybrid path) and any shared `exposures.hpp` helpers | nothing |

---

## G0-1 · L9 guard mining (t1000)

**Artifacts.** `C:\atx\data\equity_mine_l9_guard_20260923\` holds `gate_report.json` (headline), `validation.csv` (52 family rows),
`candidates.csv` (2,243 rows), `holdout.csv`, `library.tsv`, `trial_registry.bin`, and `manifest.json`
(schema `atx.equity-mine.manifest` v1).
- Producer exe sha256 `5b77a57d9a2c28bd8af1482767fba6f67fcb05eb21bff2d2b2f249d3107296cd`.
- Supersedes the unguarded `equity_mine_l9_20260923`.

**Old command, exact.** From `...\d4113e57-035d-400e-b188-3eb963457e54\scratchpad\l9fix_real_run.ps1`, run from pool-10 at `c8d7702c`
with the Release binary; it matches `gate_report.config`:

```text
C:\atx-wt\pool-10\build-equity-rel\bin\atx-impl.exe equity-mine
 --train-contexts "C:\atx\data\equity_scorecard16_ctx_2013_t1000_20260920\context.bin;C:\atx\data\equity_scorecard16_ctx_2014_t1000_20260920\context.bin;C:\atx\data\equity_scorecard16_ctx_2015_t1000_20260920\context.bin;C:\atx\data\equity_scorecard16_ctx_2016_t1000_20260920\context.bin"
 --validation-contexts "C:\atx\data\equity_scorecard16_ctx_2016_t1000_20260920\context.bin;C:\atx\data\equity_scorecard16_ctx_2017_t1000_20260920\context.bin;C:\atx\data\equity_scorecard16_ctx_2018_t1000_20260920\context.bin"
 --holdout-contexts "C:\atx\data\equity_scorecard16_ctx_2018_t1000_20260920\context.bin;C:\atx\data\equity_scorecard16_ctx_2019_t1000_20260920\context.bin"
 --membership C:\atx\data\equity_universe_pit_2013_2019_20260920\membership.bin --membership-cut 0
 --train-start 2013-01-01 --validation-start 2017-01-01 --holdout-start 2019-01-01 --holdout-end 2020-01-01
 --fixture C:\atx-wt\pool-10\atx-impl\tests\fixtures\alpha101.txt
 --population 192 --generations 15 --threads 2 --cost-bps 5 --min-names 100 --max-validate 100 --max-corr 0.7
 --gate by --smooth-windows "5;10" --fdr-q 0.10 --rw-alpha 0.10 --n-boot 1000
 --holdout publish --holdout-prior-reads 3 --max-working-bytes 5000000000
 --out C:\atx\data\equity_mine_l9_guard_20260923
```

Implicit defaults, recorded in `gate_report.config` and `stage_equity_mine.cpp:1078-1110`:
- `--seed 20260923`, `--delay 1`, `--seal 2020-01-01`;
- `--min-coverage 0.5`, `--mean-block 10`;
- return guard on (`--max-abs-log-return 1.5`, `--adj-raw-log-tol 0.10`);
- literature seeds, search, fidelity, semantic canon and output dedup all on.

**Inputs.** Dates are from each context manifest's `axes.session_keys`.

| Role | Path | Artifact id (prefix) | Sessions | Instruments |
|---|---|---|---|---|
| train | `…\equity_scorecard16_ctx_2013_t1000_20260920\context.bin` | `2a8d7a4a` | 2012-03-26 .. 2013-12-31 | 1228 |
| train | `…_2014_t1000_…` | `995eb8cc` | 2012-12-24 .. 2014-12-31 | 1254 |
| train | `…_2015_t1000_…` | `b23bac6c` | 2013-12-23 .. 2015-12-31 | 1273 |
| train + val | `…_2016_t1000_…` | `6ccd4925` | 2014-12-22 .. 2016-12-30 | 1251 |
| val | `…_2017_t1000_…` | `854f2740` | 2015-12-22 .. 2017-12-29 | 1697 |
| val (+ old holdout) | `…_2018_t1000_…` | `f7b36440` | 2016-12-22 .. 2018-12-31 | 1264 |
| **old holdout only** | `…_2019_t1000_…` | `352db9b1` | 2017-12-22 .. **2019-12-31** | 1254 |
| membership | `C:\atx\data\equity_universe_pit_2013_2019_20260920\membership.bin` | sha `776bda80…` | `seal.json`: latest_attached 2019-12-31, rank_end 2019-11-29, "no segment at/after 2020-01-01" | cut 0 = top1000_b0 |
| fixture | `<pool>\atx-impl\tests\fixtures\alpha101.txt` (unchanged since `3c72098a`) | — | — | — |

- Everything is before 2020.
- The old run **did load and score 2019** (4th load / 3rd score of that period, per the pool-10 archive).
- The membership file carries 2019 membership rows. The stage reads the whole file in both variants.

**Old headline metrics.** Source is `gate_report.json` unless noted.

| Metric | Old value |
|---|---|
| counts | seeds 378 (invalid 0) → candidates 2,243 (degenerate 178) → scored 2,065 → family 52 (corr-rejected 1,099) → admitted **0** |
| search | digest `729454a1ea25f532`; trial_count 1,941; fidelity evals 2,594, rejected 1,718; fingerprint hits 45. The unguarded run has the **identical digest**, so the search is deterministic at seed 20260923 / threads 2 |
| trials | n_eff **5.714** (uncorrected 5.757); n_raw 2,065; pnl_len 1,008; SR per period: mean −0.01406, max 0.09213, var 0.011463; registry_hash `43add7981a12592d` |
| family blend, **validation** | net SR **−1.2187**, gross −0.1311, p_one_sided 0.9503, t_NW −1.648, mean net 0.853 bps/day **loss**, turnover 0.1523, IC h1/h5/h21 −0.00082 / 0.00058 / 0.01668, coverage 0.988, mean names 991; p_BY 1, p_RW 1; not admitted |
| family blend, holdout 2019 (reused) | net SR 1.7297, gross 2.3003, p 0.0620, t_NW 1.538, turnover 0.0945 |
| family, validation (from `validation.csv`) | mean val net SR **−0.479**; 10 of 52 positive; min p_raw 0.1788; min p_BY 1.0; best val net 0.584 (rank 10) |
| family, train | mean train net 0.522 (52 of 52 positive); top train net 1.4625 (`ema(reverse(adv60), 28)`) → val −0.105 |
| return guard, cells excluded (span / realized window) | train 17 / 14; validation 13 / 4; holdout 4 / 0 |
| wall / peak | **877.5 s / 0.963 GB** (`l9fix_real_run.log`, UTF-16; pool-10 archive). Stage log: train 848 s, validation 19 s, holdout ≈ 10 s |

**Binary.** Release is required: pool-10, `equity-rel`, target `atx-impl` (§0.6).

**W0 changes that move it:**
- **D-01** (the adv/dollar_volume/vwap fields in `with_alpha101_fields`; many train winners are adv proxies);
- **A-01, A-02, A-03, A-09, A-13, A-18** (search trajectory and evaluation; A-03 may raise `seeds_invalid` above 0 or reject crossovers);
- **E-01, E-16** (n_eff and DSR semantics, registry format);
- **I-23** (recording only).

Expect a new search digest, so compare aggregates, not candidate by candidate. B-02 and I-16 change nothing here: delay 1 and `--membership` were already passed.

**G0 command (recommended, holdout closed).** `--holdout-start` stays: it bounds the validation window.
`--holdout-contexts`, `--holdout-end` and `--holdout-prior-reads` are dropped, so no 2019 context is passed. Defaults are pinned explicitly.

```powershell
$EXE = 'C:\atx-wt\pool-10\build-equity-rel\bin\atx-impl.exe'
function ctx([int]$y) { "$D\equity_scorecard16_ctx_${y}_t1000_20260920\context.bin" }
Invoke-G0 'l9_g0' $EXE @('equity-mine',
  '--train-contexts', ((2013..2016 | % { ctx $_ }) -join ';'),
  '--validation-contexts', ((2016..2018 | % { ctx $_ }) -join ';'),
  '--membership', "$D\equity_universe_pit_2013_2019_20260920\membership.bin", '--membership-cut', '0',
  '--train-start', '2013-01-01', '--validation-start', '2017-01-01', '--holdout-start', '2019-01-01',
  '--holdout', 'off', '--seal', '2020-01-01',
  '--fixture', 'C:\atx-wt\pool-10\atx-impl\tests\fixtures\alpha101.txt',
  '--population', '192', '--generations', '15', '--threads', '2', '--seed', '20260923', '--delay', '1',
  '--cost-bps', '5', '--min-names', '100', '--max-validate', '100', '--max-corr', '0.7',
  '--gate', 'by', '--smooth-windows', '5;10', '--fdr-q', '0.10', '--rw-alpha', '0.10', '--n-boot', '1000',
  '--max-abs-log-return', '1.5', '--adj-raw-log-tol', '0.10',
  '--max-working-bytes', '5000000000', '--out', "$G0\data\equity_mine_l9_guard_g0_<W0sha8>")
```

- **Exact replay variant (only on an owner ruling):** add `--holdout-contexts "<ctx 2018>;<ctx 2019>" --holdout-end 2020-01-01 --holdout publish --holdout-prior-reads 4`.
- **Optional reproducibility control (about 15 min):** run the same command on pre-W0 `main@2e0d738f`. It should reproduce the old `search.digest`, counts and `family_blend.validation` exactly.

**Compare.**
- Everything in the metrics table except the holdout row.
- `validation.csv` aggregates (mean, positives, min p).
- The train winners' DSL mix: count of `adv*` / `cap` / `vwap` tokens.

**RAM class and runtime.** Light on RAM (about 1 GB), long on CPU: about 15 min at `--threads 2`, or about 14.5 min without the holdout span.

**Gaps.** Unrecorded search-level effect of A-03 on crossover; E0b may rename the `trials.n_eff` field.

---

## G0-2 · L10 fundamental-zoo IC (v2)

**Artifacts.** `C:\atx\data\equity_fund_zoo_ic_l10v2_20260923\` holds:
- `zoo_pooled.csv` (headline: pooled h=21 rank IC, ICIR, non-overlapping t, years positive);
- `ic_by_year.csv`, `zoo_pooled_splits.csv` (survivor / non-survivor, bridged / unbridged);
- `alignment.json` (coverage and survivorship);
- `manifest.json` (schema `atx.fundamental-zoo-ic/v2`: trials_declared 120, zoo_lines 60, survivor_ids 3,382).

The report is `C:\atx-wt\pool-1\atx-engine\reviews\2026-09-23-fundamental-zoo.md`. The v1 dirs (`…_l10_20260923`) are superseded.

**Old command, exact.** An opt-in gtest harness, not an atx-impl stage. From the review's "Reproduce" section, `manifest.json` and pool-11 `SWARM_STATUS.md`; run from pool-11 at `6b949d7f`:

```powershell
$env:ATX_L10_FUND_POINTS = 'C:\atx\data\equity_fund_fields_l10v2_20260923\points.csv'
$env:ATX_L10_CONTEXTS    = 'C:\atx\data\equity_scorecard16_ctx_2013_t1000_20260920;C:\atx\data\equity_scorecard16_ctx_2013_t3000_20260920;C:\atx\data\equity_scorecard16_ctx_2014_t1000_20260920;C:\atx\data\equity_scorecard16_ctx_2014_t3000_20260920;C:\atx\data\equity_scorecard16_ctx_2015_t1000_20260920;C:\atx\data\equity_scorecard16_ctx_2015_t3000_20260920;C:\atx\data\equity_scorecard16_ctx_2016_t1000_20260920;C:\atx\data\equity_scorecard16_ctx_2016_t3000_20260920;C:\atx\data\equity_scorecard16_ctx_2017_t1000_20260920;C:\atx\data\equity_scorecard16_ctx_2018_t1000_20260920;C:\atx\data\equity_scorecard16_ctx_2018_t3000_20260920'
$env:ATX_L10_SURVIVOR_CONTEXTS = 'C:\atx\data\equity_scorecard16_ctx_2018_t1000_20260920;C:\atx\data\equity_scorecard16_ctx_2018_t3000_20260920'
$env:ATX_L10_FUNDZOO_OUT = 'C:\atx\data\equity_fund_zoo_ic_l10v2_20260923'
C:\atx-wt\pool-11\build-equity-rel\bin\atx-impl-tests.exe --gtest_filter=FundamentalZoo.RealDataIcReport
```

The engine config is hard-coded in `atx-impl/tests/fundamental_zoo_test.cpp:542-553`:
- `DropMissingForward`, `AverageRanksV1`, Q=10, `min_names_per_date` 30;
- 1,000 draws (200 for splits), seed `0x10F020260923`;
- 10 bps trade, 365 bps borrow, horizons {5, 21};
- the aligner lags 1 session, with staleness caps of 400 / 550 days.

The zoo fixture path is compiled in: `ATX_IMPL_TESTS_DIR/fixtures/fundamental_zoo.txt`.

Upstream export (**do NOT re-run in G0**; W0 does not touch it, and it reads `companyfacts.zip` and the warehouse `.bak`):
`python atx-engine\tools\export_fundamental_fields.py --companyfacts C:\atx\atx-db\data\cache\companyfacts.zip --warehouse C:\atx\atx-db\data\warehouse.duckdb.pre-migrate.20260922-235808.bak --contexts "C:/atx/data/equity_scorecard16_ctx_201[3-8]_t*_20260920" --out C:\atx\data\equity_fund_fields_l10v2_20260923`
(defaults `--seal 2020-01-01 --lag-sessions 1`). Tool v2 wrote `points.csv` (59,995 rows, sha `c8c0dc57…`).

**Inputs.**
- The 11 contexts above: 2013-2018, both cuts, no 2017 t3000. Dates run from 2012-03-26 to 2018-12-31, and the t3000 contexts have 3,544-3,608 instruments.
- `C:\atx\data\equity_fund_fields_l10v2_20260923\points.csv`. Its `manifest.json` says `seal_exclusive 2020-01-01` and "facts filed on or after 2020-01-01 are never exported".
- The survivor contexts: 2018 t1000 and t3000.

**2019 was never loaded.** The harness asserts every evaluated session and each survivor context's last key are before 2019-01-01 (`fundamental_zoo_test.cpp:407,666`).

**Old headline metrics.** Source is `zoo_pooled.csv` (cut, signal): pooled rank IC h21 / ICIR / t (non-overlapping) / years positive.

| Signal | t1000 | t3000 |
|---|---|---|
| qual_gpa | +0.01652 / 0.130 / **1.670** / 4 of 6 | +0.02324 / 0.210 / **1.816** / 4 of 5 |
| inv_iss | +0.01805 / 0.170 / 1.371 / 4 of 6 | +0.01564 / 0.193 / 1.176 / 4 of 5 |
| pead_sue | +0.01294 / 0.171 / 0.754 / 4 of 6 | +0.00851 / 0.156 / 1.471 / 3 of 5 |
| val_bp | −0.02404 / −0.181 / −2.056 / 1 of 6 | −0.00779 / −0.065 / −0.885 / 2 of 5 |
| qual_lowlev | −0.02896 / −0.311 / −2.625 / 0 of 6 | −0.02332 / −0.306 / −1.621 / 1 of 5 |
| qual_fscore | −0.01863 / −0.248 / −2.420 / 2 of 6 | −0.00054 / −0.008 / −0.255 / 2 of 5 |
| ref_momentum_12_1 (reference, not a trial) | +0.02802 / 0.141 / 0.651 / 4 of 6 | +0.01898 / 0.108 / 0.582 / 3 of 5 |

- **Verdict: NO CANDIDATE.** 0 of 60 expressions reach |t| > 2 in the predicted direction.
- 6 trials have |t| > 2 with the wrong sign at t1000 h21: qual_lowlev, qual_fscore, val_bp_ss, val_bp, val_logbp, val_bp_sn.
- Survivor bridge rates: 56-63% for survivors vs 1-3% for non-survivors (top-1000).
- Wall time was **338.5 s** (`zoorun.log`). **Peak RAM was not recorded.** The binary sha was not recorded either (the manifest has no producer field).

**Binary.** Release: `atx-impl-tests` in pool-11 `build-equity-rel` (Debug would be far slower). Build `atx-shm-worker` alongside it.

**W0 changes that move it:**
- **E-09** (`execution_delay` default 1: forward return from t+1; the harness has no knob);
- **E-02** (block length changes the `rank_ic_h21_lo/hi` CIs; the non-overlapping t is computed by the harness and does not change);
- **E-15** (Pearson only; not in the headline);
- **A-01** (`rank()` inside the `*_sn`, `*_grank` and `comp_*` expressions);
- **A-09** (flat-window guard on forward-filled fundamentals: the `ts_zscore`, `ts_std` and `delta` based `*_ts` and `pead_*` lines);
- **A-02 / A-03** if any zoo line uses `hump` or non-literal scalar slots. A-03 could make `FundamentalZoo.FixtureParsesTypechecksAndEvaluates` fail. Check that first.

The **survivor-conditioned bridge (D-10) is not fixed in W0** (it is W1-D1), so that caveat stands. `alignment.json` coverage should come out byte-identical, because the aligner is not W0-owned. Use that as the invariance check.

**G0 command.** Same inputs, new binary, new output dir:

```powershell
$env:ATX_L10_FUND_POINTS = "$D\equity_fund_fields_l10v2_20260923\points.csv"
$env:ATX_L10_CONTEXTS    = '<the same 11 dirs as above>'
$env:ATX_L10_SURVIVOR_CONTEXTS = "$D\equity_scorecard16_ctx_2018_t1000_20260920;$D\equity_scorecard16_ctx_2018_t3000_20260920"
$env:ATX_L10_FUNDZOO_OUT = "$G0\data\equity_fund_zoo_ic_l10v2_g0_<W0sha8>"   # must not exist
Invoke-G0 'l10_g0' 'C:\atx-wt\pool-11\build-equity-rel\bin\atx-impl-tests.exe' @('--gtest_filter=FundamentalZoo.RealDataIcReport')
```

First run `--gtest_filter=FundamentalZoo.*` without the env vars. The real-data test skips, and the fixture test must pass.

To isolate E-09 from A0, you would have to edit the harness (`icfg.execution_delay = 0`). That is not allowed in G0 ("change nothing"), so record the combined delta and attribute it qualitatively.

**RAM class and runtime.** Light. Estimated ≤ 1.5 GB: contexts load one at a time, the largest is 170 MB, and `points.csv` is 12 MB. About **6 min**.

**Gaps.**
- No old RAM or binary sha.
- E0a API changes might break the harness at compile time. It sits in `atx-impl-tests`, which is not an E0a-owned target, so confirm it compiles at the W0 gate.

---

## G0-3 · cp21 IC scorecard set

**What "cp21" is.** Checkpoint 21, "batch 3c":
- 4 new OHLC-range families (`intraday_mom_252`, `day_minus_night_252`, `gk_low_vol_63`, `chl_spread_63`);
- measured with 22 retained families and momentum_252, momentum_126 and blend_equal (29 signals);
- on the liquidity-floored universe (min dollar ADV $50M over 21 days; rulings R18-5 / R19-1), with the R21-1 engine fix;
- 80 new declared trials, cumulative N = 570.

Report: `C:\atx-wt\pool-1\atx-engine\reviews\2026-09-22-equity-alpha-scorecard-cp21.md`. The headline variant is `IncludeAuditedTerminalV1`, restriction `full`, net of cost.

**Artifacts.**

| Layer | Dirs | Producer |
|---|---|---|
| contexts (cp16, **reused, not rebuilt**) | `C:\atx\data\equity_scorecard16_ctx_<Y>_t<cut>_20260920\context.bin`, 13 cells | exe sha `d1224b32…` |
| baselines (cp19, reused by cp21) | `C:\atx\data\equity_scorecard19_base_<Y>_t<cut>_20260921\` (`summary.json`, `manifest.json`, `evaluation.bin`, `combo.bin`) | exe sha `737d20fa…` |
| IC cells (cp21) | `C:\atx\data\equity_scorecard21_ic_<Y>_t<cut>_20260922\` (`ic.csv`, `quantile_spread.csv`, `signal_autocorr.csv`, `ic_summary.json`, `ic_decay.csv`, `coverage.csv`, `seal.json`, `request.json`, `manifest.json`) | exe `C:/atx/.worktrees/equity-platform/build-equity-rel/bin/atx-impl.exe`, sha `e94a50f6…` |
| scorecard (headline) | `C:\atx\data\equity_scorecard21_scorecard_20260922\` (`scorecard.md` = the review doc, `scorecard.csv` 9,160 rows, `capacity.csv` 58 rows, `receipt.json`) | `build-equity/audits/iteration16_equity_scorecard.py`, sha `0e9cc6fe…`. The pool-1 copy is identical |
| anchor (token order, cp14) | `C:\atx\data\equity_ic_training_2013_20260920\` | cp14 |
| 2013 required-mark audit | `C:\atx\data\equity_source_reconciliation_2013_20260919\manifest.json`, sha `5aa71196…` | — |
| trial ledger | `atx-engine/reviews/trial-ledger-cp21-batch3c.jsonl`, 26 lines | — |
| cells receipt | `C:\atx\.worktrees\equity-platform\build-equity\audits\iteration16-cells-attempt1cp21-batch3c.json` | — |

The 13 cells are 2013-2019 × {1000, 3000}. **2017 t3000 is NOT FIT**: 5,072 > 4,096 instruments, and no context was ever built for it.

**Old commands, exact, from the cells receipt.** The runner was `iteration16_run_cells.py` (sha `b4a5f7c4…`; the pool copy is now `85d50c5d…` and differs). Receipt fields:
- `--phases ic --ic-name-prefix equity_scorecard21 --ic-stamp 20260922`
- `--base-name-prefix equity_scorecard19 --base-stamp 20260921`
- `--min-dollar-adv 50000000 --dollar-adv-window 21 --parallel 4`
- `--ledger atx-engine/reviews/trial-ledger-cp21-batch3c.jsonl --receipt-tag cp21-batch3c`
- `--atx-impl C:/atx/.worktrees/equity-platform/build-equity-rel/bin/atx-impl.exe`
- cwd = the equity-platform worktree

Per cell, with (ES, EE) = 2013 (2013-04-04, 2014-01-01), 2014 (2014-01-02, 2015-01-01), 2015 (2015-01-02, 2016-01-01), 2016 (2016-01-04, 2017-01-01),
2017 (2017-01-03, 2018-01-01), 2018 (2018-01-02, 2019-01-01), 2019 (2019-01-02, 2020-01-01):

```text
# baseline (cp19; exe 737d20fa; 0.76-3.77 s, 0.12-0.37 GB per cell, 28 s total)
atx-impl.exe equity-baseline --panel C:/atx/data/equity_scorecard16_ctx_<Y>_t<cut>_20260920/context.bin
  --out C:/atx/data/equity_scorecard19_base_<Y>_t<cut>_20260921 --evaluation-start <ES> --evaluation-end <EE>
  --max-working-bytes 3000000000 --min-dollar-adv 50000000 --dollar-adv-window 21
# ic (cp21; exe e94a50f6)
atx-impl.exe equity-ic --panel C:/atx/data/equity_scorecard16_ctx_<Y>_t<cut>_20260920/context.bin
  --baseline-dir C:/atx/data/equity_scorecard19_base_<Y>_t<cut>_20260921 --out C:/atx/data/equity_scorecard21_ic_<Y>_t<cut>_20260922
  --evaluation-start <ES> --evaluation-end <EE> --max-working-bytes 3000000000 --trial-ledger atx-engine/reviews/trial-ledger-cp21-batch3c.jsonl
# scorecard (reconstructed from receipt.json; cwd = equity-platform worktree)
python build-equity/audits/iteration16_equity_scorecard.py --cells <13 x DIR:YEAR:CUT, or --glob "C:/atx/data/equity_scorecard21_ic_*_20260922">
  --anchor C:/atx/data/equity_ic_training_2013_20260920 --out C:/atx/data/equity_scorecard21_scorecard_20260922
  --headline-variant include_audited_terminal_v1 --headline-restriction full
  --cells-receipt build-equity/audits/iteration16-cells-attempt1cp21-batch3c.json --declared-n 570
  --n-note "N = 490 (cp14-cp20 cumulative, cp20 scorecard) + 80 for cp21 = ..." --title "cp21 batch 3c (...) -- ONE PAGE"
```

The contexts themselves were built with `atx-impl panel --segs C:/atx/data/tickerhistory_training_native_2012_2019_20260920/segments --start <(Y-1)-12-22…> --end <Y+1>-01-01 --min-adv-usd 0 --min-price 1 --top-n-by-adv 0 --compact-universe true --preparation-manifest …\tickerhistory_training_20120326_20191231_20260920\manifest.json --universe-membership …\membership.bin --universe-cut <cut>:0.00 --universe-eval-start <Y>-01-01`. They are **not** rebuilt.

**Inputs and dates.** Context session ranges:

| Cell | Sessions |
|---|---|
| 2013 | 2012-03-26 .. 2013-12-31 |
| 2014 | 2012-12-24 .. 2014-12-31 |
| 2015 | 2013-12-23 .. 2015-12-31 |
| 2016 | 2014-12-22 .. 2016-12-30 |
| 2017 t1000 | 2015-12-22 .. 2017-12-29 |
| 2018 | 2016-12-22 .. 2018-12-31 |
| **2019** | 2017-12-22 .. **2019-12-31** |

- Instruments: t1000 1,228-1,697; t3000 3,544-3,608.
- The other inputs are the membership file (latest 2019-12-31) and the 2013 audit.
- Everything is before 2020. **The old set read and scored 2019** (two cells).

**Old headline metrics.** Source is `scorecard.md` / `scorecard.csv`: h=21 pooled net Sharpe [2.5%, 97.5%], IncludeAuditedTerminalV1 / full.

| Signal | t1000 | t3000 |
|---|---|---|
| intraday_mom_252 | 0.278 [−0.406, 0.946], n 63, rank IC 0.0369 | **0.676 [0.004, 1.383]**, n 52, rank IC 0.0365 |
| day_minus_night_252 | 0.264 [−0.584, 1.049] | **0.809 [0.072, 1.462]** |
| blend_equal | 0.422 [−0.193, 1.087] | 0.452 [−0.161, 1.121] |
| momentum_252 | 0.467 [−0.212, 1.166] | 0.432 [−0.239, 1.122] |
| momentum_126 | 0.424 [−0.152, 1.053] | 0.448 [−0.185, 1.148] |
| mom252_sector_neutral | 0.366 [−0.309, 1.043] | 0.444 [−0.233, 1.133] |

- **R16-8 verdict: all 29 signals FAIL.** Bar 1 needs pooled ci_lo > 0 at h21 on both cuts, so no candidate.
- DSR at N = 570 is at most 5.6e-4.
- Also compare every row of `scorecard.csv` (per year and pooled, h ∈ {1, 5, 10, 21, 63}, both variants and restrictions), `capacity.csv`, and each cell's `ic_summary.json` / `predictions_confirmed` (dates_below_min_names: 0; max 2 in one block).
- **Wall / peak:**
  - IC phase 23:08:13 → 23:11:51 (**3 m 38 s wall**, `--parallel 4`);
  - per cell 30.6-85.0 s at 0.26-0.86 GB; serial sum 729.5 s (receipt);
  - baselines 28 s total;
  - scorecard wall not recorded.

**Blocker: family-set drift.**
- `main` compiles **29** families. The 3 extra are the cp22 FINRA families (`si_shares` DSLs; commit `092314d4` says they were registered "not yet run").
- `kCheckpoint` is 22 and `kEquityFamilyRetainedCount` is 26.
- The cp16 contexts have **no `si_shares`**: 12 fields, verified from the manifests. `equity_baseline_views.cpp:30` says a missing optional field makes the DSL "fail to compile, loudly".
- So the W0 binary **cannot run equity-ic on the cp21 inputs**.
- I checked mechanically: the first 26 entries of `kEquityFamilyDsl` are byte-identical to the 26 family DSLs in the cp21 `manifest.json` `recipe.signals`.

The fix is a G0-only pin patch on a throwaway branch `g0/cp21-pin` from the W0 gate SHA in pool-11, never merged:
- `atx-impl/src/equity_baseline_views.hpp`:
  - `std::array<…, 29> kEquityFamilyDsl` → 26 (drop the three `si_shares` DSLs);
  - `kEquityFamilySignalNames` → 26 (drop `short_interest_ratio`, `days_to_cover_21`, `d_sir_63`);
  - `kEquityFamilyRetainedCount` 26 → **22**.
- `atx-impl/src/stage_equity_ic.cpp:89`: `kCheckpoint` 22 → **21**. `kTrialCountDeclared` then comes out as (26−22)·5·2·2 = 80, matching cp21's `trial_count_declared`.

Build `atx-impl` in pool-11 `build-equity-rel` **after** the L10 run, and hash it as `atx-impl-cp21pin`. Use the pinned binary for **both** the baselines and the IC cells. The IC stage re-derives the baseline view and `bind_fresh_view` must match.

**Binary.** Release (cells used `build-equity-rel`).

**W0 changes that move it:**
- **D-12**: as-of mask in the baseline views and IC. `equity-ic` rebinds the baseline view, so **the 13 baselines must be re-run with the fixed binary**; the old `equity_scorecard19_base_*` dirs will not bind;
- **E-09** (delay 1);
- **E-18** (min names 50; t1000 cells have ≥ 638 admitted names, so effect is about 0);
- **I-15** (the headline variant needs the 2013 audit);
- **A-01** (`rank()` in `mom_resid_vs_blend`, and the rank-space `blend_equal` combo);
- **A-09 / A-13** (`ts_std` on flat windows; AuditExact only if the stage uses it);
- **E-02 / E-15** (`ic_summary.json` CIs, Pearson IC).

**D-01 does not apply**: the families use `raw_close*volume` explicitly, and the contexts are pre-built.

**G0 commands.**

Step 0: copy the audit beside the new baselines. `load_required_marks` resolves `<baseline-dir parent>\equity_source_reconciliation_2013_20260919\manifest.json`:

```powershell
New-Item -ItemType Directory -Force "$G0\data\equity_source_reconciliation_2013_20260919","$G0\ledger" | Out-Null
Copy-Item "$D\equity_source_reconciliation_2013_20260919\manifest.json" "$G0\data\equity_source_reconciliation_2013_20260919\manifest.json"
# verify sha256 == 5aa7119684701827529591aa199a860b2e83a38eff17b3d5eb1168cc057032c1 (unless I0b adds an explicit audit flag)
```

Then for each (Y, cut) in the 13 cells, with the same ES/EE:

```powershell
$PIN = 'C:\atx-wt\pool-11\build-equity-rel\bin\atx-impl.exe'   # g0/cp21-pin build
$ctx = "$D\equity_scorecard16_ctx_${Y}_t${cut}_20260920\context.bin"
Invoke-G0 "base_${Y}_t${cut}" $PIN @('equity-baseline','--panel',$ctx,'--out',"$G0\data\equity_g0cp21_base_${Y}_t${cut}_<W0sha8>",
  '--evaluation-start',$ES,'--evaluation-end',$EE,'--max-working-bytes','3000000000','--min-dollar-adv','50000000','--dollar-adv-window','21'
  <# + <TBD-I0b as-of membership: e.g. '--membership',"$D\equity_universe_pit_2013_2019_20260920\membership.bin", cut ${cut}:0.00> #>
  <# + only if I0b makes them mandatory: '--replay-trade-bps','5','--replay-annual-borrow-bps','365' #>)
Invoke-G0 "ic_${Y}_t${cut}" $PIN @('equity-ic','--panel',$ctx,'--baseline-dir',"$G0\data\equity_g0cp21_base_${Y}_t${cut}_<W0sha8>",
  '--out',"$G0\data\equity_g0cp21_ic_${Y}_t${cut}_<W0sha8>",'--evaluation-start',$ES,'--evaluation-end',$EE,
  '--max-working-bytes','3000000000','--trial-ledger',"$G0\ledger\trial-ledger-g0-cp21.jsonl"
  <# + <TBD-I0b as-of membership flags>; leave execution delay (1) and min names (50) at the NEW defaults for the headline #>)
```

Run the IC cells 2 at a time. At about 0.86 GB per t3000 cell, 4 in parallel peak near 3.4 GB on a shared 16 GB host.

Scorecard. Keep the **old anchor**: it fixes the signal token order, and so the bootstrap streams. Keep `--declared-n 570` so the DSR is comparable. Pass the pool copy of the cp16 design (sha `a83484…` verified); the default path points into `C:\atx\.worktrees`:

```powershell
python C:\atx-wt\pool-11\build-equity\audits\iteration16_equity_scorecard.py `
  --cells <"$G0\data\equity_g0cp21_ic_2013_t1000_<W0sha8>:2013:1000" ... 13 entries> `
  --anchor C:\atx\data\equity_ic_training_2013_20260920 --out "$G0\data\equity_g0cp21_scorecard_<W0sha8>" `
  --headline-variant include_audited_terminal_v1 --headline-restriction full `
  --cp16-design C:\atx-wt\pool-11\atx-engine\reviews\2026-09-20-iteration16-alpha-scorecard-design.md `
  --declared-n 570 --n-note "G0 truth-delta re-measurement of cp21 on W0 gate <sha>: as-of membership, delay 1; no new trials" `
  --title "G0 re-measurement of cp21 batch 3c (W0 gate <sha>) -- ONE PAGE"
```

**Optional attribution controls** (all on the pinned binary):
- C1: IC with `<TBD-I0b: --execution-delay 0> --allow-same-close <--min-names-per-date 2>` on the new as-of baselines. This isolates D-12 plus A0 from E-09 / E-18.
- C2: pre-W0 `main` plus the same pin patch, one cell (2014 t1000). Expect `ic.csv` sha `e6c00971…`, which proves "same inputs".

**RAM class and runtime.**
- Medium-heavy at parallel 4 (about 3.4 GB); light when serial.
- About 30 s for baselines, 4-12 min for IC (parallel 4 / serial), a few minutes of single-thread Python for the scorecard (not recorded).
- Add about 10-15 min for the pin build.

**Gaps.**
- The as-of flag names are `<TBD-I0b>`.
- If I0b renames the `IncludeAuditedTerminalV1` token, the scorecard refuses with exit 3.
- The frozen Python `RECIPE` hard-codes `"membership_rule": "year-union-not-as-of"` and the md caveat "Membership is the YEAR UNION", so the G0 receipt text will be stale. Say so in `--title` / `--n-note` and the report; do not edit the frozen script.
- `iteration16_run_cells.py` hard-codes `WORKTREE=C:/atx/.worktrees/equity-platform` and its preflight hashes the CRLF-sensitive cp14 design (pool copy `d74a61ee…` vs `888c726b…` LF). Run the commands directly instead.

---

## G0-4 · L7 PIT risk-model scorecard

**Artifacts.**
- `C:\atx\data\l7_riskmodel_scorecard_pit_2014_t1000_20260923\l7_scorecards.json` (the only file; headline).
- Its input `C:\atx\data\l7_riskmodel_raw_pit_2014_t1000_20260923\` (`meta.txt` "509 1254", `close.f64`, `volume.f64`, `cap_tn.f64`, `sector_tn.f64`, `dates.i64`).
- Superseded and look-ahead-contaminated (do not use): `…l7_riskmodel_scorecard_2014_t1000_20260923` and `…l7_riskmodel_raw_2014_t1000_20260923`.

**Old commands, exact.** From the pool-8 archive and `wave2_results.json` `fix:l7-riskmodel`; head `05e83cf4`; Release `atx-engine-bench` with `ATX_BUILD_BENCH=ON`, preset `equity-rel`:

```powershell
python C:\atx-wt\pool-8\atx-engine\bench\tools\l7_panel_to_raw.py C:\atx\data\equity_scorecard16_ctx_2014_t1000_20260920\context.bin C:\atx\data\l7_riskmodel_raw_pit_2014_t1000_20260923
# -> "T=509 N=1254 … cap_ok=1.000 sectors=8"
$env:ATX_L7_REAL_DIR='C:/atx/data/l7_riskmodel_raw_pit_2014_t1000_20260923'; $env:ATX_L7_OUT_DIR='C:/atx/data/l7_riskmodel_scorecard_pit_2014_t1000_20260923'
C:\atx-wt\pool-8\build-equity-rel\bin\atx-engine-bench.exe --benchmark_filter=BM_L7RealScorecard      # Args({252,120}), Iterations(1)
```

**Inputs.**
- The raw dir, converted from `equity_scorecard16_ctx_2014_t1000_20260920` (artifact `995eb8cc`, sessions 2012-12-24 .. 2014-12-31).
- The JSON records `newest_day 16435`, which is 2014-12-31: W=252, 120 one-day forecasts, 1,254 assets.
- Everything is before 2020; 2019 was never loaded.

**Old headline metrics.** Source is `l7_scorecards.json`, band 1 ± 0.129.

| Label | K | EW bias | MinVar bias | Random bias (share in band) | Optimized bias (share in band) | Asset bias | MRAD | Excluded per period |
|---|---|---|---|---|---|---|---|---|
| fundamental_price_styles | 11.0 | 1.0900 | 0.9227 | 0.9783 (0.48) | 1.0426 (0.85) | 1.0642 | 0.1954 | 0.31 |
| hybrid_baing (default) | 18.9 | 1.0904 | 1.0225 | 0.9868 (0.55) | 1.0331 (0.90) | 1.0654 | 0.1961 | 0.31 |
| hybrid_mp | 19.0 | 1.0904 | 1.0238 | 0.9867 (0.56) | 1.0341 (0.90) | 1.0654 | 0.1961 | 0.31 |
| hybrid_baing_ewma | 18.9 | 1.0985 | 1.0289 | 1.0764 (0.58) | 1.0953 (0.80) | 1.0984 | 0.1804 | 0.31 |
| hybrid_baing_lw2020_spec_ewma | 18.9 | 1.0882 | 0.9977 | 0.9934 (0.54) | 1.0961 (0.80) | 1.0967 | 0.1834 | 0.31 |

- Random-book Q is about 2.566-2.582 for every config.
- Wall time was **81.5 s**. Peak RAM was not recorded.

**Binary.** Release `atx-engine-bench`. Its header says "Run from the Release bench build only". Configure `-Preset equity-rel -Bench`, or O1's `equity-bench` preset.

**W0 changes that move it:**
- Only R0, and only where the hybrid path shares R0-owned code: `FactorModel::create` floors D at `factor_model.cpp:94` (R-05 → the new floor at 0.1·median(D) for thin names). This is likely to move MinVar and optimized bias.
- Possibly `exposures.hpp` helpers (R-03 / R-06), if `fundamental_factors.hpp` `build_fundamental_exposures` uses them. It has its own `standardize_cw` and winsor path, so probably not.

D0 does not reach it: the context is pre-built and read by Python. **A null or near-null delta is the expected, reportable result.**

**G0 command.** Reuse the raw dir: the converter is pure Python and not W0-owned, so that keeps "same inputs". The bench **does not create** `ATX_L7_OUT_DIR` and **overwrites silently**. Use a fresh, pre-created dir and check the file appears:

```powershell
New-Item -ItemType Directory "$G0\data\l7_riskmodel_scorecard_pit_2014_t1000_g0_<W0sha8>" | Out-Null
$env:ATX_L7_REAL_DIR = "$D\l7_riskmodel_raw_pit_2014_t1000_20260923"
$env:ATX_L7_OUT_DIR  = "$G0\data\l7_riskmodel_scorecard_pit_2014_t1000_g0_<W0sha8>"
Invoke-G0 'l7_g0' 'C:\atx-wt\pool-10\build-equity-rel\bin\atx-engine-bench.exe' @('--benchmark_filter=BM_L7RealScorecard')
```

**RAM class and runtime.** Light (estimated < 1 GB; the inputs are 4 × 5 MB), about 1.5 min, plus the one-time bench build.

---

## G0-5 · equity-baseline 2013 cell

Two artifacts go by this name. Do both: they exercise different W0 fixes.

### 5a · `C:\atx\data\equity_baseline_training_2013_20260919` (the named one; replay book; **old run FAILED**)

**Artifacts.** The run failed, so there is no `manifest.json`. The dir holds:
- `failure.json` (headline), `request.json`, `readiness.csv`, `target_exposures.csv`;
- `evaluation.bin` (id `57b7c21c…`) and `combo.bin` (id `def31e5c…`) with their manifests;
- `books.bin` (38 weekly target periods; this is the target book, not the replay);
- empty `.pending/` and `report/.pending/`.

Narrative in `C:\atx-wt\pool-1\atx-engine\docs\PLATFORM_PROGRESS.md:564-601`; receipt `atx-engine/reviews/2026-09-19-equity-baseline-validation.json`.

**Old command, exact.** From `C:\atx-wt\pool-1\atx-impl\README.md:8-14`; exe `build-equity/bin/atx-impl.exe` (equity-dev = **Debug**), sha `c482790a…`:

```text
build-equity/bin/atx-impl.exe equity-baseline --panel C:/atx/data/tickerhistory_training_native_20260919/context.bin
  --out C:/atx/data/equity_baseline_training_2013_20260919 --evaluation-start 2013-04-04 --evaluation-end 2014-01-01
  --max-working-bytes 3000000000 --report-aum 100000000 --replay-execution-delay 1 --replay-trade-bps 5
  --replay-annual-borrow-bps 365 --replay-day-basis 365
```

**Inputs.**
- `C:\atx\data\tickerhistory_training_native_20260919\context.bin`:
  - artifact `ec572b82…`, payload sha `c219dc23…` (verified);
  - 445 sessions, 2012-03-26 .. 2013-12-31; 1,661 instruments;
  - a daily top-1000 universe with ADV ≥ $20M and raw price > $5 (not a year-union allow-list).
- Preparation manifest `tickerhistory_training_20120326_20131231_20260919\manifest.json`.

Everything is before 2020; no 2019.

**Old headline metrics.**
- `failure.json`: `"InvalidArgument: replay: missing/nonpositive required close at period=6 instrument=604 session_key_ns=1365724800000000000 security_id=150340"`, which is **2013-04-12**.
- 189 evaluation observations and 38 weekly targets.
- Target changes total **9.2187 × NAV**; 31 of 37 post-entry changes exceed 0.20. 3,892 cells were excluded by common readiness.
- No returns, Sharpe or executed turnover exist.
- The 2013 audit (`equity_source_reconciliation_2013_20260919`) found 110 missing required cells across 34 ids and 74 dates. **150340 and 351548 are evidenced non-terminal holes.** HNZ, DELL and MOLX are terminal cash deals; PCS 146189 is terminal without evidence.
- Wall **8.96 s**; peak WS **165,318,656 B**; peak private 154,447,872 B (Debug).

**Binary.** Release is not needed (9 s in Debug). The unpatched pool-10 Release `atx-impl` is fine; record its sha.

**W0 changes that move it:**
- **B-04**: the new default `TerminalReturn` means the run should now **complete** instead of aborting at 150340 (the B0 accept item "PCS 2013-05-01 fixture runs to the end");
- **B-05** (borrow counted once), **B-03** (legacy report);
- **I-11 / B-02** (already explicit: costs 5 / 365, delay 1), **I-17** (manifest before `.pending`);
- **A-01** (rank transform ties).

D-12 does not apply: this is a daily-mask native context.

**Caveat for the report.** Under the new default, all 34 missing-mark ids get the flagged Shumway −30% / −55%. That includes the evidenced non-terminal data holes (150340, 351548) and the 29 unclassified ids, so the "new" headline is mechanically complete but economically unqualified until the W2-D2 terminal-return table exists. Report the delist-flag count next to it.

**G0 commands.**

```powershell
$EXE = 'C:\atx-wt\pool-10\build-equity-rel\bin\atx-impl.exe'
Invoke-G0 'base2013_g0' $EXE @('equity-baseline','--panel',"$D\tickerhistory_training_native_20260919\context.bin",
  '--out',"$G0\data\equity_baseline_training_2013_g0_<W0sha8>",'--evaluation-start','2013-04-04','--evaluation-end','2014-01-01',
  '--max-working-bytes','3000000000','--report-aum','100000000','--replay-execution-delay','1','--replay-trade-bps','5',
  '--replay-annual-borrow-bps','365','--replay-day-basis','365')
# control C5 (old semantics): same + <TBD-B0/I0b: delisting policy = Abort>  -> expect the identical failure at period 6 / 150340
```

Optional: re-measure the cp14 anchor on the new baseline. It needs the audit copy in `$G0\data` (G0-3 step 0) and the pinned binary:
`equity-ic --panel "$D\tickerhistory_training_native_20260919\context.bin" --baseline-dir "$G0\data\equity_baseline_training_2013_g0_<W0sha8>" --out "$G0\data\equity_ic_training_2013_g0_<W0sha8>" --evaluation-start 2013-04-04 --evaluation-end 2014-01-01 --max-working-bytes 3000000000 --trial-ledger "$G0\ledger\trial-ledger-g0-cp21.jsonl"`.
Do **not** feed it to the G0-3 scorecard as `--anchor`; the old anchor fixes the token order.

**RAM class and runtime.** Light (about 0.2 GB), under 1 min.

### 5b · the cp21 chain's 2013 cells: `C:\atx\data\equity_scorecard19_base_2013_t{1000,3000}_20260921`

These are covered by the G0-3 baseline step; compare their `summary.json` `readiness`. `replay` is `skipped-membership-context`, so there is no book.

| Cut | Admitted of eligible cells | Liquidity-floor rejected | Min admitted names | Observations |
|---|---|---|---|---|
| t1000 | **144,225** of 227,303 | 78,404 | 638 | 189 |
| t3000 | **145,552** of 647,286 | 472,319 | 639 | 189 |

The **D-12** as-of mask should lower the admitted cells, because mid-year joiners are hidden before their effective session. This is the cleanest single-defect delta in G0.

---

## I-24 · `QUANT_PLATFORM_SWARM_STATUS.md`

File: `C:\atx-wt\pool-1\atx-engine\docs\QUANT_PLATFORM_SWARM_STATUS.md`, section "Real-data results", lines 55-60. The current text is verbatim:

```text
55 **Lane 9 mining** (`C:\atx\data\equity_mine_l9_guard_20260923`, supersedes the unguarded run): train 2013-16,
56 validation 2017-18, holdout 2019 read once. 378 seeds → 2,243 candidates → 2,065 scored trials, N_eff 5.71,
57 de-correlated family of 52. **0 alphas admitted** (BY p = 1). Train winners (net SR 1.0-1.5) were mostly
58 size/liquidity proxies and collapsed on validation (mean net SR ≈ -0.3). The pre-registered equal-weight family
59 blend was not admitted on validation (net SR ~0.6, p ~0.2); its 2019 holdout (descriptive only) was net SR ~1.8,
60 low turnover. Conclusion: price/volume-only search overfits; breadth + combination is where signal appears.
```

The true values come from `C:\atx\data\equity_mine_l9_guard_20260923\gate_report.json`:
- `family_blend.validation.sharpe_net = -1.218738142997244`
- `p_one_sided = 0.9503335211679915`, `sharpe_gross = -0.1310663851041065`, `t_nw = -1.64809607481385`, `mean_net_bps = -0.8531`
- `family_blend.holdout.sharpe_net = 1.729687486711181` (p 0.0620)

Line 59 carries the numbers of the **superseded unguarded** run (`equity_mine_l9_20260923`: validation 0.5985, p 0.1976; holdout 1.819), even though line 55 cites the guard directory. Two neighbouring phrases are wrong the same way:
- line 56 "holdout 2019 read once": the gate report says `holdout.status "reused"`, `prior_reads 3`;
- line 58 "mean net SR ≈ -0.3": that is the unguarded −0.332. The guard run gives **−0.479**, with 10 of 52 positive.

Process note: the goal prompt §8 says this file "is a log; never rewrite earlier sections", but the plan says "Correct … (I-24)". Suggestion: append a "G0 errata (I-24)" section with the three corrections, and edit the old lines in place only if the owner okays it.

---

## Ledger lines (orchestrator only)

The ledger goes in `atx-engine/docs/LEDGER.md` (progress.md decision 1). Templates:

- `2026-09-2x G0 L9 guard rerun @W0 <sha>: family-blend val net SR -1.219 -> <new>; n_eff 5.71 -> <new>; admitted 0 -> <n>; digest 729454a1 -> <new> (D-01/A0/E0b)`
- `2026-09-2x G0 cp21 rerun (pinned 26 families, as-of, delay 1): intraday_mom_252 t3000 h21 net 0.676 [0.004,1.383] -> <new>; R16-8 0/29 -> <n>/29 (D-12/E-09)`
- `2026-09-2x G0 I-24: STATUS doc blend-val "~0.6" was the unguarded run; guard gate_report says -1.219 (p 0.95)`

## Open questions for the owner / orchestrator (summary)

1. **2019:** L9 `--holdout off` (recommended) or the exact replay with publish; cp21 keeps its two 2019 cells (the old set did).
2. **G0 output root** outside `C:\atx`, which the goal prompt forbids writing to. This needs the byte copy of the 2013 audit, or an I0b audit flag.
3. **cp21 pin patch** (26 families, checkpoint 21) is required; otherwise equity-ic fails on the `si_shares` DSLs.
4. **5a:** the new `TerminalReturn` default books Shumway returns on evidenced non-terminal data holes. Report it next to the Abort control.
5. **Unrecorded:** the old peak RAM for L10 and L7, and the old L10 binary sha. The Python scorecard's frozen recipe text will say "year-union".
6. **D0 panel fixes** (D-03/04/05/06/08/09) are **not measured** by G0: the contexts are pre-built. Say so explicitly in the G0 table.
