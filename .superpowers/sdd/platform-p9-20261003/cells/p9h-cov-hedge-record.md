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
