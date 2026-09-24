# Tier-1 parity handoff — 2026-09-24 evening UTC

Latest capacity observation: window4/session15674 is terminal at22:09UTC,
19samples over180seconds, physical4.530..5.848GiB and commit9.161..11.580GiB.
No qualifying6/8GiB interval; no additional runtime or warehouse write.
This continuation was a verified wait. Next actions below remain unchanged.

Continue on `feat/tier1-parity`. Initial HEAD was2e0d738f; the checkout was on
main, with both branch pointers identical. Root switched to the requested
feature branch without changing files or merging. Preserve other-session
risk-test/alpha-swarm files and **stash@{0}**; never apply, drop or rewrite them.
All work this turn used Codex agents, with no external model spending.

The full production outcome is still incomplete. Snapshot remains2026-09-20.
Do not advance it or describe empty jobs/fixtures as a populated provider.

## Delivered and measured

- be4c2614 reconciles the newer head6 receipt with actual Git content and live
  source ledgers. It records exact commands and cache fingerprints. Existing
  whole-branch review findings are already repaired; no repeat review is due.
- ED1,44fb6392, adds the generic quarterly EPS reader and production docs.
  Numeric output uses IQ2 selected-leaf qualification. Six focused checks,
  two corrected negative/zero-base cases and scopedRuff passed. Parent's one
  independent review is clean. Source or market identity is never invented.
- Live `cvx-eps-desk1` returned `schema_prerequisite_missing`, exit2: missing
  `selected_input_refs_hash/json`. It stopped at schema preflight, no numeric
  rows. Peak0.642921GiB under the lower1GiB guard; DuckDB256MB/one thread.
- PG1,6357e9f9, repairs two test integration failures: release a caller-owned temp
  table before connection recycling; expect all current pending migrations
  while retaining legacy-row/PIT/constraint assertions. Parent review clean.
  The isolated chunk case passed (peak0.707485GiB); Ruff passed. The populated
  0314 upgrade case is still pending. This is a reviewed repair checkpoint,
  not a closed runtime gate.

## Actual runtime/source state

Fresh read-only `pipeline-status-sep24-evening2` confirms schema0322,
pending0323..0325, no lower migration gaps, terminal failed source predecessors:

- CompanyFacts:513cfbbc-096a-4186-9666-b6cc5170c4ad (archive16 already recovered).
- Submissions:04cf947d-53bb-49b7-a276-b3c74a2a52c8 (all forms/all CIKs/history,
  batch50, same retained archive required).

No migration, source write, recovery, quality run or materialization occurred
this turn. Warehouse stayed12,883,341,312bytes with unchanged modification time.
The earlier measured inventory remains the latest broad coverage receipt:
47,941,000 raw facts;31,959,271 bars; canonical downstream surfaces empty;
provider/annual item/quality readiness unqualified. Current schema-only reader
does not remeasure those counts. Historical listing/adjustment/vintage evidence
and all release thresholds remain open.

`schema-headroom-window3` is terminal `no_sustained_window`,19samples over
three minutes. Physical free4.002..5.717GiB; commit free10.704..12.610GiB; no
qualifying6/8GiB window. Later samples also stayed below6GiB physical. Root
asked whether the user can free other workloads; no response is required to
continue independent work. Never terminate other sessions or lower thresholds.

## Next action, in order

1. Observe the unchanged6GiB physical/8GiB commit floor for120seconds using
   a fresh receipt. Run only the repaired populated0314 upgrade selector via
   `verify_pit_gate_repair.py upgrade` under1.5GiB. Exact command is in
   `.superpowers/sdd/tier1-parity/resume-reconciliation-2026-09-24.md`.
   Head6's110passes/1defaultslow skip remain valid on their unchanged source;
   its two failures are the PG1 cases. Do not rerun all113 checks unnecessarily.
2. After acceptance, check free disk and execute governed migrate0325 with
   backup-keep100 under2GiB,1GB/one thread. Preserve every backup.
3. Full CompanyFacts archive17 from the actual UUID above; full submissions
   resume; scoped CVX earnings source; full-universe run5 from statement_points
   --force with16sequential reconciliation shards and no CIK filter.
4. Execute live EPS/desk/feature readouts, research manifests/evaluations,
   item/provider/all-quality measurements. Publish only if unchanged coverage,
   PIT, survivorship and freshness gates pass; verify manifest/hashes.
5. Full non-slow release gate remains pending. Existing branch review stands;
   review new repairs once, rereview only Critical. Ask before merging to main.

Use the updated production-resume-sequence for all commands and fresh artifact
names. The canonical CVX consumer is now `read_quarterly_eps_growth.py`; frozen
v1/v2 SQL remains diagnostic history. No numeric CVX production acceptance or
alpha result has been claimed. Keep the goal active and preserve stash@{0}.
