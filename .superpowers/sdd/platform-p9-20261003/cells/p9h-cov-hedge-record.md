# P9-H registration and route checkpoint

Scope: `p9h-cov-hedge` only, R15-ROOT3. Executing agent GPT-6 Astra. This is not a cell disposition.

Restart docs commit `02f27798` records 54/54 frozen exes, six reference listings and CRT unchanged.
Ledger before: 134 lines, SHA-256 `b5eeab30594960c6bb52af2366b7a6d5d3ab90b0c84b13a05de0ab14a015ae61`;
N_c 63, N_hand 124, M 110, N_tot 234. No H trial or data measurement has occurred.

Registration `818e0750` changes only the registered template parent, specified ex-ante description cut,
and registration template hash, and adds the v2 manifest at `scripts/specs/p9/waves/p9h-cov-hedge.json`.
The full JSON changes and resolved NAV flags are in the commit body, also `cells/p9h-registration-commit.txt`.
Template SHA `1f56c19428b7b62de9788e37ec21bf20a78f122e411fb8fb948a0cc0347ec082`;
registration SHA `6b4c3b0d3eebdf5752e41394d316766e75a1b0df22552bc85449740ae36f2516`;
manifest SHA `a27afd5a301f13f7037f3e85c29e0765c24f1121f56913536da1c563becb781a`.
Parent `scripts/specs/p9/templates/label-terminal-gm.json`, SHA
`de44a668999ab8997c7118afb52efd93551ac2e7df93efe9118edbf719aa9a4d`, paired NAV
`build-equity/mega-nav-p9l-label-terminal-gm`. Its published leverage is 1.1828.

Read-only configuration evidence: `cells/logs/p9h-resolved-plan.txt` and `cells/logs/p9h-wave-plan.txt`.
`baseline_fields` is absent in both resolved L and H: `research_spec.py:56-57,178-182` drops identity inputs.
`wave_context.py:106-121` constructs outputs with verify=False; actual cycle verification checks surviving inputs
in `cycle/phases.py:250-263`. Thus no v13 baseline key survives this resolution. Formal preflight has not run;
FIELDS-RISK remains binding if any actual verification refuses. Actual fields remain v15, manifest pin `26fee5ce...`.
Both required NAV print flags occur once; fit is flag-absent and eval research-fast. Registered w cap 3584 MiB,
u/card 2560 MiB, other phases 1536 MiB; 512 MiB free floor; all phase time caps <=300 seconds.
No cache index, engine timing, admission wait, host budget or automatic retry option is introduced.
Manifest driver.timings records process receipts only. A fresh RAM/disk snapshot and seal provenance scan remain
required immediately before any authorized launch. Generated-cell `lock --exes --write` remains pending.

## Concrete route incompatibility, before measurement

`research_wave.py:44` exposes main(argv, executor, log), but the standard CLI has no commit-policy override.
`wave_steps.py:139-140` builds exact-file `git add`/`git commit` with no prescribed trailer or GATE-MYPY exclusion.
`wave_context.py` commit_paths returns without committing only if no code file is dirty.
`wave_stage_util.py:93-106` supports reuse of an already identical generated spec (unpinned equivalence for the
spec stage), so initial-cell pre-generation could avoid one internal commit.
However `wave_stage_cell.py:344-400` match_engine commits the calibration recipe before its bounded NAV and
then generates/commits the plain record spec at the measured final leverage within the same stage. No CLI
checkpoint exists between those operations, and pre-generating that final recipe before calibration is impossible.
No source, pin, algorithm or executor has been changed to bypass this incompatibility. Reported to PM for a route ruling.

`wave_stage_record.py:263-295` judge calls monitor/summ, which computes paired scores and writes the trial;
then it calls bundle/book and prints the decision. It has no rho-only checkpoint. A conservative reading of CELLS
step 3 requires wave `--until verify`, a bounded rho-only log, then continuation to judge/record. Under an explicit
PM ruling that 'read' means first human registered read, computation could precede a rho-first extraction, but
that interpretation has not been assumed. No paired computation or human return read has occurred for H.

G-P1 is not awarded. The proposed wave is kind rule, one registered rule, zero new candidate admissions; its
inherited roster size is still to be recorded. It is not the plan's Y-S-sized add-alpha 73-member wave. Exact
route, hand scripts, any checkpoints, external elapsed time, stage/phase sums and scoreboard --timings must be
reported factually after an authorized run; fixtures or a planning command establish no throughput result.

## Still required

PM route ruling; lock and preflight; bounded cell measurement; rho before paired read; registered driver judge
and automatic trial write; registered printed fields and predictions; Appendix A; all output/receipt hashes;
cache/seal and void checks; G-P1 integration row; completed handback. P9-M remains unreleased.

Identity-bridge flag: universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on
2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured.
Absolute Sharpe may be overstated by survivor inputs; paired evidence concerns relative changes only.

No permission refusal. No build, test suite, atx-db access, 2024+ session read, reference/lease mutation or trial
write. A read-only Python -c attempt suffered shell quote stripping (`NameError: name 'scripts' is not defined`);
the subsequently saved read-only planning helper avoids shell quotation and makes no data run.

## PM route rulings and policy repair (still before measurement)

R15-WAVE-COMMIT / R15-WAVE-READ were received after registration `818e0750`; PM explicitly accepted that
ordering. Docs `6573d5f6` committed those ledger rulings as they stood. Native commit policy source `5352c820`,
then negative-exclusion validation `5bbe1231`, changes only scripts/wave_steps.py, scripts/wave_context.py and
scripts/tests/test_wave_commit_policy.py. The exact H registration is unchanged. The earlier route incompatibility
above is historical evidence, not a permanent H block. No executor injection or hand calibration is used.
Fresh independent review and PM measurement release are pending. `cells/p9h-cov-hedge-read-plan.md` fixes the
native full-wave route, optional Git metadata environment, raw-console filter, human rho-first read and G-P1 scope.

The single named real-Git test passed after a fixture correction, and passed again after the PM-requested
negative-exclusion validation: `cells/logs/p9h-commit-policy-test-final.txt` (1 passed, 1.33 seconds).
Initial failure and first pass are retained in `p9h-commit-policy-test.txt` and `p9h-commit-policy-test-fixed.txt`.
No H data execution, return read, construction trial or disposition yet; current ledger SHA remains the initial value.

## Native attempt 1: stopped before any bounded child phase

PM launch release R15-H-LAUNCH and R15-V2-RHO committed in `70baa15a`; native policy review APPROVE
`a8f06490`. Prelaunch 2026-10-17T14:18:19Z: 2439 MiB free RAM, C: 39.8 GB free, CRT unchanged; ledger134
at the exact initial SHA, research-store/index absent (`cells/logs/p9h-prelaunch.txt`). Initial source scan had
zero files/hits (`p9h-sealsrc-before.txt`), so the invocation supplied no seal allowance.

Exact native command, with the three environment values in the committed read plan:

```text
"C:/Program Files/Python312/python.exe" -u scripts/research_cycle.py wave run scripts/specs/p9/waves/p9h-cov-hedge.json
```

Raw combined console was redirected unread to `cells/logs/p9h-native-console.txt` (SHA
`fe2159573fd1958b7ff8d120ee69e3127337a30d74b8194701bd456b3d00f543`). The metadata filter exposed stages only.
Outer exit4, external wall3.7267768 seconds (`p9h-native-wall.txt`). Completed stage receipts in
`build-equity/waves/p9h-cov-hedge/receipts`: preflight0.828s, register0s, screen0s, spec1.219s.
Run failed0.578s, `05-run.failed-1.json`. Stage time including failed attempt2.625s; this is not a completed
platform phase-wall measurement. No bounded research phase started, so no phase-wall throughput result or
scoreboard result exists. G-P1 is not evaluated/passed. Actual kind rule:67 inherited members, one rule, zero
new candidate admissions. One invocation, no retry and no spec edit after its failure.

The native spec stage ran `lock --write --exes`, then committed exactly
`scripts/specs/p9/templates/theme-cov-hedge-v1.json` in `21c78bfd85883849e761a7397bd943969b999ccd`,
SHA `a00bba84924f91469edb320d79d5382697f0f8bc71286a3a17a58d03989f1923`. Its generated code diff adds the
derived fitter flag and frozen pins; both required NAV print flags remain inherited once. Native Git commands:

```text
git add -- scripts/specs/p9/templates/theme-cov-hedge-v1.json :(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache
git commit -q -m "wave p9h-cov-hedge: rule cell theme-cov-hedge-v1.json on scripts/specs/p9/templates/label-terminal-gm.json" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -- scripts/specs/p9/templates/theme-cov-hedge-v1.json :(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache
```

Both exit0; exact one-file scope, prescribed trailer and exclusion verified, no mypy/PNG changes. Python-driver
source is the reviewed runtime at launch `70baa15a` (then generated-spec commit21c78bfd); C++ producers remain
the frozen source5f48a25d builds. Post-stop disk hashes equal frozen eval `e2b48c46...`, IC `ccd54db6...`, NAV
`255b58ac...`; no source/hygiene fix rebuilt or re-pinned an executable.

Exact failed child command:

```text
"C:/Program Files/Python312/python.exe" scripts/research_cycle.py run scripts/specs/p9/templates/theme-cov-hedge-v1.json --stop-after w --keep-verdicts --root C:/atx-wt/pool-2
```

Child exit3, stdout empty. Full diagnostic in
`build-equity/waves/p9h-cov-hedge/consoles/006-the-cell-s-inputs----stop-after-w--its-nav-runs-.log`:

```text
research_cycle: run refused: theme-cov-hedge-v1.json requires: P9-H registered by the PM in the P9 prereg (plan section 5.2, DEC-15), after root's zero-trial diagnostic scripts/specs/p9/diagnostics/al-comb-q5-style-variance.json has run and its q / q_lit are printed in the prereg (lit section 8 Q5); theme-cov-hedge-v1.json requires: lane AL-COMB's combined round reviewed and merged by root (Rulings ALCOMB-5 / ALCOMB-5a: D2's head merged into the lane, the theme-cov-hedge-v1 row in D2's K-P9-9 params_schema form, the fixture regenerated from the merged C++); theme-cov-hedge-v1.json requires: wave_kinds.COMPOSITION_FLAGS maps theme-cov-hedge-v1 to {fit: {--theme-hedge: theme-cov-hedge-v1}} (lane E2's table; AL-COMB merge request); theme-cov-hedge-v1.json requires: root sets parent to the last accepted cell whose fit is --composition ew-theme-std-v1 or ic-shrink-v1 (with or without --theme-erc, --theme-tsmom or --mom-volman) and carries neither theme-resid-v1 nor two-speed-v1, and runs the cell as an atx.research-wave/v2 kind rule wave whose rules block is [{name: theme-cov-hedge-v1, params: {}}] (Ruling ALCOMB-1)
```

Callsite `cycle/cli.py:397-399` calls `research_spec.run_refusal` before check_clean, header or phase iteration.
`research_spec.py:257` treats every nonempty `requires` entry in the template chain as open; it does not evaluate
the narrative. `wave_steps.rule_cell_doc` copies the registered template, preserving these four entries. All
remain because deleting them was outside the permitted closed registration diff. This is a driver guard stop,
not a failed scientific hypothesis, FIELDS-RISK, resource refusal, VOID, UNDEFINED or REJECTED cell.

Post-stop source-only audit scanned8 logs: exactly two OTHER hits, compact `20261003` at
`consoles/004-commit.log:1` and `005-commit.log:1`, both within the prescribed Git exclusion path above.
`cells/logs/p9h-sealsrc-stopped.txt` exit1 records every hit. No allowance was supplied or inferred; actual sources
were reported immediately for PM adjudication. No other hit. No return/paired/IC figure has been exposed.

All11 wave files are hashed in `cells/logs/p9h-stopped-files-sha256.txt`. No H downstream output directory exists.
Cache stays686 files, index absent. Ledger after remains134 lines, SHA
`b5eeab30594960c6bb52af2366b7a6d5d3ab90b0c84b13a05de0ab14a015ae61`; no new trial/id. Rho not produced,
mechanics/verdict/predictions/AppendixA not read; no automatic judge occurred. No permission refusal.
Void checklist is not asserted all-held for an unexecuted cell: the open-requires stop and two source-classification
hits are preserved. Next action requires PM ruling. P9-M remains unreleased; accepted parent is still P9-L.
