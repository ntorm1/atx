# Task B-1 report: incremental u pass (`--no-composition`)

Lane B, worktree `C:/atx-wt/pool-10`, branch `feat/platform-v8-b-20260929`. Nothing was built or run.
Commit: the one carrying this report (on top of B-3's `bdca4c3d`).

## What was built

Flag `--no-composition` on `atx-equity-strategy-ic` (`IcRunnerConfig::no_composition`, strategy_ic_runner.hpp).
With it the run scores every candidate's IC and TRAIN orientation and builds no blend:

- `score_role` (strategy_ic_runner.cpp): no `IcComposition`, no effective-membership mask, no `__combined__` daily
  rows, no `<role>_planned_targets.csv`, no combined artifact. A `--save-combined` request is skipped (see
  Deviations). Role results omit `combined_ic`, `planned_target_proxy`, `combined_artifact` and
  `stage_seconds.composition`; `combined_evaluations` is 0. Candidate rows omit `stage_seconds.composition`.
- Summary top level: `"composition": "skipped"` (absent without the flag). The `--plan-only` JSON carries the same key.
- **Candidates are not loaded when both caches hit.** With `--candidate-cache`, a candidate whose signal entry
  resolved at preflight has its IC-result entry probed first, keyed on the payload SHA256 its sidecar records
  (the key the IC entry was written under, after that payload was verified). On an IC hit the payload is not
  opened: no read, no hash, no VM, no field load. Progress prints
  `IC cache-hit <id> role=<r> layout=v2 payload=not-loaded ic_result=hit`. On an IC miss the candidate loads and
  scores exactly as before (one probe, not two).
- Admission (strategy_ic_admission.cpp `admit`): the composition plane (`ic_composition_working_bytes`) is not
  admitted under the flag; every other term, including the per-worker envelope, is as coded. `required_bytes`
  and `admitted_working_bytes` are lower by exactly that plane.
- Refusal: `--no-composition` with `--composition-weights` refuses in configuration preflight, before the library,
  any payload or the output directory (`IC runner: --no-composition builds no blend, so --composition-weights ...`).
- `atx-impl/tools/equity_strategy_ic.cpp`: a verb table. `atx-equity-strategy-ic <verb> ...` routes to the named
  handler with argv shifted (the handler sees the verb as argv[0]); anything else is the historical option list.
  One verb today, `ic` (= the default). Lane F adds `marginal` as one line:
  `{"marginal",&atx::impl::strategy::dispatch_marginal},` (signature `int(int,char**,std::ostream&,std::ostream&)`).

**Identity.** The flag is not a method input: `recipe.json` (and so `recipe_sha256` inside `orientations.json`) is
unchanged, including the `saved_combined` key when `--save-combined` is passed. Member rows and orientations are
computed by the unchanged code path; the blend never fed them. Without the flag every branch reduces to the
previous code (the composition is created, added and finished in the same order; JSON objects are key-sorted, so
keys added after construction dump identically).

## Tests (atx-impl/tests/strategy_ic_runner_test.cpp)

- `NoComposition.SkipsBlendAndCombinedRows`: no `__combined__` rows, no planned targets, no combined files even
  with `--save-combined`; summary and plan say `skipped`; recipe bytes equal the full run's; admitted bytes lower;
  `--composition-weights` refuses before any output, plan-only too.
- `NoComposition.HitWithIcResultIsNotLoaded`: after a cold cached run every payload is tampered at equal size. The
  screening pass succeeds, logs `payload=not-loaded`, reports `verify_bytes` 0 and cache_load 0, and its
  orientations and member rows equal the cold run's. The default pass refuses on the payload SHA256; deleting one
  IC entry makes the screening pass load (and refuse on) exactly that candidate.
- `NoComposition.MemberRowsByteIdenticalToDefault`: with a field candidate, 2 workers and `--save-combined`:
  `recipe.json` and `orientations.json` byte-identical; each daily CSV equals the full run's minus `__combined__`
  rows; candidate rows and field accounting equal.
- `StrategyIcRunner.CacheMissOnRoleChange`: two roles (same sessions, different score window, so different role
  manifests) on one cache root: the second role misses every signal and IC entry, its entries live under its own
  role SHA, its outputs equal an uncached run; the first role still hits 2/2.

## How root verifies

Build: `atx-impl-strategy-ic-tests atx-equity-strategy-ic`.
gtest: `atx-impl-strategy-ic-tests --gtest_filter=StrategyIcRunner.*:NoComposition.*:StrategyIcComposition.*`.

Identity (step 3 of the brief): the v7.1 u pass as receipted (`mega-v71-train-u-run1/receipt.json`) plus the flag:
`atx-equity-strategy-ic --library atx-impl/strategies/fund_industry_ic_v71.json --library-sha256 787c802e...2259
--train build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --train-sha256 3e79978a...b809
--train-fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9 --train-fields-sha256 8fd00e9f...769b
--output build-equity/b1-v71-train-u-screen --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined
--cache-legacy-fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7
--candidate-cache build-equity/mega-candidate-cache-v71 --no-composition`
- `orientations.json` and `recipe.json` SHA-equal to `mega-v71-train-u-1`.
- `train_daily_ic.csv` member rows equal (the spec's `csv-rows` compare keyed on the library ids, or the file with
  `__combined__` lines removed, SHA-compared).
- Expected on the warm v7.1 cache: 48 signal hits, 48 IC hits, `verify_bytes` 0, no `VM-complete` line; wall is
  role load + labels + IC scope hashing (the 44-hit cache_load of 5.36 s and composition of 9.23 s disappear).
  If some IC entries are missing, those candidates load and score as before; the outputs are the same.
- Record `stage_seconds` vm, ic (composition absent) and wall from summary.json.

## Deviations

- `--save-combined` with `--no-composition` is accepted and skipped, not refused. The v7.1 u pass was run with
  `--save-combined`, and the recipe records it; refusing would force A-3 to drop the flag and change
  `recipe_sha256` and so `orientations.json`, which the acceptance requires byte-identical. The summary's
  `composition: skipped` and the absent `combined_artifact` record that nothing was saved.
- Candidate `stage_seconds.composition` and role `stage_seconds.composition` are omitted under the flag rather
  than reported as 0 (nothing ran).

## Cross-lane edits

None. (A-3 consumes the flag and the `composition` summary key.)

## Open risks

- Not compiled.
- A screening pass trusts the sidecar's payload SHA256 for a hit it does not load. That is the IC entry's own key,
  so its IC result stays correct for those bytes; a payload corrupted on disk since is found by the next pass
  that loads it (any default pass; `--cache-report` only stats extents). Stated so root can rule on it.
