# W0 VWAP correction: G0 impact and rerun contract

Prepared 2026-09-25 from source, recipes, receipts, and input metadata only. No numeric runs, builds, warehouse access, or data-payload reads were performed for this investigation. This is an impact plan, not acceptance of the pending D0 implementation.

**Only L9 requires a numerical rerun for the VWAP change.** Preserve every old artifact. Do not regenerate contexts, rebuild the cp21 diagnostic pin, or rerun unaffected recipes solely because the default VWAP rule changes.

The inspected D0 follow-up in pool-4 introduces `VwapRule::RawDailyCloseV2` as default and retains `AdjustedTypicalV1` explicitly. V2 derives a daily-close proxy from valid raw close and positive volume and replaces a stale supplied VWAP column; it is not an intraday VWAP observation. Other adjusted OHLC values remain unchanged and qualified. The final reviewed implementation SHA and binary hash are pending and must be supplied by the orchestrator before execution.

## Impact matrix

| Frozen evidence | Actual inputs and augmentation path | Required action |
|---|---|---|
| L9 mining | `stage_equity_mine.cpp`: `load_context` -> `build_role`/`stitch_span` -> `with_alpha101_fields` -> search. Forty-three Alpha101 fixture expressions and one literature seed use VWAP; the generated search field set also includes it. Six unique 2013-2018 t1000 contexts supply raw close and volume. | Rerun the full search and validation with V2. Regenerate candidates, validation, library, gate report, and trial registry. Changed fitness can change later search genealogy, so rescoring old candidates is insufficient. |
| L10 fundamental zoo | `FundamentalZoo.RealDataIcReport` reads 11 contexts for 2013-2018, fundamental points, and the two 2018 survivor contexts. `run_context` calls `derive_fields` and `with_fundamental_fields`; expressions use fundamental fields, close, cap, and sector. No Alpha101/datafield augmentation or VWAP expressions. | Reuse existing evidence with its existing qualifications. |
| L7 scorecard | `BM_L7RealScorecard` reads `meta.txt`, `close.f64`, `volume.f64`, `cap_tn.f64`, `sector_tn.f64`, and `dates.i64`. The converter selects close, volume, market cap, sector, mask, and axes; it does not augment VWAP. | Reuse existing raw inputs and evidence. |
| All 13 cp21 baseline and 13 IC runs | Baseline recipes contain two close-only momentum expressions. All IC recipes have the same 29 signals, using `atmCenI_126d`, `atmCenI_21d`, `close`, `earnFlag`, `high`, `low`, `open`, `raw_close`, `sector`, and `volume`; none uses VWAP. Baseline views exclude VWAP and require unaugmented contexts. Explicit dollar-volume expressions use raw close times volume. | Reuse baselines, IC outputs, downstream scorecards, and capacity results. No cp21 pin rebuild. |
| Native 2013 baseline and Abort control | The unaugmented native context feeds the same two close-only momentum expressions. Baseline/replay consumes close, raw close, and volume without VWAP augmentation. The disclosed run covers `[2013-04-04, 2014-01-01)`. | Reuse numerical evidence and preserve the existing assumed-liquidation and alpha-evidence-ineligibility disclosures. |

This matrix is specific to the VWAP change. A later independent change to one of these paths requires its own impact assessment.

## Frozen evidence and source anchors

Immutable old root: `C:/atx-wt/g0-data/bc5cc646_20260925` (abbreviated `OLD` below).

| File relative to OLD | SHA256 |
|---|---|
| `g0-artifact-manifest.json` | `3e2fd328a15c6671d81aff9aa2012388aad924e995b6b8185591259f117da679` |
| `input_contexts.json` | `19089632154a92c2aa10b49865a7ee1caaf3d54eef56d14cd8b9eac279fbac7b` |
| `logs/l9.receipt.json` | `616e543713988c9e14be4100038a68ae9b8970dace31e4501b5ce39c28673d62` |
| `data/equity_mine_l9_guard_g0_bc5cc646/manifest.json` | `e3a3f0ddb94e79f32c264b29b9c8dd597bec2f2139d96c274535467dffb1db8e` |

The frozen L9 source is `bc5cc646b46f6a7c23a60e87d28dfa9b972671ec`; its receipt binds argv, environment, executable hash, source, and resources. L10 and L7 receipts name the same frozen W0 base. The disclosed native baseline receipt names `c32df9512075879827b75f5e465f2640c579d4c8` and its archived executable under `OLD/bin/corrected-disclosed`.

Stable source anchors below were checked in pool-3 at `b67abaf6569817a586f4ed8462dc4ef37c0a67d5`. Use `git show <sha>:<path>` and the named function if line numbers move:

- `atx-impl/src/stage_equity_mine.cpp`: `literature_seeds` (535; VWAP reversal at 559), `load_context` (1417), `build_role` (1486; stitch 1499, augmentation 1529), and `make_config` (1773; numeric search-field selection). `atx-impl/tests/fixtures/alpha101.txt` contains the 101 fixture expressions; 43 noncomment expressions mention `vwap`.
- `atx-impl/tests/fundamental_zoo_test.cpp`: `run_context` (437; panel read 447, fundamental derivation/extension 472/474), fixture/reference evaluation thereafter. `atx-engine/include/atx/engine/data/fundamental_fields.hpp`: `with_fundamental_fields` (346). Fixture: `atx-impl/tests/fixtures/fundamental_zoo.txt`.
- `atx-engine/bench/risk_model_real_scorecard_bench.cpp`: input reads at 139-146. `atx-engine/bench/tools/l7_panel_to_raw.py`: field selection and output at 69-83.
- `atx-impl/src/equity_baseline_views.cpp`: required fields at 31, optional fields immediately following, `append_context_fields` for borrowed views. `atx-impl/src/stage_equity_baseline.cpp`: `require_context_recipe` (299; augmentation checks 316-317), context read 359. `atx-impl/src/stage_equity_ic.cpp`: context read 1144, baseline evaluation/combo reads 1171-1172, family evaluation through baseline views.
- Pending D0 patch anchors in pool-4: `atx-engine/include/atx/engine/alpha/vwap_rule.hpp`; `alpha/datafields.hpp` supplied-column replacement under V2; `alpha/augment.hpp` rule forwarding to datafield derivation; `atx-impl/src/stage_equity_mine.cpp` explicit rule parsing, augmentation forwarding, and report persistence. Review these at the final committed SHA, not a moving worktree.

Recipe/receipt anchors enabling a bounded recheck:

- L9: `OLD/logs/l9.receipt.json` is the authoritative argv; `OLD/data/equity_mine_l9_guard_g0_bc5cc646/manifest.json` binds train/validation context paths, artifact IDs, payload hashes, and membership SHA256 `776bda8045feed80d1e21fd31a3130539a8d2f334bb8fd9bb372faceb1094ac4`.
- L10: `OLD/logs/l10.receipt.json` records the fundamental points and context/survivor environment variables. L7: `OLD/logs/l7.receipt.json` records `ATX_L7_REAL_DIR=C:/atx/data/l7_riskmodel_raw_pit_2014_t1000_20260923`.
- cp21: `OLD/data/equity_g0cp21_base_{year}_t{cut}_bc5cc646/evaluation.bin.manifest.json`, JSON-decoded `recipe.signals`; corresponding `equity_g0cp21_ic_{year}_t{cut}_bc5cc646/manifest.json`, `recipe.signals`. Cells cover 2013-2019, cuts 1000/3000, with 2017 t3000 absent. All 13 baseline signal lists have canonical JSON SHA256 `b03c6463e423efd5ec6a08c929e5e2c594cdd652c1c9654e980ba78106d4033e`; all 13 IC signal lists have `eeb3f3e991eb2ab1d9333f99b573e43fcdc0f3a6bade1ee23ea110e87b6bd28f`. Canonical encoding used Python `json.dumps(signals, sort_keys=True, separators=(',', ':')).encode('utf-8')`.
- Native 2013: `OLD/logs/base2013_disclosed.receipt.json`, `OLD/logs/base2013_abort_disclosed.receipt.json`, and `OLD/data/equity_baseline_training_2013_g0_bc5cc646_disclosed/evaluation.bin.manifest.json`.

## Context reuse and stale-column guard

All 14 contexts indexed by `OLD/input_contexts.json` were checked via their `context.bin.manifest.json` only: the 13 cp16 contexts above plus `C:/atx/data/tickerhistory_training_native_20260919/context.bin`. Each manifest's `recipe` is a JSON string that must be decoded before inspection. Each declares:

- `augmentation.enabled=false` and `augmentation.adv_windows=[]`;
- fields `close, raw_close, volume, high, low, open, market_cap, sector, earnFlag, atmCenI_21d, atmCenI_126d, nEarnCnt_5d`;
- no supplied `vwap`, `dollar_volume`, or ADV field;
- raw close as `unadjusted-as-traded`; research OHLC as `raw-OHLC*cumulReturnFactor-pointwise`.

The recipe's dormant `dollar_volume_basis` label does not mean augmentation was performed. Reuse the existing context identities and payload hashes without reserialization, relabeling, or adjustment changes. L9 computes fresh V2 VWAP from raw close and volume after stitching. For other, already augmented inputs, the production V2 path must replace a supplied stale VWAP; preserve and verify the implementation's compiled regression checks before release. Such inputs are not among these 14 G0 contexts.

## Authorized rerun contract; execution held

The orchestrator has authorized the numerical L9 rerun once reviewed D0 production code, build, and final source are ready. Execution is held until it supplies the exact SHA/binary and releases the heavy slot. This note does not request another user permission step.

Proposed fresh output root, not yet created: `C:/atx-wt/g0-data/w0-vwap-v2_<final-repair-sha8>_20260925`. Use fresh `bin`, `logs`, and `data/equity_mine_l9_guard_v2_<sha8>` children. Preserve OLD unchanged.

1. Bind the final reviewed source SHA and actual executable SHA256. Verify the reviewed source forwards the chosen rule through mining augmentation and persists `vwap_rule`, basis, and `vwap_is_intraday_observation=false` in its report/config. Require the production supplied-VWAP replacement checks to have passed.
2. Acquire the shared `C:/atx/data/.heavy-run.lock` atomically under the existing coordination protocol after root releases the slot; never overwrite another owner's lock. No competing real-data workload and no claim of benchmark timing comparability.
3. Replay `OLD/logs/l9.receipt.json` exactly, changing only executable/output paths and adding explicit `--vwap-rule raw-daily-close-v2` (confirm the final CLI spelling). Preserve train contexts 2013-2016, validation contexts 2016-2018, starts 2013/2017/2019, holdout off, seal 2020-01-01, membership/cut, fixture, population 192, generations 15, threads 2, seed 20260923, and every remaining evaluation/gate/budget parameter. No retuning or additional dataset acquisition.
4. Recompute augmentation and the complete search/validation with fresh trial registry state. Do not import old candidates, fitted search state, or old trial/cluster counts. Produce new candidates, validation, library, gate report, and registry artifacts.
5. Record argv/environment, wall time, peak memory, exit status, final source/binary hashes, exact fixture bytes, input-manifest and payload hashes, membership, and explicit VWAP semantics. Verify internal file bindings and publish the new outer manifest last, including the OLD parent-manifest hash above.
6. Compare old/new search counts, actual trial/cluster counts, validation and admission results. Changed search genealogy need not provide a one-to-one candidate comparison. If other intervening repairs are present, call this corrected evidence, not an isolated VWAP causal estimate.

The prior L9 run took 987.109 seconds and peaked near 1.01 GiB. These are historical scheduling observations, not a memory guarantee or a benchmark prediction. Resource scheduling remains with root.

## Manifest gap and evidence limits

The old L9 manifest binds contexts and membership but does **not** explicitly bind fixture bytes; the outer old G0 manifest also has no fixture entry. The receipt supplies its path, and frozen source supplies the tracked fixture text, but neither establishes the old runtime file's byte hash. Do not retroactively claim that binding.

The current pool-3 `alpha101.txt` working bytes hash to `1a04d5e7182e331e1f20c87c17528ae12156036e195fdbcf47995fa92aae1e9b`; the LF blob at frozen `bc5cc646` hashes to `265c7babc099a17ca242a00ae5299d9b8f407f6d7bf168e16006189f89312385`. This checkout has CRLF working bytes for that fixture. Preserve and hash the exact rerun fixture bytes; do not silently normalize or equate these byte hashes. Record semantic text correspondence separately if needed.

Existing negative results and native assumed-liquidation qualifications remain in force. This plan does not claim tradeable-alpha acceptance, completion of the corrected L9 run, or closure of the final W0 gate.
