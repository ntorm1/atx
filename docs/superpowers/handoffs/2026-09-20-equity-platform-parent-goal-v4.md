# Equity platform parent goal — handoff v4 (2026-09-20, end of checkpoint 16)

Supersedes the stop point in `2026-09-20-equity-platform-parent-goal-v3.md` (v3) and its
predecessors v2 and v1. Every constraint in v1, v2 and v3 still binds unless restated here.
Read v1 §"Constraints", v2 §4–§7, v3 §4–§7, then this file top to bottom. The exact stop
point is in §1. Built from `.superpowers/sdd/equity-platform-parent-goal/progress.md`
lines 110–146 (the checkpoint-16 session, starting "Session start (v3 goal, 2026-09-20
evening)"), the frozen checkpoint 16 design and its addendum 1, the published scorecard and
its receipt, and the three attempt-1 run receipts under `build-equity/audits/`. No fact
below is invented; unclear items say "(not recorded)".

## 1. Exact stop point

- Checkpoint 12: validated, receipt immutable. Unchanged.
- Checkpoint 13 (claims-aware replay seam): MINIMAL-CLOSED, unchanged since v2. T3 (engine
  scenarios B/C), T4 (allocation certification), T5 (equity-book wiring, `--transitions`)
  remain DEFERRED by ruling.
- Checkpoint 14 (cross-sectional forecast evaluation, Stage 1): COMPLETE, unchanged. Its
  2013-only numbers are no longer the branch's alpha evidence — see §3.
- Checkpoint 15 (point-in-time universe builder): COMPLETE, unchanged since v3. Its
  `membership.bin` is now a consumed input.
- Checkpoint 16 (first alpha scorecard): **COMPLETE on the LEAN path (ruling R16-13)**. The
  design was frozen with a SHA before any number existed; the one-off 2012–2019 span ingest,
  the measure-first cell and the 13-cell batch all ran to exit 0; the scorecard is published
  in the repo. **Verdict: NO CANDIDATE** — all three signals fail the pre-registered bars on
  both cuts. The post-hoc gross-vs-net diagnostic (NOT pre-registered, NOT a trial) says the
  failure is signal, not cost.
- Nothing is in flight. No commit or merge was made and none is authorised. The
  documentation pass that produced this file, the `PLATFORM_PROGRESS.md` checkpoint 16
  section and the two README paragraphs was edit-only: no build, no native run, no git
  mutation.
- What was deliberately NOT done, and is therefore the next parent's inheritance: hole
  remediation attempt 2 (authorised by R16-26, not started), the as-of membership predicate,
  the checkpoint 16 canonical-ledger mode, the stdlib oracle suite, the separate receipt
  writer, and the design review — all four of the last five dropped by R16-13(c).

## 2. Where everything is

Worktree `C:\atx\.worktrees\equity-platform`, branch `feat/equity-platform-20260920`, HEAD
`dffb609b7a3c` (unchanged all session; all work uncommitted). `git status --short` 224
entries (was ~214 at v3); expected. Shared checkout `C:\atx` untouched except git-ignored
`C:/atx/data/` (§2.3). Shared stash stack not re-verified this session (carry v2's count of
9 as last known).
SDD ledger: `.superpowers/sdd/equity-platform-parent-goal/progress.md`, 146 lines at stop
(was 110 at v3; the checkpoint-16 session is lines 110–146). New reports this session, same
directory: `research-cp16-scorecard.md` (396 lines), `cp16-design-report.md`,
`cp16-ingest-runner-report.md`, `cp16-task-cpp-report.md`, `cp16-task-py-report.md`,
`cp15-handoff-v3-report.md`, `cp16-docs-report.md`. All earlier reports unchanged.

### 2.1 Pinned artifacts (SHA-256 at handoff time)

| artifact | sha256 |
|---|---|
| cp12 design (must stay unchanged) `atx-engine/reviews/2026-09-20-iteration12-security-transition-design.md` | `0a964aebc15f14ea25ddd19fad5be723835b6b7a6e99a6aa308f3b7ecf95caee` |
| cp14 design (must stay unchanged) `...-iteration14-cross-section-ic-design.md` | `888c726b123c02b39636876a667cfab890b4304794f6be42d46d35befbedbe25` |
| cp14 receipt `...-cross-section-ic-validation.json` | `38dd653f7069cb4935b51d003baf9cdf87401434eccc9af5df15e907b319d723` |
| cp15 design (must stay unchanged) `...-iteration15-point-in-time-universe-design.md` | `4810fda251c6c285b29413ab6bea05b46db66e9bb0620cf17950b45075267dc8` |
| cp15 receipt (immutable) `...-point-in-time-universe-validation.json` | `2c52c6a4c2d2e841d8ffe3b8e1ced3e8c79d40eeba5a2176bcfc75b06068c07e` |
| **cp16 design, FROZEN** `...-iteration16-alpha-scorecard-design.md` | `a83484037cae527e2fbfba5dce04289742d1c81666b2c03c132f3da9268c04d1` |
| **cp16 addendum 1** `...-iteration16-alpha-scorecard-design-addendum-1.md` | `7173cc3348adba7212a3d0cbffd04ff61d14ebc2d2e9896d3ed237ea5be23a01` |
| **cp16 scorecard (repo copy)** `...-equity-alpha-scorecard-cp16.md` | `b36ab30ba9c434ba0708cad7d7cde5d2b680e022f99635f9ebc906147c160a48` |
| **cp16 scorecard receipt** `...-equity-alpha-scorecard-cp16-receipt.json` | `6d2c2b5003169fd628bff7f3204dd33235bb7a99e3142cd2860e08fd5de77945` |
| **cp16 `scorecard.csv`** (1,080 rows, under `C:/atx/data/equity_scorecard16_scorecard_20260920/`) | `9bab19ffe2dcc7511f87bc3829dc0831232b4741e15677ed41df6fa221fbbbc4` |
| `build-equity/bin/atx-impl.exe` — produced ALL 13 cells | `d1224b32844b16329b73011af944a3c13f6d0b5f1307874e3c2878bd2c42159c` |
| earlier `atx-impl.exe` — built the measure cell's PANEL only | `33c6ca8bf0608156507b922e0fbc01d386d9eb6bb446c047e7c6d4976f809b1c` |
| superseded intermediate (post-R16-24, pre-R16-25) | `2ea569b8b3d33c3f91a24855b0ce30e038bb4d07af194a1239b5f56b70f43930` |
| runner `build-equity/audits/iteration16_run_cells.py` (as recorded in the full receipt) | `4c4a43e2cc4d0dbf0993b73144e1ae41b18368a28701af075d480f6e9459ea52` |
| `build-equity/bin/atx-impl-tests.exe`, `atx-engine-data-tests.exe` | (not recorded this session) |

**Ledger files.** `atx-engine/reviews/trial-ledger.jsonl` — the CANONICAL ledger — is
**4 lines, byte-unchanged**: lines 1–2 `iteration14-cross-section-ic-0001`
pre-registered→completed, N = 30; lines 3–4 `iteration15-point-in-time-universe-0001`,
declared 0. It was never opened by checkpoint 16. The sidecar
`atx-engine/reviews/trial-ledger-cp16-restrictions.jsonl` is **26 lines** (13 cells ×
pre-registered + completed), with its own `.manifest.json`. Every sidecar line carries
`checkpoint 14`, purpose `training-only-forecast-evaluation`, `trial_count_declared 30`:
that 30 is the **unchanged binary's compile-time constant**, not new trials, so the
sidecar's `declared_trials_for_checkpoint` reads **390 — an artefact, never an `N`**.
**N_14 is 30** and is read from the canonical ledger. Digests of both ledger files at this
state: (not recorded).

The cp16 design SHA `a834840…` is the pre-registration anchor: it was frozen before the
first number existed and is verified by the scorecard script. It is embedded in no C++
source (unlike cp14's `kDesignNoteSha256` and cp15's `kEquityUniverseDesignNoteSha256`) —
the lean path deliberately skipped that, which is one of the objections in §5.2. Addendum 1
does **not** change the design SHA.

### 2.2 Native evidence (`build-equity/audits/iteration16-*`)

- **Span ingest** (R16-10): receipt `iteration16-ingest-span-2012_2019-attempt1.json`, `accepted` true; prepare 754 s / peak WS 106 MB, load 615 s / 80 MB, verify OK, 1,386 s total. Runner `iteration16_ingest_span.py` (450 lines), dry-run first, real run detached (PID 23328).
- **C++ delta** (agent `cp16-cpp`, `cp16-task-cpp-report.md`): `check history_panel` exit 0; `check stage_panel` fix1 exit 0 (attempt 1 failed, log kept); both test targets built exit 0; ctest `DataHistoryPanel` 10/10, panel suites 7/7 (2 new pass); full exes 186 pass / 14 skipped (data) and 466 pass (impl) with ONLY the pre-existing `StageRunSyntheticSmoke` failure.
- **Python** (agent `cp16-py`, `cp16-task-py-report.md`): `iteration16_run_cells.py` (348 lines) + `iteration16_equity_scorecard.py` (350 lines); self-test 10/10; `py_compile` OK; dry-run over 14 cells; both design SHAs verified.
- **Hand-checked cell** (design section 6, replacing the oracle suite): an independent 12-line parent recomputation from `ic.csv` reproduced `momentum_252` 1.854848 and `blend_equal` 1.920265 exactly (168 emitted rows -> 8 observations each). The CI was **not** independently checked (stdlib RNG; documented).
- **Measure-first cell** 2015 top-3000 (R16-11): attempts a-d failed and are versioned (`*_failed1..3`, `*_failed2/3.log`) - pre-created output root; the cp14 recipe pin (-> R16-24); log-file immutability; the book replay's missing close (-> R16-25). Attempt **e** COMPLETE (`iteration16-cells-attempt1-measure2015e.json`): panel exit 0, 82.7 s, peak WS 2,611,576,832 B (2.61 GB < 3 GB), 510 dates x 3,608 instruments (== predicted union, cap respected); baseline signals-only 17.8 s / 383 MB; `equity-ic` 118.0 s / 383 MB; `ic.csv` 13,920 rows all emitted; all 7 checks pass.
- **Full run**: `iteration16-cells-attempt1-full.json`, `all_run_cells_ok` true, 13 cells, every phase exit 0; panel 69-92 s at peak WS 2.51-2.76 GB; baseline signals-only 5.5-16.3 s; `equity-ic` 26.6-134.8 s at peak <= 0.38 GB; contexts 445-511 dates x {1,228-1,697 | 3,544-3,587} instruments, matching the predicted unions; `not_fit_cells` records `2017_t3000: "5,072 > kMaxIcInstruments 4,096"`.
- **Logs**: `iteration16-cells-logs/` (per-phase), `iteration16-cells-{full,measure2015}.{stdout,stderr}.log`, `iteration16-ingest-span-run.*.log`, `iteration16-configure.log`, `iteration16-build-atx-impl.log`. Two dead launches (unknown flag; missing argument) wrote nothing.
### 2.3 Real-data outputs

- Span ingest: prepared `C:/atx/data/tickerhistory_training_20120326_20191231_20260920/`
  (`manifest.json`), native
  `C:/atx/data/tickerhistory_training_native_2012_2019_20260920/segments`.
- Per cell (R16-12, all directly under `C:/atx/data`, immutable, attempts versioned):
  `equity_scorecard16_ctx_{Y}_t{CUT}_20260920` (panel context),
  `equity_scorecard16_base_{Y}_t{CUT}_20260920` (**signals-only baseline — NOT a book
  baseline**, `status = "complete-signals-only"`), `equity_scorecard16_ic_{Y}_t{CUT}_20260920`
  (`ic.csv`, `quantile_spread.csv`, `signal_autocorr.csv`).
- Scorecard: `C:/atx/data/equity_scorecard16_scorecard_20260920/{scorecard.csv,
  scorecard.md, receipt.json}`, copied into the repo as
  `atx-engine/reviews/2026-09-20-equity-alpha-scorecard-cp16{.md,-receipt.json}`.
- Checkpoint 15 inputs unchanged: `C:/atx/data/equity_universe_pit_2013_2019_20260920/`.

## 3. What checkpoint 16 established (and did not)

**There is NO measured candidate on this branch.** Headline cell
(`IncludeAuditedTerminalV1`, restriction `full`), pooled 2013–2019, net of cost, h = 21,
2.5/97.5 bootstrap interval: top-1000 `momentum_252` −0.04 [−0.75, +0.72] n = 74,
`momentum_126` −0.19 [−0.70, +0.67], `blend_equal` −0.23 [−0.80, +0.66]; top-3000
`momentum_252` −0.26 [−0.99, +0.69] n = 63, `momentum_126` +0.08 [−0.45, +0.70],
`blend_equal` −0.10 [−0.69, +0.65]. Sign stability 2/7–4/7 (top-1000) and 2/6–4/6
(top-3000). Rank-IC mean at h = 21 0.017–0.032; implied turnover 0.0012–0.0068; breadth
998.0 / 2,772.9. **All three signals FAIL bar 1 (`ci_lo` > 0) and bar 2 (sign stability) on
both cuts; bar 3 (turnover/breadth printed) passes. Verdict: NO CANDIDATE.** Nothing was
tuned in response.

**The momentum family's pooled GROSS spread is already ≈ 0 over 2013–2019 on both cuts.**
From the post-hoc diagnostic (NOT pre-registered, NOT a trial): gross/net at h = 21 pooled —
top-1000 `momentum_252` +0.11/−0.04, `momentum_126` −0.06/−0.19, `blend_equal` −0.12/−0.23;
top-3000 `momentum_252` −0.09/−0.26, `momentum_126` +0.24/+0.08, `blend_equal` +0.06/−0.10;
mean cost drag ≈ 38–40 bps per 21-session period. Cost reduction is therefore not the lever.

**2013 was one favourable year.** Per-year Sharpe at h = 21 is positive in 2013–2015 and
2019 and negative in 2016 (−0.4…−1.8), ≈ 0 in 2017 and negative in 2018 (−0.1…−1.5), on
both cuts; every per-year cell has n_obs < 20 (≈ 12 at h = 21), so those points are
indicative only. **The single-year checkpoint 14 numbers (2013 rank IC +0.09…+0.10 at
h = 21, positive net decile spreads on 189 observations) must never again be quoted as alpha
evidence**; they are now one favourable year inside a flat seven-year measurement and appear
in the scorecard only as a labelled context anchor (R16-5).

ESTABLISHED, positively: the machinery. A point-in-time-membership-restricted, per-year
evaluation pipeline that fits the 3 GB budget (panel peak WS 2.51–2.76 GB) and the 4,096
instrument cap, runs 13 cells end-to-end, and produces a pre-registered statistic with a
bootstrap interval from the already-emitted `spread_net` column — with no change to
`equity-ic`, the engine IC unit, the recipe version, or the canonical trial ledger.

NOT ESTABLISHED: that the momentum family is dead outside this specification (one horizon
grid, one weighting, one cost convention, one universe construction, one contaminated
archive); that any spread is achievable (no borrow availability, capacity, impact beyond a
flat 5 bps, no fills); anything out-of-sample — **2020-01-01 onward has never been read and
2023-2025 remain sealed**; and, because membership is a year union rather than as-of, the
per-year numbers carry a declared, uncorrected within-year selection look-ahead.

## 4. Rulings this session (all in progress.md lines 110-146, each with cost-if-wrong)

**R16-1 (route).** Panel-level allow-list + three `panel` flags + a Python scorecard over `ic.csv`; no engine IC change, no cap change. *Cost:* ~40-70 lines of code and tests reworked.
**R16-2 (ledger / N).** Year x cut runs are AR-7 restrictions of the 30 cp14 configurations, published side by side, never selected between => N_14 stays 30. *Cost:* if a later reader deems years/cuts a search, every deflated Sharpe is recomputed with a larger N - the 26 sidecar lines make the recount possible.
**R16-3 (allow-list).** Union of rebalances effective in Y plus the last one effective before Y (2013: effective-in-year only), applied once at compaction. *Cost:* year-edge membership off by one rebalance.
**R16-4 (2017 top-3000).** Union 5,072 > `kMaxIcInstruments` 4,096 => pre-declared NOT FIT, not run, cap not raised; 13 cells. *Cost:* one missing cell, disclosed in advance.
**R16-5 (panel screen).** PIT floors (`--min-adv-usd 0 --min-price 1 --top-n-by-adv 0 --compact-universe true`) + membership; cp16 contexts are NOT cp14 contexts, so the cp14 2013 run stays the anchor, printed beside. *Cost:* none - disclosure only.
**R16-6 (variants/restrictions).** All 30 configurations run and are emitted per cell; headline label `IncludeAuditedTerminalV1`/`full` fixed before the run; cp14 declares no headline variant. *Cost:* the label only.
**R16-7 (Sharpe recipe).** Offset-0 stride-h sub-series of emitted `spread_net`; mean/sd(ddof=1) x sqrt(252/h); circular block bootstrap block 5, B=2000, seed 20260920; pooled = concatenation in year order; sign-stability k/n; offsets robustness-only. *Cost:* recomputable from the same `ic.csv` under a versioned attempt 2, no native re-run.
**R16-8 (acceptance bars).** Written before the run: pooled `ci_lo` > 0 at h=21 on both cuts; sign stability n/n; turnover and breadth printed. Passing = candidate, not tradeable; failing = nothing is tuned. *Cost:* the bars are off by one decision; every underlying number is printed anyway.
**R16-9 (data hole).** Attempt 1 runs as-is on qa-v1 with the contamination pre-registered as a prediction, `hole_flag` labelling, remediation gated on the material test. *Cost:* one extra pre-registered attempt.
**R16-10 (data span).** One prepare+load 2012-03-26..2019-12-31 into a new versioned dir; per-year dirs untouched; parent-only, detached, dry-run first. *Cost:* ~35 min and ~3.5 GB.
**R16-11 (measure first).** 2015 top-3000 measured natively before the batch, seven checks; fall back to a thin exporter if panel peak WS > 3 GB. *Cost:* ~4 minutes.
**R16-12 (layout).** Baselines directly under `C:/atx/data`, immutable, attempts versioned. *Cost:* every cell fails the required-marks check immediately and loudly.
**R16-13 (LEAN path, user-chosen).** Panel-side C++ only; `equity-ic` unmodified (no as-of predicate, no cp16 ledger mode); sidecar trial ledger; no design review, no end review, no oracle suite (one hand-checked cell), no separate receipt writer; kept: span ingest, measure-first, SHA-frozen pre-registration, all caveats printed. *Cost:* a reviewer may call the sidecar a weaker pre-registration, and if the year-union bias is larger than assumed the per-year numbers need re-running under an as-of predicate - a new checkpoint, not a patch.
**R16-14 (CI percentiles).** 2.5/97.5 nearest-rank round-half-up, matching cp14, so bar 1 is strictly stronger than the 5th percentile R16-8 names. *Cost:* one recomputation from the same draws.
**R16-15 (turnover source).** `implied_turnover = 1 - rho_rank` from `signal_autocorr.csv`, not `quantile_spread.csv`. *Cost:* one column re-read; both files are published.
**R16-16 (zero dispersion).** `sd == 0` or `n_obs < 2` => blank Sharpe, `reportable = 0`, reason `zero-dispersion`. *Cost:* degenerate cells only.
**R16-17 (bootstrap seeding, parent-amended).** stdlib `random.Random(20260920)` per group in a fixed order; the engine's splitmix64/Xoshiro stream key is NOT reproduced. *Cost:* the CI is reproducible from the script but not bit-comparable to any engine CI - stated in the caveats.
**R16-18 (`blend_equal` hole inheritance).** 2018 `blend_equal` rows carry `hole_flag`, as do all pooled rows. *Cost:* one extra flag; flags never change a number.
**R16-19 (materiality scope).** The test runs per signal per cut; any fire opens attempt 2; inevaluable => no attempt 2, printed. *Cost:* one attempt opened or not.
**R16-20 (unreportable rows).** Point estimate printed, interval blank, never feeds a bar; `n_obs < 20` carries a printed wide-interval caveat. *Cost:* a reader over-reads a per-year interval, which the caveat exists to prevent.
**R16-21 (evaluation start, parent-amended).** S(Y) = first NYSE session >= Y-01-01 (2013-04-04 / 2014-01-02 / 2015-01-02 / 2016-01-04 / 2017-01-03 / 2018-01-02 / 2019-01-02); panel `--start` = (Y-1)-01-01 minus 10 d, 2012-03-26 for 2013. *Cost:* a stage refuses the window with an explicit error before anything is written.
**R16-22 (turnover granularity).** One implied-turnover value per signal serves every horizon, variant and restriction, because `signal_autocorr.csv` is keyed by signal only; labelled. *Cost:* the turnover column is less granular than ideal.
**R16-23 (pooled CI).** Pooled CI blank when pooled `n_obs < 8` (h=63 single-year cases). *Cost:* none - disclosure.
**R16-24 (baseline pin widening; parent edit, 1st deviation from design section 4).** `stage_equity_baseline.cpp::require_context_recipe` now accepts EITHER the cp14 screen OR the cp16 membership recipe (`membership_rule == "year-union-plus-last-prior-rebalance;not-as-of"` + `universe_membership_sha256` + `universe_cut` + top_n 0 / min_adv 0 / min_price 1.0), nothing in between; helpers `universe_screen_is_cp14` / `universe_screen_is_cp16_membership`. Drifts the cp13/cp14 receipts' `stage_equity_baseline.cpp` pin - recorded, receipts never touched. *Cost:* the baseline profile `slow-momentum-equal-weekly-shaping-v1` now admits a second, declared, context class.
**R16-25 (signals-only baseline commit; parent edit, 2nd deviation).** For membership contexts the baseline stops after `combo.bin` and commits `status = "complete-signals-only"`, `qualification = "not-attempted"`, `replay = "skipped-membership-context-signals-only-no-book-result"`, parents source-context/evaluation/combo only; cp14-screen contexts still run the full replay. *Cost:* **the cp16 baseline directories are NOT book baselines and must never be cited as one** - the status string enforces it.
**R16-26 (materiality).** The fired test (`momentum_252`, both cuts) AUTHORISES a pre-registered attempt 2 but it was NOT started: the test cannot separate the archive hole from a genuine 2016 momentum reversal, the per-year cells carry ~12 observations, and whether to spend time on data remediation is the user's decision the scorecard exists for. *Cost:* one session of delay on attempt 2.

Process rulings carried forward still apply: two-line parent fixes without a re-review round
(used again for the `allow_ids{}` default member initializer); a runner patched mid-flight is
versioned, never silently re-run; a source edit that drifts an older receipt's pin is recorded
in an addendum, never patched into the receipt.
## 5. Open items

### 5.1 Pre-existing, unrelated, open
`StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard` still fails
(`invalid stod argument`, CTest #944), untouched this session and confirmed as the ONLY
failure in the 466-test `atx-impl-tests` run.

### 5.2 Lean-path shortcuts to retire (new; the price of R16-13)
1. **As-of membership.** `equity-ic` has no per-date membership predicate; the cut is a
   year union applied once at compaction. This is a declared within-year selection
   look-ahead, uncorrected. Retiring it means the as-of predicate ANDed into `out.mask`
   (`stage_equity_ic.cpp:526` per R16-1) and re-running all 13 cells.
2. **Checkpoint 16 ledger mode.** cp16 wrote a sidecar ledger, not the canonical one, and
   the sidecar's per-run "30 declared" is the binary's constant. Retiring it means the
   R16-2 stage mode (checkpoint 16, non-trial purpose
   `cp14-configuration-restriction-evaluation`, `trial_count_declared 0`, trial ids
   `iteration16-cp14-restriction-NNNN`) and the `kNonTrialPurposes` entry.
3. **Oracle suite.** Replaced by ONE hand-checked cell; the point estimate was reproduced
   exactly, the CI was not independently checked.
4. **Separate receipt writer.** The scorecard script emits its own `receipt.json`; there is
   no independent 47-check-style validation receipt as cp14/cp15 have.
5. **Design review.** None was run on the cp16 design, and no end review was run on the
   checkpoint.
6. **Design SHA not embedded in C++.** Unlike cp14/cp15 the frozen cp16 SHA is verified only
   by the Python scripts, not fail-closed by a stage.

### 5.3 Measured deviations and disclosures open for the next checkpoint
- **The fired materiality test (R16-26).** `momentum_252` on both cuts: 2016 lies outside
  the union of the 2015 and 2019 intervals. Attempt 2 (`tickerhistory-qa-v2` re-ingest
  and/or a hole-aware rank rule on the last full session) is AUTHORISED and NOT STARTED, as
  a fresh pre-registration under a versioned `--attempt`.
- **2017 top-3000 NOT FIT.** Declared before any number existed (union 5,072 > 4,096, an
  artefact of the 2016-12-30 / 2017-01-31 rebalance pair, itself hole-contaminated). The cap
  was not raised (RR-1 stands). Every top-3000 pooled figure rests on six years, not seven.
- **The signals-only baseline directories.** `C:/atx/data/equity_scorecard16_base_*` carry
  `status = "complete-signals-only"` and no book result. They are valid `equity-ic` parents
  and nothing else.
- **Receipt pin drift on `stage_equity_baseline.cpp`.** R16-24 (and R16-25) edited a file
  pinned by the cp13 and cp14 receipts. This is a legitimate later edit; the receipts are
  untouched and the drift is recorded here, in `PLATFORM_PROGRESS.md` and in addendum 1.
- **Executable split across the run.** All 13 cells used `d1224b32…`; the measure cell's
  panel was built by `33c6ca8b…`. Panel code is identical between them and the recipe binds
  the digest, so the difference is disclosed rather than hidden.

### 5.4 Carried from v3 §5, unchanged
cp13 T3–T5 deferred. The data hole itself (19 corrupted pre-holiday sessions
2016-01-15…2018-02-16, `ohlc_order_violation` under qa-v1) and its cp15 contamination list.
`coverage_by_year` attribution (cp15 I-1: engine attributes by RANK-session year) — cp16
derived its own attribution from effective sessions per R16-3, so the two conventions
coexist and the next checkpoint must keep saying which it uses. Identity caveats: 56
same-ticker ID-segment overlaps, 610 tickers used by more than one security id, 955 ids with
more than one `todayTicker`, vendor `shares` reported-only, instrument type unknown
accepted, 2018's 83 duplicate-key dates quarantined. `kMaxIcInstruments` 4,096 vs. the cp15
cumulative unions (t2000 4,447 / t3000 6,624). cp15 I-4/I-5 (fallible calls between the
ledger pre-registration append and the guarded stage body; no write-failure test after
pre-registration). All deferred minors enumerated in `final-branch-review.md` and
`cp15-end-review.md`.

## 6. How to resume

1. Read this file, v1 §"Constraints", v2 §4–§7, v3 §4–§7, `progress.md` lines 110–146, the
   frozen cp16 design §1–§3 and §11, addendum 1, and the scorecard in full — **including its
   caveat block**, which is load-bearing for every number quoted above.
2. Verify §2.1 pins with `sha256sum`; cp12/cp14/cp15/cp16 designs and the cp16 addendum MUST
   match exactly. Verify the canonical ledger is still 4 lines and the sidecar 26.
3. Native loop (parent-only, serialized, ≤ 2–3 wrapper calls per PowerShell process,
   `CCACHE_DISABLE=1`, never raw cmake/ninja) — command block copied verbatim from v2 §6 /
   v3 §6:
   ```powershell
   $env:CCACHE_DISABLE='1'
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 configure -Preset equity-dev -Groups 'risk;data;core;book;eval' '-DVCPKG_MANIFEST_MODE=OFF' '-DVCPKG_INSTALLED_DIR=C:/Users/natha/vcpkg/installed' '-DFETCHCONTENT_BASE_DIR=C:/atx/.worktrees/equity-platform/deps/equity-dev'
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 check 'atx-impl\src\<file>.cpp' -Preset equity-dev
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 build atx-impl-tests -Preset equity-dev -Jobs 4
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 -Ctest -Preset equity-dev -Jobs 1 -R '^(ConfigEquityIc|EquityIcTerminalValue|StageEquityIc|TrialLedger)\.' -VV --output-junit 'C:/atx/.worktrees/equity-platform/build-equity/<name>.xml' --output-log 'C:/atx/.worktrees/equity-platform/build-equity/audits/<name>.log'
   ```
   Carried traps (v2/v3): PowerShell 5.1 swallows ctest `-O`/`--verbose` (use `--output-log`,
   `-VV`); `check` on a new TU needs a reconfigure first; a relative `--output-junit`
   resolves against the build dir; **`atx-impl-tests` does NOT rebuild `atx-impl.exe`** —
   build `atx-impl` explicitly before a real-data run, and never while a run holds the exe;
   `-Wunused-result` / `-Wrange-loop-construct` / `-Wmissing-field-initializers` are fatal
   under `/WX`; Bash background dies at 10 min — detach long native runs with `Start-Process`
   and monitor the PID; always `--dry-run` a runner first; never re-run a receipt writer,
   version a new attempt.
4. **New traps from checkpoint 16** (each one cost a failed attempt):
   - Python stdout is **block-buffered** under `Start-Process`, so a detached runner's log
     stays empty for minutes: infer progress from the output directories appearing under
     `C:/atx/data/`, not from the log tail.
   - `argparse` eats a leading dash in an option value: pass `--receipt-tag=-x`, the `=`
     form, never `--receipt-tag -x`.
   - **Never pre-create a stage's output root.** `equity-baseline` and `equity-ic` refuse
     with "output root must not exist"; only the panel directory may be made in advance.
   - Runner log files are opened in mode `"x"`: version an old log before relaunching or the
     runner crashes before it starts.
   - PowerShell here-strings do **not** pipe into `python -`; use a Bash heredoc instead.
   - Inside a Python heredoc, `"\n"` becomes a literal newline in the generated source unless
     you double the backslash — this broke one `check` run.
   - A failed run leaves a directory behind: version it (`*_failed1..3`) and keep the log.
5. **Three concrete next investments** (the scorecard exists to let the user choose one; do
   not drift into one):
   - **(a) Stage 3 — alpha families beyond momentum.** The measured failure is signal, not implementation, and the evaluation machinery is built and measured, so a new family reuses it end to end. Cost: new signal definitions in the baseline/combo path (C++ scope not estimated — (not recorded)), one new pre-registration, and a 13-cell batch at the measured per-cell cost (panel 69–92 s, baseline 5.5–17.8 s, `equity-ic` 26.6–134.8 s, peak WS < 2.8 GB). The 2012–2019 span ingest is done and does not repeat.
   - **(b) Hole remediation attempt 2** (authorised by R16-26). Cost: a `tickerhistory-qa-v2` re-ingest at roughly the measured span-ingest cost (754 s prepare + 615 s load, ≈ 3.5 GB per R16-10) and/or a hole-aware rank rule, plus a re-run of the contaminated cells and a fresh pre-registration. Buys clean 2016/2017 rows; cannot by itself turn a flat seven-year result positive.
   - **(c) Retire the lean shortcuts** (§5.2). Cost: the as-of predicate in `stage_equity_ic.cpp` (R16-1 estimated ≈ 40–70 lines plus tests), the cp16 ledger mode and `kNonTrialPurposes` entry, an oracle suite, a separate receipt writer, a design review — and a full 13-cell re-run, since the numbers change. Changes no signal; removes the within-year look-ahead and the sidecar-ledger objection.
6. Whatever is chosen: the design note comes first, frozen with a SHA before any number
   exists; bars are written before the run; the canonical ledger stays at 4 lines unless a
   ruling says otherwise; and no checkpoint 14 or checkpoint 16 receipt is ever edited.

## 7. Agent/process conventions used this session

As v3 §7 (fresh implementer per task, edit-only, reports to `cp<N>-task-<T>-report.md`;
parent builds/tests and appends every measurement and ruling to `progress.md` before the
next dispatch; independent read-only reviewers write `...-review.md` with verdict lines;
receipts are immutable Python-written JSON with pins, failed attempts versioned; Edit/Write
tools for file changes, not Python workarounds; agents cannot be resumed across a session
restart — re-dispatch fresh with the same brief), plus:
- **Lean path (R16-13).** When the user asks for the shortest honest path to a number, the
  parent may drop the design review, the end review, the oracle suite and the separate
  receipt writer, and may route a checkpoint's ledger lines to a sidecar file — provided the
  design is still frozen with a SHA before the first number, the acceptance bars are still
  written first, every caveat is still printed beside the numbers, and every dropped control
  is enumerated as an open item (this file's §5.2). The shortcuts are the cost of speed and
  must be stated as such, never quietly.
- Two implementers on strictly disjoint files can run in parallel without a review round
  between them (`cp16-cpp` on the panel/config/stage sources, `cp16-py` on the runner and
  scorecard scripts).
- A parent-made deviation from a frozen design during measurement (R16-24, R16-25) is
  recorded as a numbered addendum to the design that does NOT change the design SHA, plus a
  ruling line in `progress.md`, before the run that depends on it is accepted.
- A post-hoc diagnostic computed after a verdict is labelled "not pre-registered, not a
  trial" everywhere it appears and never feeds a bar.
