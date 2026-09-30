# Integration 5 brief (root, pool-2; one integrator at a time)

Read first, binding: `.superpowers/sdd/platform-v8-20260929/integrator-rules.md`. Build tag prefix: `v8-6`.
Build script after E-1: `powershell -File scripts/research-build.ps1 -Tag <tag> -Targets "a,b" -Preset equity-dev`.
Merge by commit SHA (never by branch name). Root head at dispatch: see the dispatch message.

## Part A: fix lanes, R45, A2 (this dispatch)

1. Merge in this order, each `--no-ff` by SHA, resolving conflicts by reading both lanes' reports
   (`task-FIX-C-report.md`, `task-FIX-AB-report.md`, `task-E25-report.md`, `task-A2-report.md`):
   FIX-C `<sha>`, FIX-AB `<sha>`, R45 `<sha>`, A2 `<sha>`.
2. Build (target-scoped) every target whose sources changed: the atx-impl strategy binaries, the owning test
   targets named in the reports (`atx-impl-tests`, `atx-engine` target tests, ic / book / combine / data / strategy /
   target suites as touched). Fix compile errors in place (smallest change, lane design kept), commit as
   `fix(<area>): ... (integration of <lane>)`.
3. Run every C++ suite that ran in integration 4 part B (data, combine, book, ic, strategy, target, impl) and every
   Python suite (strategies, engine tools, impl tools, scripts). Known pre-sprint failure: `ConfigJsonNotInDiscoverDigest`
   (1) -- record, do not fix. Any other failure is fixed or reported.
4. Run the gtest filters each report names under "how root verifies". Run `tiny_world`; no golden moves.
5. Capture the spo-v2 digest pin if a pre-R6 build is not required by the test (read the test's skip reason; if it
   needs a pre-R6 build, record that and leave the skip).
6. Append the integration-log section, commit with `git add -f`. Tree must be clean at the end.

## Part B: H3 (next dispatch, after part A reports)

Merge H3 `339c07b1`; build `atx-equity-strategy-mine` and `atx-impl-strategy-mine-tests`; own build-fix pass; run
the fixture acceptance named in `task-H-3-report.md`; the two golden digests pinned at 1 and 4 workers must hold
(mismatch is a finding, never edit a pin). Log section; clean tree.

## Part C: identities on the existing 3-year role (next dispatch, after part B)

Each identity is one bounded run through `scripts/run_bounded_research.py` (never a bare executable on data), on
the 3-year role and caches that exist under `build-equity/`, hidden-data rule in force. A mismatch is a finding;
never edit a golden or a pin. Record every argv and every digest in the log:
1. NAV with every new flag absent; holdings compared against `build-equity/v7-w4-holdings` (byte identity).
2. NAV `--hold-band 0` equals 1.
3. NAV `--adv-hold-q 1e9` equals 1.
4. Composition v8 with re-rank and cap off equals the v7 composition on the same inputs.
5. H3 warm u pass: 48 of 48 cache hits, digest equal to the cold pass.
6. Field reuse step 2 (`--reuse`): expect 49 reused, 14 recomputed (report F-3 / F3 states the numbers; if the
   report says otherwise, the report's number is the expectation and you record both).
7. spo-v2 side files: identical to the pinned v7 side files; capture the spo-v2 digest pin (`lock --write` or the
   test's pin file) and commit it.
8. NAV `--label-role` equal to `--role` (same manifest, same sha) equals flag absent, byte for byte.
