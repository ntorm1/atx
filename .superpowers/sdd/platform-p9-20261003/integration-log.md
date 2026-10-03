# P9 integration log

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. One section per integration step, appended in order.
Detail for wave 1 lives in `root-wave1-merge-report.md` (same directory); this log keeps the summary, the gates and
the open items.

## P9 wave-1 merges

### M1a: slots 1-4 (E1, T1, A1, A2), 2026-10-03

Base `6a68d7f9` (R0-14 cut). Head after M1a: the report commit on top of `96c0cfee`.

| slot | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity |
|---|---|---|---|---|---|---|---|---|
| 1 E1 | `fdd2bda3` | `28cd7d8c` | none | none (Python only) | - | - | scripts/tests x2 seeds 352p/4s/1f each (1f pre-existing); engine/tools 349p; impl/tools 625p/2s | `wave plan y-s` identical pre/post (`990cad41`); `wave status` 4 waves 9/9 unchanged |
| 2 T1 | `0a61b706` | `769ca3d3` | none | none of its own (CMake only; configured in p9-1a/b) | - | - | scripts/tests x2 seeds 373p/4s/1f each (same 1f); strategies 14p; impl/tools 625p/2s; eval_tie 7p | slice-2 `--check` before and after: v71 `787c802e`, recipe.v2 `69e95298`, 19 frozen, exit 0 |
| 3 A1 | `f8edca96` | `de925dca` | none | none (Python only) | - | - | engine/tools 399p; fixtures 9p/1s; focused 50p | registry `ok 92`, `6c56b739` |
| 4 A2 | `2b6f8e6f` | `b50c0a1f` | `atx-engine/tests/CMakeLists.txt` tail (T1 + A2 appends), both kept | p9-1a FAILED (slip); **p9-1b** `atx-engine-research-fields, atx-engine-research-fields-tests, atx-research-fields`, 19.2 s | 0 | Registry 7/7, Manifest 8/8, Fixture+Cli 6/6; `ResearchFields*` 44p/2f (2f pre-existing) | engine-path real exe 11p/0s; engine/tools 399p; fixtures 12p/1s | v15 `--registry`: 84/84 payloads, 82 reused + 2 recomputed as predicted, **2 metadata path groups outside A1's list** -> STOP; A2 TRAIN 3/3 = v15 |

Extra commits: `a97d0483` (registry flip of si_shares, si_dtc, vol_126 to kind engine through A1's `regenerate()`,
sha256 `6c56b739` -> `835b93ea`, `check` passes, round-trip byte-identical) and `96c0cfee` (slip: `-Werror,
-Wfor-loop-analysis` at `research_fields_cli.cpp:243`, loop now steps by two).

CTest (listing only): `-N -L atx_research` = 90 (fields-tests 46 = 31 + A2's 15, mine-tests 44; T1 predicted 75 before
A2's cases); `-N -L atx_equity_strategy` = 537.

Trial ledger: 133 lines, `27e40f9f` (0 trials). Real runs (bounded runner, memory gate free >= cap + 1,536 MiB):
v15 identity (cap 1,536, 38.9 s, peak 315 MiB), A2 TRAIN identity (cap 1,536, attempt 1 refused at spec parse in
0.3 s -- my missing empty output dir -- attempt 2 27.5 s, peak 192 MiB). Nothing dated 2024-01-01 or later opened;
outputs compared by SHA and JSON path only.

### Open items (PM)

1. **v15 identity metadata outside A1's predicted list** (payloads all identical): `source_checks.v9.earn_season_rank`
   moved into `reuse.prior_source_checks_of_partial_groups.v9` (content equal modulo the key rename; the A1 review
   had flagged this as a deferred minor), and `source_checks.holdings.code.*` = the new file hash of
   `research_fields_holdings.py` (A1 rewrote it). Needs a ruling before these count as the accepted identity (and
   for the DEC-20 substitution list).
2. **Pre-existing at the base, not from wave 1:** `scripts/tests/test_research_mine.py::
   test_fields_are_the_rule_applied_to_the_registry` (v8 `a5914373` registry rows vs the test's `EXCLUDED_BY_CLASS`)
   and gtests `ResearchFieldsWriter.QuantilesPartitionLikeNumpy`, `ResearchFieldsVolumeMean.SumOrderIsNumpys`
   (unchanged v8 code; the exe was never in CTest before T1). They block the wave gates "scripts/tests 0 failed" and
   `ctest -L atx_research` until ruled.

### Platform gates (G-P) checkable after M1a

- G-P4 (CTest registration): partial -- `atx_research` lists 90 tests across both registered research exes; final
  count after B1 (admission target) is M1d's.
- G-P5 (mirror guard): `test_no_python_mirror.py` green under both seeds, printing 37 mirrored rule functions in 14
  rows; A1/A2's seal-rule pair is not yet a `FROZEN_PAIRS` row (A2 open risk; root adds it under a ruling).
- G-P6 (no versioned script): the 11 class-C generators (+ 9 tests) are deleted after root's slice-2 `--check`
  passed (before and after the merge); `test_no_versioned_scripts.py` green under both seeds. Checkable now; tick at
  M1d with the full suite.
- G-P7 first half (eval_tie): `generate_eval_tie.py --check` tied; fixture test 7 passed.
- G-P8 (canary goldens): not yet -- recorded after C1 (M1c/M1d).

### M1b: slots 5-7 (S1, B1, D1), 2026-10-03

Base `20443022` (S1 merged and its MARGINAL_BUILT note applied by the killed root-m1b; slot 5 re-derived from git and
run receipts by root-m1b2). Detail in `root-wave1-merge-report.md` "M1b".

| slot | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity |
|---|---|---|---|---|---|---|---|---|
| 5 S1 | `34ef92dd` | `6476b927` (+ `20443022` E1 x S1 note) | none | **p9-1c** `atx-equity-strategy-ic, atx-impl-strategy-ic-tests`, 97.9 s | 0 | Debug S1 filter 31/31 (MarginalIc 15, IcIdentity 1, CombineMarginalRankIc 13, 2 pins); Release deferred to M1d (no Release tree) | `test_research_cycle.py` 92p/3s x2 seeds | flag-absent Debug X-5 marginal: old `985019d9` (v8-16d = pre-S1 code) vs new `4e321143` byte-identical outside `stage_seconds`; also = v8's recorded output |
| 6 B1 | `01f20405` | `1ab97798` | 3 CMake list tails (`atx-engine/tests`, `atx-impl`, `atx-impl/tests`), all blocks kept | **p9-1d** `atx-equity-strategy-targets, atx-impl-strategy-target-tests, atx-engine-research-admission, atx-research-admission, atx-engine-research-admission-tests`, 31.3 s | 0 | FactorsVerb 4/4; ResearchAdmission 16/16 (report said 17: review m9); `Exposures.*` only in `atx-impl-tests` -> M1d | `test_factor_series_admission.py` 8p/0s with both exes; exposures_export + horizon_stats 12p/0s with the exe | X-3 (X-5's parent) NAV old `72ff6d2d` vs new `5149aab9`: 27/27 files identical; admission comparator X-5 (58) and Y-S (73): factor series 0 diffs, admission.csv 0 diffs under P12 (worst 5.4e-15), admitted order / counts / sign conflicts equal |
| 7 D1 | `757ba582` | `1239a5ff` | 2 CMake list tails (`atx-impl`, `atx-impl/tests`), all blocks kept; in `atx-impl/CMakeLists.txt` the hunk shared B1's `endif()`: each block keeps its own | **p9-1e** `atx-equity-strategy-ic, atx-impl-strategy-ic-tests, atx-impl-strategy-target-tests, atx-impl-tests`, 117.8 s; **p9-1f** = B1's five rebuilt after D1, 6.2 s (0 TUs, 1 link) | 0 / 0 | D1 new 8/8; D1 regressions 132/132 (15 suites); TwoSpeed 20/20; S1 filter 31/31 and B1 FactorsVerb 4/4 + ResearchAdmission 16/16 post-D1; impl-tests CompositionRules + Exposures + FactorsVerb 14/14; `--list-rules --json` JSON-equal to the fixture | D1's ten fitter/composition suites 231p (= lane); B1 suites post-D1 20p/0s | X-5 flag absent (R0-3 procedure): fit = X-5 after PM7-30 subs; cold w 10/12 + 2 seconds/cache-path only (I/O counters equal; 58/58 cache payloads equal); NAV 27/27 (S2 `529062d6`); flag present: only `theme_registry_sha256` added |

Extra commits: `7d618e24`, `394fc8b8`, `e6b583f2` (slot 5 / 6 report and log) and the slot 7 report commit. No code
slip in slots 5-7 (S1's `20443022` was the planned E1 x S1 merge note). Real runs (bounded runner): slot 5 reused the
killed agent's two marginal runs after proving exe provenance (old `985019d9` = v8-16d, new `4e321143` = p9-1c);
slot 6: X-3 NAV x2, factors / screen on X-5 and Y-S (one refused attempt: my repo-relative payload paths); slot 7:
X-5 fit, cold w (2,401 MiB peak), NAV, flag-present w. 0 trials; trial ledger 133 lines `27e40f9f`. Nothing dated
2024-01-01 or later opened; compared by SHA and JSON path only.

### Open items after M1b (for M1c / M1d / PM)

1. S1 Release gtests and Release marginal adoption deferred to M1d (no Release tree in pool-2; first `equity-rel`
   configure fetches deps into `deps/equity-rel`).
2. Next build tag `p9-1g`. C1's `atx-impl/CMakeLists.txt` append will meet two `if()...endif()` blocks at the tail.
3. B1 / E2 / D2: `signals.json` payload paths must be absolute or relative to the signals DIR.
4. Known reds (M1a-RED) not exercised by M1b's filters; failure set in M1b runs: empty.

### Platform gates (G-P) after M1b

- G-P3 (Release IC adoption): Debug half shown for S1 (flag-absent marginal identity, `build_vm_identity` legacy
  `dslvm1_clang18.1`); Release half is M1d's.
- G-P4 (CTest registration): `atx-engine-research-admission-tests` now exists (T1's deferred call registers it under
  `atx_research`); D1's rules test is in the `atx_equity_strategy` IC target. Counts: M1d.
- K-P9-4 (B1) and K-P9-6 (D1) contracts exercised on real data / the built exe (see the M1b report).

### M1c: slot 8 (C1), 2026-10-03

Base `d656bbfa` (M1b hand-off). Lane C1 head `10c35df3` (code head `a275088b`; sqrt-only commit `dd925b7f`).

#### C1 NAV re-pin: DEC-20 substitution list (written before any C1 run)

Written by root-m1c before the stage-1 build and before any C1 NAV run; committed on its own. Source: C1 report §4
(fix round 1, M1 + M2), ruling C1-PROD, plan §2.4 C1. Compared by file SHA-256 and JSON path only; no value of a
summary, daily or events file is read.

Procedure in pool-2. C1 is merged in two steps so each stage builds a committed source (the bounded runner refuses a
dirty code pathspec): first `git merge --no-ff dd925b7f` (sqrt only, on the wave-1 head) = the stage-1 source, then
`git merge --no-ff 10c35df3` (the whole lane) = the stage-2 source. The final tree is the one a single merge of
`10c35df3` gives. Every run: the argv of `build-equity/p9-d1-x5-nav-run/receipt.json` (X-5's NAV, L 1.1720, argv sha
`7b517c04`) with only `--output` changed; `run_bounded_research.py`, `--build-type` set, cap 1,536 MiB, floor 512,
admission wait 900 s.

Labels: L0 `linear-6bps-stale5-v1+swap-fin-v1` (S1 law, no sqrt); L1 `modeled-1bn-stale5-v1+swap-fin-v1` (primary);
L2 `modeled-1bn-terminal-adverse-v1+swap-fin-v1`; L3 `modeled-1bn-stale5-v1+flat-300-v0`; L4
`modeled-1bn-stale5-v1+engine-tiers-v1` (L1-L4: S2 law, delta .5). Capacity books K = `capacity-x{0p5,1,2,4,8}-v1+swap-fin-v1`.
summary.json `scenarios[i]` = Li (index order L0, L1, L2, L3, L4 in the reference).

**Stage 1** = Debug (equity-dev) exe built at the dd925b7f merge (tag p9-1g) vs `build-equity/p9-d1-x5-nav`
(pre-C1 head `1239a5ff`, exe `f0ee3908`; 27/27 = X-5's own dir and R0-3's `v8-i16d-x5-nav`).

| file (27) | stage 1 |
|---|---|
| `daily_L0.csv`, `events_L0.csv`, `recipe.json`, `capacity/recipe.json` | byte-identical |
| `daily_` / `events_` of L1-L4 (8 files) | may differ (sqrt) |
| `capacity/daily_K`, `capacity/events_K` (10 files) | may differ (sqrt) |
| `capacity_curve.csv`, `v7_transfer_coefficient.csv` | may differ (sqrt) |
| `summary.json` | may differ only at `/scenarios/1`, `/scenarios/2`, `/scenarios/3`, `/scenarios/4` (whole subtrees), `/warm_start/score_begin_gross_leverage/<L1..L4>`, `/v7/books/<L1..L4>` (subtrees); `/scenarios/0`, `/recipe_sha256`, `/locate_in_aim` and every other path identical |
| `capacity/summary.json` | may differ only at `/scenarios/0..4` (subtrees), `/warm_start/score_begin_gross_leverage/<K>` (all five), `/v7/books/<L1..L4>` (subtrees); `/v7/books/<L0>` and every other path identical |
| `v7_extras.json` | may differ only at `/files/v7_transfer_coefficient.csv`, `/files/capacity_curve.csv`, `/capacity/0..4` (subtrees); `/capacity_x1_equals_primary_bit_for_bit` stays `true` (identical) |

No file on one side only. Zero differences is a legitimate stage-1 result (the Debug CRT's `pow(x, .5)` may already
round like `sqrt`).

**Stage 2** = Debug exe built at the 10c35df3 merge (tag p9-1h, C1's three targets) vs the stage-1 output.

| file | stage 2 |
|---|---|
| all 20 `daily_*` / `events_*` CSVs (main and `capacity/`), `recipe.json`, `capacity/recipe.json`, `capacity/summary.json`, `capacity_curve.csv`, `v7_transfer_coefficient.csv` (25 files) | byte-identical |
| `summary.json` | differs only at `/v7/extras` (changed: the bound sentence), `/v7/files/*` (added: SHA-256 of `v7_extras.json` and `capacity/summary.json`), `/producer/*` (added: `engine_git_sha`, `build_type`, `definition`; C1-PROD) |
| `v7_extras.json` | differs only at `/files/capacity/summary.json` (added) |

No other file and no file on one side only. Anything else is a structural defect (per-book BookState / BookLeverage,
the capacity lockstep, book_groups, the record merge) and a STOP.

**Release clause** = Release (equity-rel) exe built at the same merge (C1's three targets, first Release tree in
pool-2) vs the stage-2 Debug output: every file byte-identical after dropping `summary.json` `/producer` (build type
differs by design); `/v7/files` stays identical because the files it binds are identical. Per plan §2.4 C1, a
difference beyond `/producer` is CRT build noise outside the sqrt path (`guarded_move`'s `std::log`, the p95 log /
log10 / pow(10), the cagr `pow`), not a C1 defect: then NAV stays Debug, G-P3's NAV half is reported unmet, and the
first differing file and column (header name only) is recorded. If the build wrapper's memory gate does not admit the
Release configure + build within 20 minutes: "Release clause deferred to M1d".

Exe identity: `summary.json` `/producer` is the only exe-identity location (C1-PROD); cross-build comparisons drop it.

#### Result (detail in `root-wave1-merge-report.md` "M1c")

| slot | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity |
|---|---|---|---|---|---|---|---|---|
| 8 C1 | `10c35df3` | `d71cabe1` (step 1: `dd925b7f`, sqrt) + **`b52a5de7`** (step 2: `10c35df3`) | none (C1 edits no CMake file) | **p9-1g** `atx-equity-strategy-targets` (stage-1 exe), 14.8 s; **p9-1h** `atx-engine-w1-cost-tests, atx-impl-strategy-target-tests, atx-equity-strategy-targets`, 113.9 s; **p9-1i** `atx-equity-strategy-ic, atx-impl-strategy-ic-tests, atx-impl-tests` (earlier lanes linking C1's libraries), 22.7 s | 0 / 0 / 0 | w1-cost `ReplayCostSqrt.*` 3/3, whole 49/49; C1 new 13/13; C1 regression 160/160; target-tests whole 338/338; S1 31/31, D1 8/8 + 132/132, impl-tests D1/B1 14/14 + C1 13/13 | nav_summ 29p/1s; B1 exe suites 20p/0s with the p9-1h exe | stage 1 (sqrt vs `p9-d1-x5-nav`): 10 files moved, all inside the list (3 S2 dailies, 5 capacity dailies, 2 summaries at `daily_csv_sha256` + 1 cost leaf); stage 2 (lane vs stage 1): 25/25 identical, `summary.json` only `/v7/extras`, `/v7/files`, `/producer`; `v7_extras.json` only `/files/capacity/summary.json` -> **both hold** |

New Debug X-5 NAV reference (DEC-20 re-pin): `build-equity/p9-c1-s2-x5-nav` (exe `bf4b0ec2`, S2 daily `75a54774`).
0 trials; trial ledger 133 lines `27e40f9f`. Nothing dated 2024-01-01 or later opened.

**Release clause** (after the Debug commit `61ac4423`): the memory gate admitted at once (free 5,648 MiB). Configure
`equity-rel` exit 0 (121 s, first Release tree in pool-2). **p9-1j** (equity-rel, C1's three targets) exit 0, 802.6 s,
314 TUs; 0 first-party warnings (11 third-party lines: spdlog `/MP` x7, vendored databento `getenv` x4); targets
`93ea323e`. Release gtests: `ReplayCostSqrt.*` 3/3 (w1-cost) and in C1's new filter 13/13; w1-cost whole 49/49;
target-tests whole **336/338**: `BookNormalScore.TiesShareTheMeanRankAndMirrorsAreOpposite` and
`BookNormalScore.FixtureTellsWrongRulesApart` fail in Release only (1-ULP `EXPECT_EQ` vs `norm_ppf`; v8 `02633038`
code unchanged since `d7c1c520`; Debug 338/338). Not from C1; needs a ruling. X-5 NAV Release (`p9-c1-rel-x5-nav`,
41.7 s) vs stage-2 Debug: 26/27 byte-identical, `summary.json` only `/producer/{build_type, engine_git_sha}` ->
**Release clause holds** (G-P3 NAV half shown on X-5).

### Open items after M1c (for M1d / PM)

1. **Ruling needed:** Release-only `BookNormalScore.*` pair in `atx-impl-strategy-target-tests` (v8 norm-score-v1
   kernel `normal_score.hpp`, not a wave-1 change). Today the M1a-RED gate lists only three known reds.
2. Next build tag `p9-1k`. Release tree `build-equity-rel/` exists (configured at `61ac4423`); Debug tree configured
   at `1239a5ff` (stale `producer.engine_git_sha` in Debug NAV outputs until it is reconfigured).
3. Canary goldens (G-P8) can be recorded now that C1 is in (Debug, then Release). S1 Release gtests and Release
   marginal (G-P3 IC half) remain M1d's.
4. P9-B0: the C1 part of the substitution list = the stage-1 and stage-2 lists above.

### Platform gates (G-P) after M1c

- G-P3 (Release adopted), NAV half: Release X-5 NAV = Debug bit for bit outside `producer`, with the sqrt probe
  green on both presets. Shown for X-5 (no rank shape); norm-score-v1 books are not covered (item 1). IC half: M1d.
- NV-1..NV-4 (plan §1.2): C1's deliverables merged and exercised (capacity lockstep, summary binding, producer, sqrt).

### M1d: post-merge gates, 2026-10-03 (STOPPED by the owner, partial; `handoff-root-m1d.md`)

From `ad406718` (code `b52a5de7`). Debug tree reconfigured at `ad406718`; build **p9-1k** (14 test / exe targets)
exit 0, 193.3 s, 36 TUs, 0 warnings. Debug suites: `ctest -L atx_research` 104/106 (failures = M1a-RED's two gtests,
same assertion text); `-L atx_equity_strategy` 577/577; engine alpha 771, factory 392, book 184 all passed; golden
`0x889874a3b9b29c55` holds at 1 and 4 workers. Not done: `atx-impl-tests` whole, all pytest (killed mid-run at the
stop, no result), canary goldens, Release IC, P9-B0 (no substitution list written), scoreboard / timings. 0 trials;
ledger 133 lines `27e40f9f`. Next build tag `p9-1l`.

G-P after M1d (partial): G-P4 counts measured (atx_research 106, atx_equity_strategy 577; registered and running) --
tick once the wave-1 suite gate passes with pytest; G-P3 IC half, G-P8, G-P6 tick still open (blocks 2-3 not reached).

### M1d resume (root-m1d resume, 2026-10-03): P9-B0 substitution list (written before any P9-B0 run)

Written and committed alone before any P9-B0 run (DEC-20; Ruling B0-Y1). Compared by file SHA-256 and JSON path only;
for CSVs only header names, row keys and counts of differing rows / cells, never a value. No statistic is read.

**Current parent and runs.** Parent = Y-F0 (`scripts/specs/v8/lib-v8ysb-gm.json`, the last accepted unlevered cell;
plan §5.2). Its u / fit / w / NAV, plus the two levered NAV books on the same w that later cells pair against: X-10
(`x-leverage-L2.0.json`, fixed L 2.0) and Y-1 (`y-vol-target-y-1.json`, vol-target-v1 at L 2.0; brief-COV step 4
needs its re-based reference). Every run = the reference receipt's argv with **only `--output` changed** (scratch
`rerun.py`: asserts exactly one token substituted), through `run_bounded_research.py` with the reference receipt's
limits, `--build-type Debug` for an exe, admission wait <= 3,600 s. Debug tree `build-equity` (equity-dev,
configured at `ad406718`), bin = p9-1k: ic `393183c0`, targets `407c34ad` (code = wave-1 head `b52a5de7`). 0 trials.

| run | reference (exe / source) | new dir |
|---|---|---|
| u | `mega-v8-b0b-train-u-v8ysb-1` (receipt `-run1`; ic `985019d9` = v8-16d, `816be40b`; warm shared cache) | `p9-b0-yf0-u` |
| fit | `mega-weights-v8x-theme-erc-v8ysb` (`-run1`; fitter at `816be40b`) | `p9-b0-yf0-fit` |
| w | `mega-v8xw-train-theme-erc-v8ysb-2` (`-run2`; ic `985019d9`; warm shared cache) | `p9-b0-yf0-w` |
| NAV Y-F0 (L 1.1828) | `mega-nav-v8x-theme-erc-L1.1828-v8ysb` (targets `72ff6d2d` = v8-16d) | `p9-b0-yf0-nav` |
| NAV X-10 (L 2.0) | `mega-nav-v8x-theme-erc-L2.0-v8ysb` (`72ff6d2d`) | `p9-b0-x10-nav` |
| NAV Y-1 (vol-target, L 2.0) | `mega-nav-v8y-vol-target-L2.0` (`72ff6d2d`) | `p9-b0-y1-nav` |

Each stage consumes the reference's own inputs (the argv pins them), so each stage is compared alone.

Code basis: `d7c1c520..816be40b` and `d7c1c520..1239a5ff` change no NAV, vol-target or fitter source (git log / diff
empty for `fit_composition_weights.py`, `strategy_nav_*`, `strategy_risk_target.*`, `strategy_vol_target.*`,
`strategy_cost_v2.cpp`, `atx-engine/src/book/`); so the references are the v8-16d code, and the moves since are S1 / D1
(IC, fitter: shown byte-neutral flag-absent in M1b) and C1 (NAV: sqrt + structure, M1c).

Labels as M1c: L0 `linear-6bps-stale5-v1+swap-fin-v1` (FlatBpsV1, no sqrt); L1 `modeled-1bn-stale5-v1+swap-fin-v1`
(primary); L2 `modeled-1bn-terminal-adverse-v1+swap-fin-v1`; L3 `modeled-1bn-stale5-v1+flat-300-v0`; L4
`modeled-1bn-stale5-v1+engine-tiers-v1` (L1-L4 SqrtImpactV1, delta .5); K = `capacity-x{0p5,1,2,4,8}-v1+swap-fin-v1`
(S2 law). The three NAV references hold exactly these 27 files (Y-1: + `vol_target.csv` = 28).

**u** (`p9-b0-yf0-u` vs reference; 5 files):

| file | expected |
|---|---|
| `orientations.json`, `recipe.json`, `train_daily_ic.csv` | byte-identical |
| `summary.json` | differs only at timing leaves: paths with a segment `wall_seconds`, `hash_seconds` or `stage_seconds` (`/roles/*/{wall_seconds, hash_seconds, stage_seconds/*}`, `/roles/*/candidates/*/{wall_seconds, stage_seconds/*}`); every other leaf identical, incl. cache counters and identity strings (both warm on the spec's cache `mega-candidate-cache-v8-lo3`, Debug root `dslvm1_clang18.1`: S1 keeps the Debug identity strings) |
| `train_candidates.jsonl` | same line count; each line differs only at `/stage_seconds/*`, `/wall_seconds` |

**fit** (`p9-b0-yf0-fit`; 3 files): `admission.csv` byte-identical; `admission.json` only `/inputs/script_sha256`
(D1 edited the fitter, `85b19345`); `composition_weights.json` only `/provenance/script_sha256` and
`/provenance/admission_sha256` (the new admission.json's SHA). Unlike the PM7-30 / D1 precedent on X-5,
`/provenance/std/registry_sha256` is expected **identical**: `atx-impl/strategies/alphas/registry.json` last changed at
`a5914373`, an ancestor of `816be40b` (X-5's fit predated it); composition modules (`*/module_sha256`) unchanged since
`816be40b`.

**w** (`p9-b0-yf0-w`; 12 files): `orientations.json`, `recipe.json`, `train_combined.{f64,json}`,
`train_combined_{finite.u8,ids.u64,member.u8,sessions.i64}`, `train_daily_ic.csv`, `train_planned_targets.csv`
byte-identical (10); `summary.json` only the timing leaves as for u (incl. `stage_seconds/{composition,
save_combined}`); `train_candidates.jsonl` only `/stage_seconds/*`, `/wall_seconds` per line.

**NAV Y-F0 and X-10** (27 files each; the union of the M1c stage-1 and stage-2 lists above):

| file | expected |
|---|---|
| `daily_L0.csv`, `events_L0.csv`, `recipe.json`, `capacity/recipe.json` | byte-identical |
| `daily_` / `events_` of L1-L4 (8), `capacity/daily_K`, `capacity/events_K` (10), `capacity_curve.csv`, `v7_transfer_coefficient.csv` | may differ (sqrt) |
| `summary.json` | only `/scenarios/1..4` (subtrees), `/warm_start/score_begin_gross_leverage/<L1..L4>`, `/v7/books/<L1..L4>` (subtrees), `/v7/extras` (changed), `/v7/files/*` (added), `/producer/*` (added: `build_type`, `definition`, `engine_git_sha`; C1-PROD) |
| `capacity/summary.json` | only `/scenarios/0..4` (subtrees), `/warm_start/score_begin_gross_leverage/<K>` (5), `/v7/books/<L1..L4>` (subtrees) |
| `v7_extras.json` | only `/files/v7_transfer_coefficient.csv`, `/files/capacity_curve.csv`, `/capacity/0..4` (subtrees), `/files/capacity/summary.json` (added); `/capacity_x1_equals_primary_bit_for_bit` identical |

No file on one side only.

**NAV Y-1** (28 files): the NAV list above, plus (Ruling B0-Y1, by code reading only):

| file | added to the list |
|---|---|
| `vol_target.csv` | may differ only as: header line identical; same row count and the same `(session, book)` key sequence; every row of book L0 byte-identical; rows of books L1-L4 may differ in any other column |
| `summary.json` | `/vol_target/books/<L1..L4>` (subtrees); `/vol_target/books/<L0>`, every other `/vol_target/*` leaf and `/rule` identical |
| `v7_extras.json` | `/vol_target/books/<L1..L4>` (subtrees), `/files/vol_target.csv`; the rest of `/vol_target` identical |
| `capacity/summary.json`, `recipe.json`, `capacity/recipe.json` | nothing added: their `/vol_target` block is parameters only (no books) and identical |

Code-path proof per added entry (post-C1 code at `b52a5de7`):
1. Sqrt kernel: `SqrtImpactCost::cost_fraction` -> `participation_power` (`atx-engine/src/book/replay_cost.cpp:26-28,
   92-102`, `std::sqrt` at delta .5) prices every fill of books L1-L4 and every K book; L0 is FlatBpsV1 (C1 report book
   table).
2. Cost -> NAV: `strategy_nav_replay.cpp:762` `c.model.cost(...)` -> `cost += priced.cost_dollars` (:786) -> `b.cash -=
   cost` (:792), `b.nav_post = b.nav_pre - cost` (:795).
3. NAV -> leverage state: the book's next DECIDE `s.current[i] = s.held[i] / b.nav_post` (`plan_decision`, :1037) ->
   `plan_weights` (:966-967) -> `BookLeverage::plan(... current ...)` (`strategy_risk_target.cpp:175-192`) ->
   `BookScaler::leverage` / `estimate` (gross, priced_share, sigma_hat / sigma_ref via `vol_target_update`, the in-force
   L_t and clip; :95-169) -> a `Record` (:118-133) only for a main book's scored decision (`scored = !b.capacity && d >=
   x.decision_begin`, nav_replay :1043). L_t then scales that book's own aim (`scaled_.target.aim_leverage`), so its
   later fills, costs and records follow; every field of an L1-L4 row is downstream of its book's sqrt-priced costs.
4. Records -> files: `merged(results, &NavReplayResult::leverage)` (`strategy_nav_v7.cpp:498-522, 885, 921`) ->
   `records_csv` (risk_target :228-242, one row per record, `book` = the record's own book) -> `vol_target.csv` and
   `files["vol_target.csv"]` (nav_v7 :435-443); `summary_json` (risk_target :277-312, one subtree per record.book) ->
   `/vol_target/books/<book>` in `v7_extras.json` (nav_v7 :441-443) and `summary.json` (`add_risk_target`, :316-326,
   called at :591 with the main pass's records).
5. Per-book isolation: each book owns its `BookLeverage` (`Book::leverage`, nav_replay :293, made at :1341), its names
   and cash; the shared inputs across books are the desired target, borrow tiers and the session liquidity (market data
   only), so no L1-L4 cost bit reaches L0's rows or block.
6. Capacity books keep no record (`scored` false); the capacity publication's `/vol_target` is parameters only
   (`s.pass == Capacity` -> `nullptr` at nav_v7 :591; `CapacityPublication` :928-936).
7. C1 left the record format and the blocks unchanged: `git diff 1239a5ff b52a5de7 -- strategy_risk_target.cpp` does not
   touch `records_csv`, `summary_json` or `parameters_json` (vol law: `strategy_vol_target.cpp`, unchanged); it moves the
   per-book state from the shared `Scaler` map into `BookScaler` (same law, same per-book state; working buffers per book).

**A1 manifest keys (Ruling M1a-ID):** not exercised: no stage of P9-B0 builds a fields manifest (u / w / NAV read the
pinned fields-v15 manifest `26fee5ce`); the M1a-ID keys stay ruled for the next fields rebuild.

Anything outside these lists, a file on one side only, or a changed row key in `vol_target.csv` is a STOP. The re-based
references, once they hold, are the six new dirs.

### M1d resume: Release IC adoption expectation (G-P3 IC half; written before the Release runs)

S1 report "How root verifies" 2-4 and fix round 1 step 4. Debug side = this tree's p9-1k exes; Release side = the
`equity-rel` exes of this resume's Release tag. X-5 family (library v8x3b, fields v13), so the marginal's candidates are
the u pass's. DIR = `build-equity/p9-m1d-rel-cache`, a hard-link copy of `p9-d1-x5-cand-cache-empty` (Debug-only
entries: D1's cold Debug w of X-5, 174 files); removed afterwards (Ruling DISK).

| step | run | expected |
|---|---|---|
| 1 | Debug marginal on DIR (argv of `p9-s1-x5-marginal-new-run`, `--candidate-cache DIR`, `--output`) | completes |
| 2 | Release marginal on DIR, same argv | refuses NotFound ("no candidate cache entry"), nonzero exit |
| 3 | Debug u on DIR (argv of `mega-v8-b0b-train-u-v8x3b-run1`, `--candidate-cache DIR`, `--output`) | completes (warm Debug) |
| 4 | Release u on DIR, same argv | completes (cold Release; writes `DIR/dslvm1_clang18.1_opt_md_ndebug_xs13.0.0/`) |
| 5 | Debug w on DIR (argv of `p9-d1-x5-w-run`, `--candidate-cache DIR`, `--output`) | completes (warm Debug) |
| 6 | Release w on DIR, same argv | completes (warm Release) |
| 7 | Release marginal on DIR, as step 2 | completes |

Identity: u (3 vs 4) and w (5 vs 6): every payload (`orientations.json`, `train_daily_ic.csv`, `recipe.json`,
`train_combined.*`, w's `train_planned_targets.csv`) byte-identical; `train_candidates.jsonl` equal outside
`/stage_seconds/*`, `/wall_seconds`; `summary.json` differs by design only at identity strings (suffix
`_opt_md_ndebug_xs13.0.0`), cache-root paths, cache counters (warm vs cold) and timing leaves (listed by path).
Marginal (1 vs 7): `marginal_ic.json` identical outside `/stage_seconds/*` and `/inputs/candidate_cache/build_vm_identity`.
Plus Release gtests: `atx-impl-strategy-ic-tests` S1 filter and whole binary; alpha oracle / conformance suites
(`atx-engine-w1-foundation-tests`). A payload difference here is not a P9-B0 stop: G-P3's IC half is then reported
unmet with the first differing file.
