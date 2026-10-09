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

### M1d resume: P9-B0 result (list committed first: `7d5b2ec9`)

All six runs completed (exit 0) through `run_bounded_research.py` (scratch `rerun.py`, one argv token changed:
`--output`; reference limits; receipts "clean in the code pathspec"). Compared by `b0_compare.py` (session scratchpad
`m1d/`): file SHA-256, JSON leaf paths classified against the list, CSV header names and differing-row counts only.

| run | exe | wall / peak / admission wait | files same / differ | outside the list | verdict |
|---|---|---|---|---|---|
| u `p9-b0-yf0-u` | ic `393183c0` | 20.2 s / 530 MiB / 557 s | 3 / 2 (`summary.json` 140 timing leaves; `train_candidates.jsonl` 134 timing leaves) | 0 | **inside** |
| fit `p9-b0-yf0-fit` | python fitter (HEAD) | 3.8 s / 60 MiB / 527 s | 1 / 2 (`admission.json` `/inputs/script_sha256`; `composition_weights.json` `/provenance/{script_sha256, admission_sha256}`) | 0 | **inside** |
| w `p9-b0-yf0-w` | ic `393183c0` | 133.8 s / 1,435 MiB / 0 s | 10 / 2 (`summary.json` 277 timing leaves; `train_candidates.jsonl` 268) | 0 | **inside** |
| NAV Y-F0 `p9-b0-yf0-nav` | targets `407c34ad` | 89.5 s / 590 MiB / 61 s | 16 / 11 (8 sqrt CSVs; `summary.json` `/scenarios/1..4`, `/v7/extras`, `/v7/files` +2, `/producer` +3; `capacity/summary.json` `/scenarios/1..4`; `v7_extras.json` `/files/capacity/summary.json` +1) | 0 | **inside** |
| NAV X-10 `p9-b0-x10-nav` | targets `407c34ad` | 79.3 s / 591 MiB / 153 s | 15 / 12 (9 sqrt CSVs; same JSON paths, `capacity/summary.json` `/scenarios/0..4`) | 0 | **inside** |
| NAV Y-1 `p9-b0-y1-nav` | targets `407c34ad` | 84.0 s / 602 MiB / 0 s | 17 / 11 (8 sqrt CSVs; same JSON paths, `capacity/summary.json` `/scenarios/0..3`) | 0 | **inside** |

- Every moved CSV column is a cost column (`impact_cost_dollars`, `unrationed_cost_dollars`, `trade_cost_dollars`,
  `trade_cost_return`), 1-8 rows of 1,006 per file; no events file, no L0 file, no recipe moved; `capacity_curve.csv`
  and `v7_transfer_coefficient.csv` identical in all three books; `capacity-x1` daily = the primary daily in all three.
- **Y-1 (B0-Y1):** `vol_target.csv` byte-identical (`100286a5`), and no `/vol_target` leaf moved in `summary.json` or
  `v7_extras.json`: none of the B0-Y1 additions was needed (allowed, not exercised).
- u / w cache counters and identity strings identical (warm on `mega-candidate-cache-v8-lo3`, Debug root).
- **P9-B0 holds: 0 differences outside the written list. Re-based references (pinned for the next paired tests):**
  u `build-equity/p9-b0-yf0-u` (orientations `d0d16f30` = the reference's), fit `p9-b0-yf0-fit` (`admission.json`
  `0dc97aff`, `composition_weights.json` `fdbcb918`), w `p9-b0-yf0-w` (combined json `e13fbc4d` = the reference's),
  NAV Y-F0 `p9-b0-yf0-nav` (`summary.json` `4f048210`, S2 daily `e7eb5720`, was `73b69bcc`), X-10 `p9-b0-x10-nav`
  (`summary.json` `42d104fc`), Y-1 `p9-b0-y1-nav` (`summary.json` `07e23261`, `vol_target.csv` `100286a5`).
- 0 trials; `build-equity/trials.jsonl` 133 lines, `27e40f9f` (mtime 11:32, unchanged).

### M1d resume: result (detail in `root-wave1-merge-report.md` "M1d resume")

**Status: STOP for a PM ruling** -- all blocks done, P9-B0 holds, but the failure set is outside the five named
known-reds by two new items. No code change; no expected hash edited; 0 trials (133 lines, `27e40f9f`).

| block | result |
|---|---|
| 1 suites (rest) | `atx-impl-tests` whole (Debug): 1,124 passed / 5 skipped / **1 failed** `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest` (new red 1: pre-existing since `d060cd81` 2026-09-26, not wave 1). pytest: `scripts/tests` seed 0 and seed 1 each 373p / 4s / 1f (= M1a-RED, same assertion), `atx-engine/tools` 399p, `atx-impl/tools` 640p / 3s, `atx-impl/strategies` 14p |
| 2 canary (G-P8) | p9-1l Debug record (`506ea123`), p9-1m Debug check 15 passed; p9-1n equity-rel build (25 TUs, 0 warnings) + Release record (`5e1ebbbc`), p9-1o Release check 15 passed; Release digests = Debug digests (all five) |
| 3 Release IC (G-P3 IC half) | u / w / marginal Release vs Debug: payloads bit-identical; Release marginal refuses NotFound before a Release u pass and succeeds after; Release gtests S1 filter 31/31, w1-foundation 83/83, **ic-tests whole 186/193** (new red 2: 7 `StrategyIcRunner.*`, Release only, MAX_PATH 262 > 260 from S1's Release cache root in the test scratch path) |
| 4 P9-B0 | list `7d5b2ec9` before the runs; 6 runs, **0 differences outside the list**; re-based references `build-equity/p9-b0-*` |
| 5 scoreboard | `scoreboard --timings` exit 0: lineage X-F0 -> Y-F0 (y-s), 4/4 checks, ledger chain verified (133 lines, head `5a3ef9d9`, N 62) |

New reds needing a ruling (not in M1a-RED / M1c-RED): (1) `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest`
(Debug `atx-impl-tests`; manifest line `config_json=` added by `d060cd81` vs a test asserting the manifest ignores
config_json); (2) seven Release-only `StrategyIcRunner.*` in `atx-impl-strategy-ic-tests` (`candidate cache partial
output`, path 262 chars: test scratch root + `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0`). Real-data Release paths
measured under 260 (~16 chars margin on the spec cache).

#### `scoreboard --timings` (v8 waves, wall seconds by phase; runs)

| wave | card | fit | monitor | nav | ref | summ | u | w | total |
|---|---|---|---|---|---|---|---|---|---|
| y-2 | 18.2 (1) | 0.8 (1) | 1.6 (1) | 90.9 (2) | - | 34.4 (1) | 12.7 (1) | 42.2 (1) | 200.7 |
| y-3 | 24.5 (1) | 1.9 (1) | 1.6 (1) | 105.5 (2) | - | 37.2 (1) | 12.7 (1) | 52.8 (1) | 236.2 |
| y-5 | 18.8 (1) | 1.0 (1) | 1.6 (1) | 161.6 (2) | - | 37.7 (1) | 12.7 (1) | 62.2 (1) | 295.5 |
| y-s | 24.5 (1) | 1.9 (1) | 1.6 (1) | 115.3 (2) | 61.9 (1) | 40.0 (1) | 12.7 (1) | 52.8 (1) | 310.7 |

No P9 wave exists yet, so these are v8's numbers re-read through the P9 scoreboard (the baseline the S1 / S2 / C2 / E1
speed claims are measured against). M1d's own walls for comparison (Debug unless noted; busy host): P9-B0 u 20.2 s,
w 133.8 s, NAV 79-90 s; Release u cold 160.5 s, Release w warm 52.6 s, Release marginal 104.8 s vs Debug 416.5 s.

#### Platform gates (G-P) after M1d

| gate | state | evidence |
|---|---|---|
| G-P4 CTest | **ticked** | `ctest -N`: `atx_research` 106 (fields 46 + admission 16 + mine 44), `atx_equity_strategy` 577 (strategy 46 + ic 193 + target 338); all run (failures: M1a-RED's two gtests only) |
| G-P6 no versioned script | **ticked** | class-C generators deleted at M1a after slice-2 `--check`; `test_no_versioned_scripts.py` green under seeds 0 and 1 |
| G-P8 canary | **ticked** | goldens recorded Debug (p9-1l) and Release (p9-1n), checked green on p9-1m and p9-1o; Release = Debug digests |
| G-P3 Release adopted, IC half | shown, **not ticked** | u / w / marginal payload identity and the S1 refusal / adoption behaviour hold; held back by new red 2 (Release ic-tests) until ruled |
| G-P3 NAV half | shown on X-5 at M1c | wave-3 gate (plan §4.3) |
| G-P5 | guard only | `test_no_python_mirror.py` green (37 functions / 14 rows left for wave 2) |
| G-P7 | first half only | eval_tie fixture (M1a); wave 2 |
| G-P1, G-P2, G-P9 | not checkable at wave 1 | - |

Plan §4.3 phase-1 exit: wave-1 merged yes; G-P4 yes; G-P8 yes; G-P3 IC half pending ruling; P9-B0 pinned yes.

#### Disk (Rulings DISK, DISK-2), after every gate and P9-B0 had finished

Checked first (0 tracked, git-ignored, no reparse point inside, rename probe) and found unnamed by any P9 spec, test,
plan or sprint brief (grep of `scripts/specs`, `scripts/tests`, `docs/plans`, `atx-impl/strategies` and the
`.superpowers` dirs of pools 2, 17, 19, 20: only receipts, logs and `disk-audit.md` name them). Deleted, all in pool-2
`build-equity/`:

| path | what | files |
|---|---|---|
| `p9-m1d-rel-cache` | my Release-adoption DIR (hard links of D1's cache + the Release root) | 348 |
| `p9-d1-x5-cand-cache-empty` | DISK-2 (audit #5): D1's cold X-5 cache | 174 |
| `v8-i16d-cand-cache-empty` | DISK-2 (audit #2): v8 R0-3 cold X-5 cache | 174 |
| `recent-fast-validation-2023-2024-v1-fields-v{1,2,3}/*.f64` | DISK-2 (audit #4): payloads only, deleted unread; each `manifest.json` kept | 11 + 8 + 8 (984,602,304 B) |

Free on C: 36,422,791,168 B before, 49,014,763,520 B after. Not touched: spec-pinned caches
(`mega-candidate-cache-v8-lo3` / `-lo1`), the lo3 hard-linked stores, risk dirs, reference outputs, `trials.jsonl`.
Also removed: my pytest basetemp dirs (scratchpad) and one `.pyc` my canary run wrote. Kept: the P9-B0 references and
the adoption run dirs (evidence). Next free build tag **p9-1p**.

### M1d fix (root fix agent, 2026-10-03; detail in `root-wave1-merge-report.md` "M1d fix")

**Status: STOP for a PM ruling on red 1. Red 2 is fixed.** 0 trials (133 lines, `27e40f9f`). No expected hash edited.

| item | result |
|---|---|
| red 2: 7 Release `StrategyIcRunner.*` (MAX_PATH) | **fixed, test-only** `27e2e5b4`: the scratch name in `strategy_ic_runner_test.cpp` is now `icr-<6 digits>-<n>` (was `atx-strategy-ic-runner-<ns stamp>-<n>`); Release cache root, cache identity and OS long-path settings unchanged. p9-1p (equity-dev): ic-tests **193 / 193**. p9-1q (equity-rel): the 7 anchored 7 / 7, whole **193 / 193**, 0 skipped |
| red 1: `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest` | **not fixed; ruling needed.** The stage digest (`fnv1a64` over the admitted DSL) never contained config_json; its assertion (`:370`) passes. Only `:373` fails: `_manifest.txt` byte-equality, broken by the `config_json=` line itself (`field_cardinality_max` 12 vs 999). Keeping the line and leaving the expectation unedited cannot both hold. Pin grep: no P9 pin, reference, spec or golden stores a discover digest or manifest hash. Options A (test compares manifests minus that one line; recommended) / B (move the line to a `_config.json` sidecar) / C (drop it) in the report |
| whole `atx-impl-tests` (Debug, p9-1p) | 1,130 tests, 906 s: 1,123 passed / 6 skipped / **1 failed** (red 1 only); the extra skip `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` needs cwd = repo root and passed when re-run there (so 1,124 / 5 / 1 as at M1d); all 51 `StrategyIcRunner.*` pass |

Failure set: M1a-RED (pytest `test_fields_are_the_rule_applied_to_the_registry`; gtests
`ResearchFieldsWriter.QuantilesPartitionLikeNumpy`, `ResearchFieldsVolumeMean.SumOrderIsNumpys`), M1c-RED (Release
`BookNormalScore.TiesShareTheMeanRankAndMirrorsAreOpposite`, `.FixtureTellsWrongRulesApart`), plus **red 1**. Wave-1
gate "failure set within the five named known-reds": **not met** (red 1 only).

#### Platform gates (G-P) after M1d fix

| gate | state | evidence |
|---|---|---|
| G-P3 Release adopted, IC half | **ticked** (Ruling M1d-RED-2: "ticked when Release is 193/193") | M1d: u / w / marginal Release payloads = Debug, S1 refusal and adoption behaviour; now Release `atx-impl-strategy-ic-tests` 193 / 193 (p9-1q). Red 1 is discover provenance, outside G-P3; the PM can withhold the tick until the wave-1 gate is met |
| G-P3 NAV half | shown on X-5 at M1c | wave-3 gate |
| G-P4, G-P6, G-P8 | ticked (M1d) | unchanged |

Plan §4.3 phase-1 exit: wave-1 merged yes; G-P4 yes; G-P8 yes; G-P3 IC half yes; P9-B0 pinned yes; suite gate
pending red 1.

**Risk for SQL3 (Ruling M1d-RED-2).** Real-data Release cache paths keep only ~16 characters below MAX_PATH. The spec
cache `mega-candidate-cache-v8-lo3` plus `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0\` reaches ~244 for the longest
entry; the M1d adoption DIR reached 233. A deeper cache dir, a longer candidate id or a longer identity token crosses
260 and fails loudly (`IoError: candidate cache partial output`). SQL3 should keep the per-entry depth bounded or
record the budget.

**Volatile list for wave-2 P9-B0 (Ruling M1d-NOTE-3), amending the u / w rows of the "P9-B0 substitution list"
above.** In `train_candidates.jsonl`, each line may differ only at `/stage_seconds/*` (incl. `cache_load`,
`cache_write`), `/wall_seconds`, **and `/signal_cache`, `/ic_result_cache`** (`"hit"` / `"miss"`, present only with
`--candidate-cache`; `strategy_ic_runner.cpp:502-503`). These strings are timing-class (warm vs cold), outside identity.
`summary.json` cache counters stay as listed per comparison (identical when both sides are warm on the same cache).

Disk: only my scratchpad logs deleted; the build trees are kept for wave-2 merges. Next free build tag **p9-1r**.

#### M1d fix, red 1 resolved (Ruling M1d-RED-1b, option B), `aa5d4858`

- Reader grep first: no C++, Python, script or test parses the `config_json=` manifest line (only its two writers
  matched). `_manifest.txt` readers are gtests reading other keys; alphas-dir scans filter on `.dsl`. So nothing was
  re-pointed and no P9 pin or reference depends on it.
- Change: `stage_discover.cpp` writes the config JSON to `<alpha_out>/_config.json` (`build_config_json` + `'\n'`)
  under the old line's condition (non-legacy CPCV / IC-screen / PBO), in both the gated and the window paths. A stale
  sidecar is removed when none is due. `_manifest.txt` no longer carries `config_json=`, and the test is unedited.
- New gtest: `AtxImplProvenance.ConfigSidecarHoldsTheRunConfigAndStaysOutOfTheManifest` (sidecar = run-DB config_json
  + `'\n'`; no line in the manifest; a stale sidecar is removed by a legacy run).
- p9-1r (equity-dev, `atx-impl-tests`, 2 TUs, 0 warnings, exe `035ea247`): anchored provenance suites 13 / 13. Whole
  `atx-impl-tests` from the pool-2 root: **1,131 tests, 1,126 passed, 5 skipped, 0 failed** (869 s).
- Failure set = the five named known-reds: M1a-RED x3 (pytest `test_fields_are_the_rule_applied_to_the_registry`,
  `ResearchFieldsWriter.QuantilesPartitionLikeNumpy`, `ResearchFieldsVolumeMean.SumOrderIsNumpys`) and M1c-RED x2
  (Release `BookNormalScore.*`). Reds 1 and 2 are fixed.

**wave-1 gate met (known-reds only).** Plan §4.3 phase-1 exit: wave-1 merged yes; G-P4 yes; G-P8 yes; G-P3 IC half
yes; P9-B0 pinned yes; suite gate yes (known-reds only). Next free build tag **p9-1s**. 0 trials (133 lines,
`27e40f9f`).

## P9 wave-2 merges

### M2a: SQL1, SQL2, A3, 2026-10-03

Base `632060d1` (wave-1 gate met). Detail: `root-wave2-merge-report.md` section M2a.

| lane | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity / root items |
|---|---|---|---|---|---|---|---|---|
| SQL1 | `1a5b051d` | `d31c5367` + N1 fix `8d4cc52c` | `atx-engine/tests/CMakeLists.txt` tail (B1 + SQL1), both kept | p9-1s FAILED (root tree lacked test group `store`; reconfigured +store); **p9-1t** `atx-core-tests, atx-engine-research-store, atx-engine-research-store-tests, atx-engine-store-tests, atx-engine-library-tests, atx-impl-tests`, 331.9 s, 82 TUs | 0 | Db* 44p; ResearchStore* 27p; engine-store 40p; library 66p/2 disabled; impl StoreDiscover+Provenance+ProvenanceDigest 23p | SQL1 4 files x2 seeds 31p each; record-store consumers 166p | flag-absent: `20443022..1a5b051d -- atx-impl scripts` empty; PCH-off satisfied in equity-dev (no PCH flags on the SQL1 TUs) |
| SQL2 | `cfe2de87` | `1300fd6a` + SQL2-CLS `1892a183` (`atx.nav-rules/v1` -> nav-output) | `atx-engine/tests/CMakeLists.txt` tail (HEAD empty side), SQL2 block taken | **p9-1u** `atx-engine-research-catalog, atx-engine-research-catalog-tests, atx-research-store` + SQL1's three, 91.4 s, 21 TUs | 0 | catalog ResearchCatalog+SealGuard+StoreCli 27p; ResearchStore* 27p; Db* 44p | 5 files x2 seeds 43p/129 sub each, FixtureChain ran | fixture chain `d32f7655...4e69` (28/25/0/4), identity 12 checked 0 mismatches, rel = abs root; flag-absent diff empty |
| A3 | `a217fd7e` then flip `50cb1571` | `ff1637ac` + S-R1 slip `08b0695d` + flip `1c90011a` | both CMake tails (SQL2 + A3), both kept | p9-1v first launch refused by memory gate (no receipt); **p9-1v** `atx-engine-research-fields, atx-engine-research-fields-tests, atx-research-fields`, 79.9 s, 20 TUs | 0 | fields-tests 66/66 (M1a-RED pair green); A3 filter 19p | pre-flip real exe 8/11/3/2/35/4 p; post-flip (M2b) registry `ok 92` + round-trip (9 engine rows), real exe `test_field_registry` 35p, `test_vendor_engine_path` 9p, `test_research_fields_engine_path` 11p | TRAIN six fields: py = engine = v15 6/6; manifests differ only in 6 producer blocks; rows_sealed_value_decoded 7,592,840 (0/262 groups pruned); peaks 542 / 950 MiB; DISK-2 pool-12: object tree already gone, keep-list intact (0 B freed) |

### M2b: HYG-1, A3 post-flip, AL-SIG, 2026-10-04

Base `c7ef4e3d`. Detail: `root-wave2-merge-report.md` section M2b; logs `m2b-logs/`. HYG-1: 609 staged
`wq-explore/.mypy_cache` paths unstaged; both cache dirs deleted; the committed `tools/.mypy_cache` (129 files, from
`749f9ac2`) removed in `56060f84`.

| lane | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity / root items |
|---|---|---|---|---|---|---|---|---|
| AL-SIG | `ebb081ab` | `b1406e14` + SQL2-CLS `6673f8d1` (`atx.p9-blocked-registration/v1` -> new class `blocked-registration`; `atx.alpha-panel.indexes/v1` -> legacy_allow, the guard's actual red) | 7, A2 shared tail vs A3 (both CMake tails, `build_spec.hpp`, `registry.hpp`, `registry.cpp` kind table 9+2=11, `research_fields_cli.cpp` known keys, `research_fields_registry_test.cpp` KindLookup); all kept both, A3 first; `field_registry.json` auto-merged, `ok 94` + round-trip | **p9-1w** `atx-engine-research-fields-tests, atx-research-fields`, 124.0 s, 24 TUs | 0 | AlSig+Registry 33p; whole target 92/92 (M1a-RED pair OK); supplementary ic `NonPointInTimeFieldsAndUnflaggedManifestsAreRefused` 1p (p9-1p exe) | tools 39p; scripts 16p; engine-path 11p + vendor-engine 9p (real exe); candidates validate 1 valid; store suite 43p/131 sub x2 seeds, FixtureChain ran; theme pins 31p | ALSIG-9 admission refusal read (`strategy_ic_admission.cpp:297-315, 398-406`); fixture chain `d32f7655...4e69` (28/25/0/4), identity 12/0, rel = abs; M-2 seam holds (13 themes, no new theme); DISK-2 pool-13: already clean (0 B); ledger 133 / `27e40f9f` |

### M2c: OD-A3-6F, S2, 2026-10-04 (BLOCKED on one Release red)

Base `4497784b`. Detail: `root-wave2-merge-report.md` section M2c; logs `m2c-logs/`. OD-A3-6F: the five
`build-equity/p9-a3-train-*` dirs deleted by name after a reference grep found no reader (about 762 MiB).

| lane | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity / root items |
|---|---|---|---|---|---|---|---|---|
| S2 | `2ed2fc18` (re-review `8314fb59`) | `f6444417` (no SQL2-CLS commit: no new literal) | none (IC runner / rules / admission / detail / test tails auto-merged) | **p9-1x** equity-dev and **p9-1y** equity-rel, each `atx-engine-alpha-tests, atx-engine-factory-tests, atx-impl-strategy-ic-tests, atx-impl-strategy-mine-tests, atx-equity-strategy-ic, atx-equity-strategy-targets` (1,142.9 s / 203 TUs; 1,048.4 s / 216 TUs); **p9-1z** equity-rel `atx-shm-worker` (slip: Release test prerequisite) | 0 / 0 / 0 | Debug: S2 filter 28p, ic 204/204, mine 44/44 (golden 1 + 4 workers), alpha 782/782, factory 396/396. Release: S2 filter 28p, ic 204/204, mine 44/44 (golden 1 + 4), factory 396/396 after p9-1z (first 389/7: missing `atx-shm-worker`), alpha 779p / 1 skip (CRT debug heap) / **1 FAILED `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`** (Debug-pinned golden; Release oracle misses too; attributed pre-existing by code reading; ruling needed) | plugins 8p; canary Debug 15p (p9-1x canary: digests equal, 15 s first-run wall missed on a busy host; re-check 15p in 12.29 s), Release 15p; store suite 43p/131 sub x2 seeds (guard green) | digests `dsl_vm` `5bcd55c7` (35), `ic_result` `dd3c6011` (11) on the merged tree = lane = pin; `exe_capabilities` append-only 5 -> 8 sorted; fixture chain `d32f7655...4e69` (28/25/0/4), identity 12/0, rel = abs; X-5 u / w vs P9-B0: all payloads byte-identical, summary only timing + `/roles/0/ic_cache_bytes` (+56 B = sizeof growth of the S2-1 fields), 67/67 signal + IC hits; cold Release u pre- vs post-S2: payloads + 67/67 signal payloads byte-identical; path margin Release + audit-exact: lo3 251 (-1 vs 250), parent `lib-v8ysb-gm` 250 (0); S2-6 no imputed entry; P13 live manifests v13 / v15 seal `2024-01-01`; DISK-2 pool-18: already clean (0 B); ledger 133 / `27e40f9f` |

#### S2 speed claims against the wave-1 table (M1d walls; same argv where named)

| measurement | wave-1 (M1d) | M2c before S2 | M2c after S2 | note |
|---|---|---|---|---|
| Release cold u, 4 workers, fresh cache | 160.5 s / 1,693 MiB (X-5 family v8x3b argv) | **163.7 s** / 1,617 MiB (Y-F0 argv, exe `ea370cec`) | **93.7 s** / 1,616 MiB (exe `bf0ad802`) | the only like-for-like S2 measurement: same argv and host, about 40 min apart; -42.8 % wall, peak unchanged (E-24 claim holds: banded cold u faster, not slower) |
| Debug warm u (Y-F0, lo3 cache) | 20.2 s / 530 MiB (P9-B0) | - | 13.3 s / 530 MiB | 67/67 hits on both sides: the VM does not run, so the wall is host noise, not an S2 effect |
| Debug warm w (Y-F0, lo3 cache) | 133.8 s / 1,435 MiB (P9-B0) | - | 85.8 s / 1,436 MiB | same: signals loaded from cache; composition only |
| tiny_world canary, Debug | 15 passed (M1d p9-1m) | - | 12.29 s for 15 (re-check); 23.2 s first run under p9-1x on a 100 % CPU host | host load, not code |
| tiny_world canary, Release | 15 passed (M1d p9-1o) | - | 10.93 s for 15 (p9-1y) | - |

Next free build tag **p9-2a**. Known-red after M2c: pytest `test_fields_are_the_rule_applied_to_the_registry` (E2),
Release `BookNormalScore.*` x2 (C2), Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (new; ruling
pending).

### M2d: D2, COV, 2026-10-04

Base `f96f9399`. Detail: `root-wave2-merge-report.md` section M2d; logs `m2d-logs/`. Housekeeping docs commit
`64069a3c` (PM's untracked docs).

| lane | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity / root items |
|---|---|---|---|---|---|---|---|---|
| D2 | `49af0a52` (re-review `c55063ff`) | `61378337` + D2-9 slip `e129a7fa` (3 long lines wrapped, whitespace only) + SQL2-CLS `c0446b19` (`atx.composition-fit-spec/v1`, `atx.composition-fit/v1` -> fit-output) | `atx-engine/CMakeLists.txt` and `atx-engine/tests/CMakeLists.txt` tails (SQL1 / SQL2 / A3 / AL-SIG vs D2), both kept; `exe_capabilities` untouched (8, sorted) | **p9-2a** equity-dev `-Canary`: `atx-engine-research-composition-tests, atx-research-composition, atx-research-admission, atx-equity-strategy-ic, atx-impl-strategy-ic-tests, atx-equity-strategy-targets, atx-impl-strategy-target-tests, atx-engine-combine-tests, atx-impl-tests` (430.9 s / 135 TUs) | 0 | ResearchComposition 10p; PairCacheSourcesPinned 1p; strategy-ic whole 206p; strategy-target whole 338p; combine whole 243p; atx-impl-tests whole 1135p / 5 skip (real-data gated) / 0 f; canary Debug 15p | `atx-impl/tools` 13 lane files with real exes 264p / 39 sub / 0 skip (S-7 incl.); `test_no_python_mirror.py` 7p; store suite 43p/131 sub x2 seeds | pair pin `df589a88` (3 files) recomputed = pin; `dsl_vm` / `ic_result` unchanged; fixture chain `d32f7655...4e69`, identity 12/0, rel = abs; refit check: no P9 parent uses a resid parent (only the v8 template r11.json names `--theme-resid`); flag-absent pre-D2 vs merged fitter on the tiny world (canary argv) and on aim_world (9 cases): only CM-5 / D2-2 key renames + `provenance.admission_sha256` (+ `resid.parent_weights_sha256` in a resid child); draft P9-B0 list in the report; report S-1 sentence corrected; item (iv) superseded; DISK-2 pool-20 already clean; ledger 133 / `27e40f9f` |
| COV | `c8ff4f1c` (re-review 2 `75428364`; base `ad406718`) | `694b4b95` (no SQL2-CLS commit: no new literal, guard green) | none (named CMake appends auto-merged, both kept) | **p9-2b** equity-dev `atx-engine, atx-engine-risk-tests, atx-impl-strategy-target-tests, atx-equity-strategy-risk, atx-equity-strategy-targets` (269.4 s / 82 TUs); **p9-2c** equity-rel `atx-engine-bench` (bench ON for this build only, then OFF; 162.9 s / 57 TUs); bench 5 reps: write 561-691 MiB/s, open-lazy ~0.21 ms, open-full 115-338 ms (default workers), first-touch 0.40-1.04 ms, view < 0.1 us, all 4.12 targets met except first-touch N 5627 f64 (1.04 ms vs 1 ms); open-full faster than the legacy emulation everywhere | 0 | RiskCovContainer 21p; Risk* minus the oracle 431p + 1 nightly skip; Cov* 29p; unchanged filter 104p | classes guard 5p / 92 sub | S9: lo3 role `returns_applied false` (0 / 990 events true); flag-absent store rebuild vs `v8-risk-lo3-v10`: 10 / 10 payloads byte-identical, manifest only `producer.{engine_git_sha,executable_sha256}` (54.3 s, 612 MiB); `--emit-container`: legacy files identical, manifest + `container` + `files["model.atxcov"]` only, **`cov-diff --legacy` exit 0** (1405 sessions); fixture chain `d32f7655...4e69`; S12 report slip fixed; D-COV / COV-7 / steps 4-5 left for the wave-2 gate; DISK-2 pool-24 already clean; ledger 133 / `27e40f9f` |

Next free build tag **p9-2d**. Known-red after M2d (unchanged): pytest `test_fields_are_the_rule_applied_to_the_registry`
(E2), Release `BookNormalScore.*` x2 (C2), Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`
(M2c-RED, lane S2R).

### M2e: C2, AL-COMB, 2026-10-04

Base `6960568f` (code `694b4b95`). Detail: `root-wave2-merge-report.md` section M2e; logs `m2e-logs/`. Housekeeping
docs commit `20bc5f5f` (status-5, next-goal-prompt, progress.md, AL-DATA edits, engine-impl status).

| lane | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity / root items |
|---|---|---|---|---|---|---|---|---|
| C2 | `4d77d3db` (re-review 2 `1ea664f2`; base `ad406718`) | `a25453be` (no SQL2-CLS commit: no new literal, guard green) | `atx-impl/CMakeLists.txt` tail (D2's composition link vs C2's calibrate source), both kept; COV / C2 tails of `atx-impl/tests/CMakeLists.txt` and the E2-file lines auto-merged | **p9-2d** equity-dev and **p9-2e** equity-rel, each `atx-impl-strategy-target-tests, atx-equity-strategy-targets, atx-engine-eval-tests, atx-engine-book-tests, atx-impl-tests` (331.8 s / 114 TUs; 775.4 s / 253 TUs) | 0 / 0 | Debug and Release each: `CalibrateGross.*:NavBookRule.*:BookNormalScore.*` 19p; target-tests whole 377p; impl-tests `CalibrateGross.*` 7p; eval `*StatsExt*:*Deflated*:*MinTrl*` 10p; book `BookNormalScore.*:*DecayMonitor*` 13p. **M1c-RED cleared** (both Release `BookNormalScore` reds green) | `test_research_wave.py` 20p; eight wave suites 77p; classes guard 5p / 92 sub | **X-5 gm-pair identity HOLDS** (list `4d0583cf` first): -gm run vs `p9-c1-s2-x5-nav` 26/27 identical + `producer.engine_git_sha`; engine `nav --calibrate-gross` vs the Python -gm 26/27 identical + added `/calibration`; L' `1.1720` exact, gross bit-equal, `construction_reused` true; 92.3 s two-process vs 63.7 s one-process. (ii) deletion of `wave_rules.py:110-119` NOT done: it is the whole Python `pm6-6` mode (fixture default, direct tests, G-P5 row) in files E2 is rewriting: left to C3 with C2-9 and S14. (iv) no P9 reference under `rel` / `rel-avx2` (receipt scan). (v) fixture chain `d32f7655...4e69`, identity 12/0. S13 at E2's merge. DISK-2 pool-15 already clean; ledger 133 / `27e40f9f` |
| AL-COMB | `efb7ab2e` (re-review 1 `098e6ccd`; base `1239a5ff` + D2 `49af0a52` merged in) | `9d518317` + eol `2ee62800` (E2's R0-7-EOL `.gitattributes` lines verbatim; 18 CRLF spec copies refreshed from blobs) + SQL2-CLS `e1c16051` (`rule-registration`, `diagnostic-spec` class rows; slip: `spec_kind` `diagnostic` refused by the `spec_doc.kind` CHECK on a scratch catalog, fixed to `other` before the commit) | `atx-impl/tests/CMakeLists.txt` tail (C2 vs AL-COMB), both kept; `exe_capabilities` unchanged (8, sorted, size = count); runner / detail / rules / fixture auto-merged | **p9-2f** equity-dev `atx-engine-combine-tests, atx-impl-strategy-ic-tests, atx-equity-strategy-ic, atx-impl-tests` (110.0 s / 36 TUs, ccache) | 0 | CharCovHedge 5p; combine whole 248p; IC anchored 111p; IC whole 226p; impl-tests `ThemeHedgeRule.*:MomVolman.*:CompositionRules.*` 23p; `--list-rules --json` = fixture | composition set 133p / 39 sub / 0 skip (real exes); fitter 142p; guards 11p; store suite 43p / 133 sub x2 seeds | registration pins = disk = blob (`7c6778bb`, `7e9b26c1`); W1-MIN M-2 seam holds (registry 13 = built-in prefix); flag-absent X-5 w pre (`ba1232ed`) vs post (`c5015605`): 10/12 identical, 2 timing-only (list `789d7e91` first); flag-absent fitter fixture 9 cases: only the listed fingerprint / SHA values, OUTPUT `d0153b` -> `d28a30` (list `60bd542b` first); fixture chain `d32f7655...4e69`; MR-4 at E2's merge; Q5 at the wave-2 gate; DISK-2 pool-19 already clean; ledger 133 / `27e40f9f` |

Next free build tag **p9-2g**. Known-red after M2e: pytest `test_fields_are_the_rule_applied_to_the_registry` (E2);
Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (M2c-RED-2 / OD5-2: a visible Release skip once
S2R's split merges). Release `BookNormalScore.*` x2 cleared by C2.

### M2f: S2R, B2 identity part, 2026-10-04

Base `3e283275` (code `e1c16051`). Detail: `root-wave2-merge-report.md` section M2f; logs `m2f-logs/`.

| lane | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity / root items |
|---|---|---|---|---|---|---|---|---|
| S2R | `048b01c6` (light review APPROVE `d92a20ee`; contains root `f6444417`) | `0f7a84b1` (no SQL2-CLS: test-only lane) | none | **p9-2g** equity-dev and **p9-2h** equity-rel, each `atx-engine-alpha-tests, atx-impl-strategy-ic-tests` (13.3 s / 1 TU; 123.2 s / 31 TUs) | 0 / 0 | Debug: alpha 783p / 0 skip / 0 fail, lit-golden 3p (prints = pins), S2 filter 28p, IC whole 226p; Release: alpha **780p / 2 skip / 0 fail** (`EveryPreW2OpcodeDigestIsPinned` with the M2c-RED-2 CRT `pow` message, `WarmDispatchLoop_BoundedByOutputsAndSizeIndependent`), prints `0xeeae40c4...` / `0x24c97b5b...` / `0xeeae40c4...`, S2 filter 28p, IC whole 226p; `EveryPreW2OpcodeVmMatchesOracle` green in both | none (C++ test lane) | no pin moved (hex constants pre / post: only the canonical NaN added); source-digest pins hold; UCRT `2c08e77f` unchanged; S2R-1 scan on the merged tree: 5 `stddev` book candidates, 0 P9 strings; slip S-3 (`hf_crowd` has a DSL string) fixed in the report; Release lit-golden known-red is now a named skip |
| B2 (identity part) | `1b25b9a4` (re-review 1 `4bed5c43`: identity part sound; post-identity `598d2199` / `8fb17ad6` not merged, B2-12) | `c2416d62` + SQL2-CLS `ba5e6e5d` (`ledger-head` and `eval-output` rows; two fixture literals into `test-golden`) + canary `42f42a07` (`$canaryExes` += `atx-research-eval`) | the three CMake tails (`atx-engine`, `atx-engine/tests`, `atx-impl`), both kept; `wave_stage_record.py` auto-merged (C2's R4 lines + B2's ledger-head writer) | **p9-2i** equity-dev and **p9-2j** equity-rel, each `atx-engine-research-ledger-tests, atx-engine-research-eval-tests, atx-research-eval, atx-impl-tests, atx-equity-strategy-ic` (122.4 s / 38 TUs; 128.1 s / 46 TUs); verb Release `a57a2090`, Debug `bd8b5e08` | 0 / 0 | Debug and Release each: eval 24p, ledger 16p, impl-tests `TrialLedger*` 24p | identity point, Debug and Release verb each: tools 109p / 1 skip + scripts 17p; the skipped root-only `test_legacy_n37_numbers_reproduced` 1p on real v7.1 cells; store suite 43p / 135 sub x2 seeds | **rho first** (`ea52989a`, before the run): X-5 vs X-3 0.92027 (1 - rho 0.0797), Y-F0 vs X-5 0.95560 (0.0444), both non-degenerate; **TRAIN identity HOLDS** (list `ea52989a` first): X5 and YF0, Python vs Release verb vs Debug verb, stdout byte-equal (31 lines), stderr equal, JSON 0 problems (max rel 5.0e-14), Debug = Release verb to the bit, **0 B2-7 boundary lines**; ledger `prev_sha256: null` lines 0; fixture chain `d32f7655...4e69`; DISK-2 pool-18 already clean; ledger 133 / `27e40f9f` |

Next free build tag **p9-2k**. Known-red after M2f: pytest `test_fields_are_the_rule_applied_to_the_registry` (E2).
Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` is a named Release skip (0 failed; M2c-RED-2 /
OD5-2).

### M2g: wave-2 gate diagnostics, part 1 (GATE-EARLY; no merge), 2026-10-04

Base `ab189471` (code `42f42a07`). Detail and every pre-run list: `root-wave2-gate-report.md`; logs `m2g-logs/`. Zero
trials: `build-equity/trials.jsonl` 133 lines `27e40f9f` at start and end.

| item | commits | build tag | runs (bounded, 2,048 MiB) | result |
|---|---|---|---|---|
| 1 AL-DATA | spec `30b9c9d2` (5.1 + 5.2, one commit); nits D-n1 / D-n2 in `3fd0bf98` | - | - | `al-data/*.md` already committed as they stood (`ab189471`); six "before" lines byte-identical to HEAD; `test_p9_al_sig_registrations.py` 11p before and after; 5.3 not applied (P10) |
| 2 ALD-DIAG | list `3fd0bf98`, result `1b8c4df2` | - | `p9-m2g-ald-diag-run` 0.5 s | names / delisted <= 2023-12-01 / unclassified: 2020 2,420 / 83 / 0; 2021 2,540 / 103 / 0; 2022 2,325 / 106 / 0; 2023 2,186 / 95 / 2; almost all M&A (performance 5 in four years) |
| 3 B2-13 rho | list `3fd0bf98`, result `1b8c4df2` | - | `p9-m2g-b2-13-rho-run` 1.1 s (Release verb `a57a2090`) | X-10 vs Y-F0 rho 0.9994863 (1 - rho 5.14e-4); Y-1 vs X-10 rho 0.9947114 (5.29e-3); pinned = ledgered; both paired-test defined |
| 4 COV | list `3fd0bf98`, result `1b8c4df2` | **p9-2k** equity-dev `atx-equity-strategy-risk` (1 TU, 9 s) `94b5a5e7`; **p9-2l** equity-rel `atx-equity-strategy-risk, atx-equity-strategy-targets` (2 TUs, 9 s; first attempt refused at the memory gate, no receipt) `a31a0f96`, `53ca5dd4`; 0 warnings | COV-7 on the lane fixture (synthetic, direct) | (a) S9 holds; **(b) COV-7 IDENTITY MISS**: Debug vs Release container blocks differ for both recipes (cov-diff exit 1 at block 2; factor covariance median 17-24 ULP, max rel 1.5e-9; deterministic per build); **(c) not run**: no D-COV, no COV step 4, no step 5, no atx-cov-v1 store; PM ruling needed |
| 5 Q5 | list `3fd0bf98` + amendment `1b8c4df2`, result `29adf2fa` | - | holdings re-run 27.4 s, book-csv 4.0 s, risk 20.7 s (Debug cross-check 49.2 s), G-2a 6.1 s | Y-F0 holdings re-run inside the list (26/27 identical, `summary.json` producer only); **q +0.0502, q_lit +0.0671**; ex-ante vol ratio not produced; Debug cross-check equal at G-2a's digits |
| 6 S2R-DIAG | list `29adf2fa`, result final docs commit | - | `p9-m2g-s2rd-u` 94.6 s, peak 1,617 MiB (Release audit-exact u, fresh cache `build-equity/s2rd`) | `smax` 0, `smax5` 0 cells NaN under AuditExact and finite under ResearchFast (all sessions and score window) |

Next free build tag **p9-2m**. Known-red unchanged: pytest `test_fields_are_the_rule_applied_to_the_registry` (E2);
Release lit-golden is a named skip (M2c-RED-2 / OD5-2). New open item (not a test red): COV-7 identity miss.

### M2h: E2, 2026-10-04 (merged by the stopped root agent; verified and recorded by a fresh root, Ruling M2H-1)

Base `2fbef84e` (housekeeping docs). Code head `59893e9f`. Detail: `handoff-root-m2h.md`; logs `m2h-logs/` (the
stopped agent's `e2-*` / `root1-*`, this agent's `root1b-*` / `m2h2-*`; pre-run list `m2h2-prerun.md`). Zero trials:
`build-equity/trials.jsonl` 133 lines `27e40f9f` at start and end.

| lane | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity / root items |
|---|---|---|---|---|---|---|---|---|
| E2 | `80c322ef` (re-review 4 APPROVE `dd857198`, 0 Required; contains root `42f42a07`) | `430c29e8` (of `dd857198`) + E2-18 slip `1dd0ac46` (S2: `wave_stage_kinds.py:421` rewrapped, string unchanged) + SQL2-CLS `59893e9f` (`atx.research-wave/v2` -> wave-manifest) | 0 conflicted files | none (Python lane; no C++ / CMake change from the exes' sources `c2416d62` / `3fd0bf98` to HEAD); real exes used: Release IC p9-2j `fccaa07e`, NAV p9-2l `53ca5dd4` | n/a | none | `scripts/tests` seed 0 **510p / 4s / 2f** (this agent; the two e27b known reds, 0 exit 0xC0000142), seed 1 510p / 4s / 2f (stopped agent, same head); the stopped agent's seed-0 run (339p / 173f, 152 `git init` + 9 HARD-STOP at exit 3221225794) is VOID (ENV-1); `test_fields_are_the_rule_applied_to_the_registry` passes by name (E2's red cleared); stage chain 9/9 both seeds (stopped agent); `test_research_store_identity.py` 16p both seeds; SQL2-CLS: guard 1 fail before / 5p after, store suite 43p both seeds, fixture chain `d32f7655...4e69` (stopped agent); Release live canary `test_cycle_e2e.py` 15p | live-table check PASS on both trees, MR-4 rows included (stopped agent); MR-4 + C2 S13 at HEAD as the lane placed them (5+/2- in 2 files, lane head = HEAD on both); engine match on real exes (tiny world, P9-C shape with a free `--trade-fraction` constant for the gross change): exe calibration record complete, `leverage_requested` 1.12 = cell L, `target` = G_parent, L 1.12 -> **L' 1.0606**, G 1.0419 -> 0.98778 vs G_parent 0.98669, reader G = exe G bit for bit, cell of record plain at 1.0606 (argv `--aim-leverage 1.0606`, no `--calibrate-*`), verify passes and pins `record_spec_digest`; literal `aim_leverage` 1.5 stops at match, exit 3 (`:374`); **O1 (E2-16)**: template-set `ref.leverage` after match = **1.0606 under `pm6-6`** (L'; its `-gm` run then refuses at the ref phase, exit 3) and **1.12 under `pm6-6-engine`** (the cell's own L; ref NAV ran at 1.12); W1-I1 worst `v70-lo3` 249 (margin 1), none refused, canary `%TEMP%` margin 8 (2 audit-exact) |

Next free build tag **p9-2m**. Known-red at `59893e9f`: the two e27b tests of `test_research_spec.py` (cleared by D2R's
merge); Release lit-golden is a named skip (OD5-2).

### M2i: D2R, COVR, item 4 re-run in Release (D-COV, COV steps 4 and 5, store pins), 2026-10-04

Base `e8e424b9` (docs; code `59893e9f`). Code head **`c8ddf736`**. Detail: `handoff-root-m2i.md`, gate report item 4 (d)
(pre-run list `f1d2a102`, committed before any run); logs `m2i-logs/`. Zero trials: `build-equity/trials.jsonl` 133
lines `27e40f9f` at start and end.

| lane | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity / root items |
|---|---|---|---|---|---|---|---|---|
| D2R | `283043ce` (light review APPROVE `6d3eb19a`, 0 Required; contains root `d7f762cf`) | `5afb5447` (no SQL2-CLS: no new literal) | 0 conflicted files (E2's `test_research_spec.py` hunks and D2R's auto-merged) | **p9-2m** equity-dev `atx-research-admission, atx-research-composition, atx-equity-strategy-targets` (1 TU, 9.9 s; admission `411af33a` / composition `b0175bd1` no work = current, targets `c02717ef` rebuilt: replaces the stale p9-2d) | 0 | none (Python lane) | `test_research_spec.py` **87p / 0f** (both e27b tests green); 15 fitter / composition files with real exes (p9-2m) **286p / 0 skip / 39 sub** (the two exe-gated tests ran green); ROOT-1 `scripts/tests` seed 0 **512p / 4s / 0f**, stage chain 9p; 0 exit 0xC0000142 | report / review arrived with the merge (= lane blobs, CRLF only); known-red set now **empty** |
| COVR | `494ec9b2` (light review APPROVE `0145a834`, 0 Required, 6 Suggested; contains root `d7f762cf`) | `c8ddf736` (test-only) | 0 | **p9-2n** equity-dev and **p9-2o** equity-rel, each `atx-impl-strategy-target-tests, atx-engine-risk-tests, atx-equity-strategy-risk` (7 TUs, 49 s; 63 TUs, 293 s); risk exe Debug `9f2c6934`, Release **`ccc73cd1`** | 0 / 0 | Debug 21 / 29 / 106 / 431 + 1 skip; Release 21 / 29 / 106 / 430 + 2 skips (the named two); 0 failed; `CovStageDigest.*`: 0 ran, 1 DISABLED in both | ROOT-1 `scripts/tests` seed 0 **512p / 4s / 0f**, stage chain 9p | diff = 2 test files, no pin; review S-2 / S-3 / S-6 applied as one-line doc slips in root's `task-COVR-report.md`; S-1 ruled by COVR-1; S-4 / S-5 P10 notes |

| item 4 (d) | runs (bounded, `--seconds 600 --max-rss-mib 2048`, Release) | result |
|---|---|---|
| C1 S9 | before each COV run | holds x4 |
| C2 D-COV v1.1 | `p9-m2i-dcov-v11` 47.1 s, 633 MiB | store kept, manifest `39736647...23c7` |
| C2 / C6 D-COV atx-cov-v1 (= step 5) | `p9-m2i-dcov-covv1` **211.2 s, 601 MiB** | store kept, manifest `ea58691f...f0f3`; `recipe.eigen_adjustment` printed (a = 1, 64 sims, seed 7, Kish HL 504) |
| C3 Release = Release | `-r2` repeats under affinity mask 1 (46.6 s, 181.3 s) | **holds**: 12 / 12 files byte-identical per recipe, roots equal |
| C4 D-COV read | `dcov_read.py` | printed in gate report 4 (d) (risk-forecast accuracy only) |
| C5 step 4 | Release NAV p9-2l `53ca5dd4` on the Debug-built container, 30.5 s | **holds**: 27 / 28 byte-identical, `summary.json` producer `build_type` / `engine_git_sha` only; COVR-1 reading: no `exp2` / `pow` / `exp` on the `--risk-model` read path |
| C7 pins | `store_pins.py` | both stores' manifest / payload / container pins recorded; `-r2` dirs and `p9-m2g-cov7` deleted (2.1 GB) |

Next free build tag **p9-2p**. Known-red set after M2i: **empty** (Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`
stays a named skip, OD5-2).

### M2j: C3 part A merge, D-COV-EIG, 2026-10-05 (stopped by the OWNER STOP at a clean point; partial)

Base `5c57c1ef` (docs; code `c8ddf736`). Code head **`327c86b2`**. Detail: `handoff-root-m2j.md`, merge report "M2j"
(pre-run lists `9c7aa654`, committed before any run), gate report items 4 (d) / 4 (e) (D-COV-EIG; amendment 1
`438f0481` before attempt 2); logs `m2j-logs/`. Zero trials: `build-equity/trials.jsonl` 133 lines `27e40f9f` at start
and end.

| lane | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity / root items |
|---|---|---|---|---|---|---|---|---|
| C3 part A | `0dbfa459` (re-review 1 APPROVE, 0 Required; merged by SHA, C3b continues on the branch) | `327c86b2` (no SQL2-CLS yet) | 0 | **p9-2p** equity-dev and **p9-2q** equity-rel, each the lane's nine targets `-Canary` (130 TUs, 731 s; 132 TUs, 769 s; `CCACHE_RECACHE=1`); NAV exe Debug `637f7069`, Release **`adcf72d4`** | 0 / 0 | named 33 / 33 Debug and Release; whole target-tests Debug 390p / 0f / 1 DISABLED; Release whole target-tests and `atx-impl-tests` NOT run | NOT run (ROOT-1 pending) | canary 15 / 15 both goldens; **item 2 holds** (X-5 vs `p9-c1-s2-x5-nav` and `p9-c1-rel-x5-nav`, Y-F0 vs `p9-b0-yf0-nav`: producer keys only); **item 3 holds** (`--spec` = argv 27 / 27 both cells; digests x5 `53fd737d`, yf0 `a793e183`); items 4, 5, 6 (SQL2-CLS), 7 (W3-8), 8 (C3-5 probe), 9 (ROOT-1) and M2I-HK NOT done |

| Part B | result |
|---|---|
| D-COV-EIG (COV-13 (b)) | attempt 1 void (its session set failed the list's T_k check; slip in the tool, amended before attempt 2); attempt 2: session check equal on all 51 ranks; **no rank 00-02 structurally null in either recipe**; third pooled row = the all-ranks row (N 54,770) |

Next free build tag **p9-2r**. Known-red set: empty, but the classes guard (`test_research_store_classes.py`) is
expected to fail on `atx.nav-spec/v1` until SQL2-CLS lands (not run).

### M2j finish (RESUME 7, R7-1), 2026-10-05 (BLOCKED at item 8 only: duplicate `--rule`, a stop for the PM)

Start docs `4dc831ac` (code `327c86b2`); housekeeping docs `a15ad887`. Code head **`ebc918c3`** (SQL2-CLS). No lane
merged, no build (EXE-CUR: 0 C++ / CMake files changed since `327c86b2`; exes p9-2p / p9-2q current). Detail: merge
report "M2j finish"; logs `m2j-logs/`. Zero trials: `build-equity/trials.jsonl` 133 lines `27e40f9f` at start and end.

| item | result |
|---|---|
| 4 opt-in (PRE-7a, C3-9) | **holds**: `--ann-mean-gross` adds exactly `/scenarios/0..4/ann_mean_gross` to `summary.json`; every other file byte-identical; strip = flag-absent bytes |
| 5 Release = Debug | **holds**: X-5 Debug p9-2p vs Release p9-2q only `/producer/build_type`; vs `p9-c1-s2-x5-nav` only `/producer/engine_git_sha`; walls X-5 Rel 41.7 -> 38.4 s, Dbg 62.6 -> 84.2 s, Y-F0 Rel 27.4 -> 35.8 s (load not controlled) |
| 1 rest | Release whole target-tests 390p / 0f / 1 DISABLED; whole `atx-impl-tests` Release and Debug 1,201p / 5 skip / 0f |
| 6 SQL2-CLS | `ebc918c3`: `atx.nav-spec/v1` in `nav-output`; guard 1f -> 5p; store suite 43p seeds 0 and 1; fixture chain `d32f7655...4e69`, identity 12 / 0, rel = abs |
| 9 ROOT-1 | `scripts/tests` seed 0 512p / 4 skip / 0f; stage chain 9p; ENV-1 hits 0 |
| 8 C3-5 probe | **STOP**: the calibrated NAV argv carries `--rule aim-partial-v5` twice (`phases.py:829` from `nav.rule` + the row x-flag via `wave_kinds.py:385-399` / `research_spec.py:128`); NAV exe refused (exit 2); wave exit 4 at match |
| M2I-HK | `p9-m2i-cov-y1-nav` deleted by literal path (12 MB) |
| 7 W3-8 | not root's (W3-8a) |

Next free build tag **p9-2r**. Known-red set: **empty**.

### M2 (RESUME 9): B2 post-identity redo, T2, 2026-10-07 (two stops for the PM, both attributed to `5dfb0ba1`)

Start docs `685e238b` (code `fd084e78`); housekeeping docs `944581cc`. Code head **`fcb0200e`**. Detail: merge report
"Step group M2 (RESUME 9)"; logs `m2-logs/` (tools `m2-logs/tools/`). Zero trials: `build-equity/trials.jsonl` 133
lines `27e40f9f` at start, after each real-data run and at the end. TEST-1 / ROOT-1a: targeted checks only.

| lane | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity / root items |
|---|---|---|---|---|---|---|---|---|
| B2 | `46ffe3dc` (re-review 2 APPROVE of `53856c8e`, 0 Required, 5 Suggested) | `5dfb0ba1` (tree = `git merge-tree` recompute; code = lane's) + slips C3B-6 `6c201a1d` (`scripts/cycle/spec.py`, 3 text hunks, +12 / -10) and B2-S S-1 `fcb0200e` (3 line wraps, +5 / -3, tokens unchanged) | 0 | **p9-2t** equity-dev; `atx-research-eval,atx-engine-research-eval-tests,atx-engine-research-ledger-tests,atx-impl-tests,atx-engine-research-fields-tests` (9 TUs, 7 links) | 0 | eval 26 / 26 (PRE-15 gtest in); ledger 16 / 16; `atx-impl-tests` `*Ledger*:EvalVerb*` 29 / 29 | `scripts/tests` B2 files 317p / 3 skip; `atx-impl/tools` B2 files + edges fixture 76p / 1 skip; root-only `test_legacy_n37_numbers_reproduced` **1 FAILED** (pre-merge `944581cc`: 1 passed; 0 leaves beyond 1e-12, B2-R1 floats + one B2-6 degenerate row) | identity X-5 / Y-F0 **holds** (stdout equal but the one PRE-15 line; JSON 137 leaves bit-equal outside `tot_minus_m`; without `--dsr-total` byte-equal); PRE-15 public cross-check **MISS**: n 124 = 233 + 1 (Y-F0 not ledgered) - 110 vs R0-12's 123; the oracle at N 123 reproduces R0-12's value (stop for the PM); SQL2-CLS none (guard 5p); SQL3-2 holds |
| T2 | `dfa4218d` (review APPROVE of `2e9d6813`, 0 Required, 6 Suggested; code `bc618cb4`) | `27e69387` (as a unit, T2-SLOT; tree = recompute; 20 files = `bc618cb4`) | 0 | none (no C++) | - | `ResearchFields*` 92 / 92 on p9-2t (no-change control) | T2's 22 modules 248p; other lanes' tools tests without the bind (7 files) 87p | A1-SHIM moot (no root edit of T2's files since `1239a5ff`); SQL2-CLS none; S1 ledger facts: xdata (2, 4) -> (1, 2), v9_nt (sealed_nt, 1) -> (sealed_nt, 2), SEC `:475, 480, 482` 14 -> 17, 66 -> 67, -52 -> -51 (+ assertion `:483`); whole `atx-engine/tools` seeds 0 / 1 deferred to the gate |

Next free build tag **p9-2u**. Known-red set: suites **empty**; root-only red `atx-impl/tools/test_nav_summ_v8.py::
test_legacy_n37_numbers_reproduced` (attributed to `5dfb0ba1`, for the PM).

## P9 wave-3 merges

Detail per lane: `root-wave3-merge-report.md` (same directory). B2 and T2 rows stay under the wave-2 heading.

### M1 (RESUME 8): SQL3, 2026-10-06

Start docs `1b64ce45` (code `ebc918c3`); housekeeping docs `cfd5a25d`. Code head **`fd084e78`**. Logs `m1-logs/`.
Zero trials: `build-equity/trials.jsonl` 133 lines `27e40f9f` at start and end.

| lane | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity / root items |
|---|---|---|---|---|---|---|---|---|
| SQL3 | `a02c074f` (APPROVE, review `69b8c162`, 0 Required, 8 Suggested) | `fbd2c045` + SQL2-CLS `fd084e78` (`atx.run-store/v1` on `run-receipt` / `run-start`) | 0 | **p9-2r** equity-dev, **p9-2s** equity-rel; `atx-engine-research-catalog,atx-engine-research-catalog-tests,atx-research-store` (14 / 25 TUs) | 0 / 0 | `ResearchCatalog*:SealGuard*:StoreCli*` 28 / 28 on both | hook file 8 (real exe); book monitor 13; store suite 43 x seeds 0 / 1; stage chain 10 x seeds 0 / 1; `scripts/tests` seed 0 @@S0@@, seed 1 @@S1@@ | guard 1f -> 5p; fixture chain `d32f7655...4e69` unchanged, identity 12 / 0, rel = abs; tiny world catalog off 69 / 69 identical (F-1 filter), runner equal outside the substitution list, no `store` key: **holds**; SQL3-2: default catalog path absent, hook OFF, 0 of 1,473 receipts carry the block: **holds**; G-P10 (a) P10 for SQL3 / gate, (b) PARTIAL, (c) gate, (d) PARTIAL, (e) LANDED; item 5 pending (B2) |

Next free build tag **p9-2t**. Known-red set: **empty**.

### M3 (RESUME 10): C3b, D3W, ABS, 2026-10-08 / 09

Start docs `baefd8a9` (code `fcb0200e`); housekeeping docs `8346fc2e`. Code head **`094bcd3b`**. Logs `m3-logs/`.
Zero trials: `build-equity/trials.jsonl` 133 lines `27e40f9f` at start and end. TEST-1 / ROOT-1a: targeted checks;
whole suites, Release, canaries and the parents' identity runs deferred to the gate.

| lane | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity / root items |
|---|---|---|---|---|---|---|---|---|
| C3b | `b2517fe4` (APPROVE, review `76b9f256`, 0 Required, 5 Suggested) | `b182dfdd` (tree = recompute `49159e4f`) | 0 (keep-both with B2 in 3 test files: every hunk kept) | **p9-2u** equity-dev; `atx-impl-strategy-target-tests` (16 TUs) | 0 | named 34 / 34 incl. `NavSpec.GridFileRefusesADuplicateKey` | lane's 8 wave files 104; `test_wave_kinds.py` + `test_wave_eval_verb.py` 118; classes guard 5 | items 1, 3, 7 **hold**; C3B-S tools listed (not rewritten); `sign_pm7_35` row still names E2 (SIGN / root at the freeze); D3 keep-both and the 5 docstrings -> M4; NAV flag-absent run -> gate |
| D3W | `1e6c4336` (APPROVE, review `f0726acb`, 0 Required, 10 Suggested) | `08f04e07` (tree = recompute `a9f66958`) | 0 (B2-EVAL-KEEP: `eval_report.cpp` B2 8 + D3W 7 hunks, `.hpp` 2 + 2, tie test 2 + 1, `test_research_spec.py` 7 + 1: all kept) | **p9-2v** equity-dev; eval verb, eval / composition / IC tests, composition, admission, IC exe (33 TUs) | 0 | eval whole 27 / 27 (incl. PRE-15 `HandRowAtNTotMinusM...` and `VolFlagAddsNetVolOnly`); composition 17 / 17; IC filter 18 / 18 | fitter / composition 4 files 155 / 1 skip; with p9-2v exes `test_fit_walk_forward.py` + `test_composition_engine_fit.py` 47 / 0 skip; `test_research_spec.py` 87; guard 5 | identity `ident.py` 17 / 21 identical + 4 fingerprint-only: **holds**; fingerprints: only `composition_engine_fit`, `composition_resid` move, `OUTPUT_PRODUCERS` `d28a3089` unchanged; D3W-STALE holds; sleeve line KEPT (Appendix IV item 14) |
| ABS | `5dae4f69` (APPROVE, review `6a3f5043`, 0 Required, 6 Suggested) | `898bf120` (tree = recompute `639a26a5`) + SQL2-CLS `094bcd3b` (`atx.od3-flagged-members/v1` own class `od3-drop-list`; `atx.carry-weights/v1` in `fit-output`) | 0 | **p9-2w** equity-dev; `atx-equity-strategy-ic,atx-impl-strategy-ic-tests` (26 TUs) | 0 | `AbsentMembers.*` 11 / 11; identity filter 53 / 53; D3W's IC filter again 18 / 18 | fixture readers 138; guard 1f -> 5p; store suite 43 x seeds 0 / 1 | fixture chain `d32f7655...4e69` unchanged; `--list-rules --json` = fixture, `absent-members-v1` first, `exe_capabilities` holds it: **holds**; od3 plan refuse / plan on fakes -> M4 (OD3P not in root); IC flag-absent passes -> gate |

Next free build tag **p9-2x**. Known-red set: exactly `atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced`
(root-only, B2-N37, merge `5dfb0ba1`); no new red.
