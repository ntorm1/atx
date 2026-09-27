# G0 truth-delta execution, 2026-09-25

- Owner: Codex G0 evidence subagent; branch `feat/w0-g0-codex-20260925`, pool-3.
- Frozen source base: `bc5cc646b46f6a7c23a60e87d28dfa9b972671ec`.
- Lease run/heartbeat: `aes-w0-g0-codex-20260925` (keeper published successfully;
  the expected cold `dev` configure failed after lease publication).
- Output root: `C:/atx-wt/g0-data/bc5cc646_20260925`.
- Root authorized serial heavy G0 measurements, 2019 development reuse and output root.
- No data at or after 2020-01-01 and no warehouse/database opened. Inputs read only.
- COMPLETE: all 35 measurement receipts match expected exits. 54 full comparison
  tables cover 2,027,028 metric/metadata cells. Verification passed for 33 recursive
  manifests and 597 evidence files; `g0-artifact-manifest.json` is published last.

## Implemented preparation

`g0_measure.py` records exact argv, env, binary SHA256, source pin diff SHA256, exit,
wall time and peak working set for every process. Refuses receipt overwrite.
It has independent phases: preflight, baseline, l9, l10, l7, cp21, scorecard.
`g0_compare.py` emits complete old/new metric CSVs, including every cp21 scorecard
and capacity metric and each cell's summary. These scripts use only the named frozen
pre-2020 artifacts and write only beneath the authorized output root.

Preflight executed: all 13 cp21 contexts plus the 2013 native baseline context have
session axes strictly before 2020. Payload hashes and artifact IDs are stored in
`input_contexts.json`. The required-mark audit was copied and SHA256 checked.
Frozen scorecard script SHA256 matches `0e9cc6feb148cc8e52bc72f5a4956fbace62413abbf8dc947310247e45b542c8`.

## Current execution

- Release `atx-impl atx-impl-tests atx-shm-worker atx-engine-bench` build completed
  exit 0; log: `logs/build-release.log`. Configure session 56300 and build session
  51996 completed and consumed. Frozen binaries archived in `bin/unpinned`.
- Build started with 4.46 GiB free, CMAKE_BUILD_PARALLEL_LEVEL=2. `-Jobs 1` affects
  ctest only in this wrapper. Further builds use environment cap 1 per root.
- Initial `configure.log` command passed isolated FetchContent directory but subsequent
  regeneration retained shared `C:/atx-cache/deps`; possible configure/build overlap.
  Root was notified and held other shared-dependency builds. No artifact validity claim
  depends on isolation. Root authorized retaining successful binaries without a solely
  cosmetic rebuild. Isolate and verify cache before a later cp21 pin rebuild.
- Baseline complete, expected exit 1: same security 150340 abort, 2.031 s / 0.146 GiB.
- L7 complete, exit 0: 61.625 s / 0.105 GiB; only three 1e-9/1e-10 numeric changes.
- L10 complete, exit 0: 302.079 s / 1.201 GiB; no positive t>2 candidate.
- L9 completed exit 0, 987.109 s / 1.007 GiB; no admitted alpha, validation blend
  net SR -0.535778 vs old -1.218738. Receipt/logs `logs/l9.*`; tool session 79174 consumed.
- `.heavy-run.lock` is acquired atomically per run with PID/start/token and only
  removed for a matching owner. This coordination file is the sole authorized C:/atx write.
- Root requires importing tested/reviewed replay and DSL-internal D12 repairs before
  corrected baseline and cp21, to avoid rerunning the 13 expensive cells. Replay and
  membership build/tests remain in their own lanes. Root's integration head may be
  merged when both pass. Archive corrected unpinned binary and exact SHA, run native
  baseline, then apply 26-family diagnostic pin and rebuild only atx-impl.
- G0 cp21 build overlap grant: <=3 compiler workers total, >4 GiB free and verified
  per-worktree isolated deps. Environment cap 1; no other shared deps builds.
- Root subsequently authorized provisional imports for compilation while lanes finish
  tests. Imported D12 `39347e28`, replay `8ba15b0e`; measurement source HEAD now
  `8ee78be46c0cfc01d0c892e77fd4a2671ce942f7`. No production source changes afterward.
- Verified isolated cache `C:/atx-wt/pool-3/deps/equity-rel`; configure-corrected exited 0.
- Corrected Release atx-impl build completed exit 0, 210 steps with isolated deps;
  tool session 7202 consumed. Log `build-corrected.log`, archive `bin/corrected`.
- Replay passed whole book 128/128 plus focused impl 32/32. Production `8ba15b0e` unchanged;
  test-only fixture follow-up `4f257729` is not needed for this production binary.
- D12 runtime gate passed: focused 27/27, whole alpha 706/706; independently checked
  owning logs and binary SHA and ran reduced-universe oracle 1/1 PASS. Root explicitly
  authorized corrected baseline and Abort control once this Release build completes.
- Do not commit report-only changes during build and then record the new HEAD as producer;
  this binary's configured source is exactly `8ee78be4`. Archive source marker accordingly.
- Corrected native baseline and explicit Abort control completed, receipts
  `base2013_corrected` and `base2013_abort_control`. Default completes in 2.266 s but
  has 32 assumed missing-price liquidations, stress PnL -$1,957,929.80 and is ineligible
  for alpha evidence. Abort reproduces exact old failure in 1.766 s.
- Diagnostic 26-family/retained22/checkpoint21 pin built 7 steps exit 0, archived
  `bin/cp21-pinned` with exact `bin/cp21-pin.patch`, then both source files restored.
  Compiler slot released to root/audit/replay owners. No pin source diff remains.
- CP21 serial tool session 35837 completed and consumed: all 13 baseline + 13 IC
  processes exit 0, 608.406 s combined, largest peak 0.862 GiB. All 13 signal/cost
  recipes already checked equal to historical recipes, checkpoint21/N80 preserved.
- Frozen scorecard tool session 74687 completed and consumed: exit0, 938.703s,
  peak0.303GiB, 9,080 scorecard rows and 58 capacity rows. R16-8 remains0/29 candidates;
  positive h21 individual-cut CI lower bounds fall2→0. No alpha promoted.
- G0 exposed omitted top-level quarantine fields in baseline summary. Replay owner
  committed tested presentation-only fix `1bdeed388b8819d539175df65c202fd33b6f2d51`
  (focused 32/32). Imported ONLY that commit as
  `c32df9512075879827b75f5e465f2640c579d4c8`; Release 37-step rebuild exit0 and archived
  separately in `bin/corrected-disclosed`, executable SHA256
  `2edbf4f5177ca3f9a8169ff6ee8ba3e4e939ceaf98e9b450a0bad77ee2f0870e`.
  Verified no uncommitted production source diff; configure dirty marker is docs/harness.
  Final configure log `configure-disclosed-final.log`, build `build-disclosed.log`.
  Initial unquoted argument configure failed safely; logs retained. Compiler slot released.
- Final `baseline_disclosed` exit0,3.782s; `baseline_abort_disclosed` expectedexit1,2.015s;
  both peak0.146GiB. Nested summary is byte-identical to8ee. Top qualification failed,
  ten replay eligibility/count/PnL fields agree exactly. Abort error is identical.
- All measurement sessions consumed, no compiler or real-data process remains;
  shared heavy lock absent after final run. Root and benchmark owner notified.

## Verification and retained caveats

1. Full `g0_compare.py` completed exit0. All 54 table counts are in `comparisons/index.json`.
2. `verify` passed all35 expected measurement exits, exact Abort errors, top/nested
   fields parity, identical replay summary, every scorecard hash binding and recursive
   manifest-bound files. Three expected failed attempts retain `.pending` markers.
3. All13 cp21 historical signal/cost recipes match; no diagnostic source pin remains.
   Immutable binaries and pin patch preserve exact producing source/hash provenance.
4. Actual flags: `--membership` (cut inferred from context), `--ic-execution-delay`,
   `--min-names-per-date`, `--membership-rule`, `--terminal-returns`.
5. Frozen Python scorecard retains year-union labels although W0 masking is as-of.
   Its statistical computation and stream are unchanged for comparison; report erratum.
   Pin also retains iteration22 trial-ID prefix/prose; numeric checkpoint21/N80 and
   exact historical 26-family definitions are asserted. Overall frozen N remains570.
6. D0 panel construction changes are outside the frozen-input G0 measurement. L10's
   frozen harness does not exercise new baseline/IC as-of CLI masks. Old L10/L7 peak
   RAM and old L10 binary hash were never recorded.
7. Final report, progress, independent review and harness changes are committed separately
   from production repair imports. Root handles integration ledger/I-24 and W0 acceptance.
