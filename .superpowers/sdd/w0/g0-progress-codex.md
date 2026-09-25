# G0 truth-delta execution, 2026-09-25

- Owner: Codex G0 evidence subagent; branch `feat/w0-g0-codex-20260925`, pool-3.
- Frozen source base: `bc5cc646b46f6a7c23a60e87d28dfa9b972671ec`.
- Lease run/heartbeat: `aes-w0-g0-codex-20260925` (keeper published successfully;
  the expected cold `dev` configure failed after lease publication).
- Output root: `C:/atx-wt/g0-data/bc5cc646_20260925`.
- Root authorized serial heavy G0 measurements, 2019 development reuse and output root.
- No data at or after 2020-01-01 and no warehouse/database opened. Inputs read only.

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

## Current execution (updated after first three measurements)

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
- L9 RUNNING from archive, tool session 79174; receipt/logs `logs/l9.*`.
- `.heavy-run.lock` is acquired atomically per run with PID/start/token and only
  removed for a matching owner. This coordination file is the sole authorized C:/atx write.
- Root requires importing tested/reviewed replay and DSL-internal D12 repairs before
  corrected baseline and cp21, to avoid rerunning the 13 expensive cells. Replay and
  membership build/tests remain in their own lanes. Root's integration head may be
  merged when both pass. Archive corrected unpinned binary and exact SHA, run native
  baseline, then apply 26-family diagnostic pin and rebuild only atx-impl.
- G0 cp21 build overlap grant: <=3 compiler workers total, >4 GiB free and verified
  per-worktree isolated deps. Environment cap 1; no other shared deps builds.

## Known caveats and next actions

1. I0b pins identified replay to Abort. Frozen baseline should still fail at 2013-04-12
   security 150340; measure this accurately. Root's separate replay agent is fixing the
   consumer wiring; corrected code will require a new receipt after merge.
2. Run unpinned baseline/l9/l10/l7 first. L9 uses holdout off; preserves train/validation
   boundaries and all seeds/settings. No tuning.
3. Preserve/hash unpinned binaries, then pin only cp21 26 families, retained 22,
   checkpoint 21 in local source. Save the exact diff as an evidence patch; never merge
   the pin. Rebuild atx-impl, run 13 baseline + IC cells serially and frozen scorecard.
4. Actual flags: `--membership` (cut inferred from context), `--ic-execution-delay`,
   `--min-names-per-date`, `--membership-rule`, `--terminal-returns`.
5. Frozen Python scorecard retains year-union labels although W0 masking is as-of.
   Its statistical computation and stream are unchanged for comparison; report erratum.
6. D0 panel construction changes are outside the frozen-input G0 measurement. L10's
   frozen harness does not exercise new baseline/IC as-of CLI masks. Old L10/L7 peak
   RAM and old L10 binary hash were never recorded.
7. Generate complete delta tables, report actual results and gaps, restore the two pin
   source files, and commit report + scripts only. Root handles integration ledger/I-24.
