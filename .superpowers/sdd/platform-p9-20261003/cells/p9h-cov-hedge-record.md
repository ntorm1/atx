# P9-H record: ACCEPTED

Final disposition: native pm7-34 ACCEPTED, paired S2 net dSR0.02554011056222616 >0 and all six mechanics checks
PASS. Exactly one construction trial is ledgered. The completed result and final accounting follow the preserved
chronological checkpoints below. Stop before P9-M, which requires the PM's separate release.

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

## Attempt2 preparation under R15-H-READY / R15-H-RESTART / R15-SEAL-C

PM closed all four textual prerequisites on `cells/p9h-prerequisite-closure.md` at `d9d5c132`. Metadata-only
clones are committed in `708ab6ce`: ready template changes only requires to []; attempt2 manifest changes only
out_dir, rule_cell.template, its SHA and rule_cell.name. Exact closed JSON delta and both full hashes are in
`cells/p9h-attempt2-registration-commit.txt`. The original registration, rule, template, generated21c78bfd spec
and all eleven stopped files remain byte-identical; `cells/logs/p9h-attempt2-preservation.txt` records each hash.
Trial ledger remains134 / b5eeab30594960c6bb52af2366b7a6d5d3ab90b0c84b13a05de0ab14a015ae61.

The sprint checker now recognizes source C only at the exact compact20261003 hit span in the standalone,
unquoted mandated Git exclusion argv token after --, on line1 of a numbered *-commit.log Git add/commit header.
Other sources/hits remain OTHER; A/B predicates are unchanged. Scanner SHA-256:
`9672f889d24185a8667bdb2e5fc0cd7cdb81f59fd7310e6ad35067af74b17cc8`.
Focused `python .superpowers/sdd/platform-p9-20261003/tools/sealsrc.py --controls`: 21 PASS, recorded in
`cells/logs/p9h-sealc-controls-final.txt`. Actual `--manifest scripts/specs/p9/waves/p9h-cov-hedge.json` scan:
8 logs, exactly2 class-C hits at preserved consoles004/005 line1, OTHER=0; final log
`cells/logs/p9h-sealc-attempt1-rescan-final.txt`. Earlier19-control logs remain separately preserved.
No production algorithm, frozen binary, guard enforcement, data or statistical computation changed.

Read-only `research_cycle.py wave plan scripts/specs/p9/waves/p9h-cov-hedge-attempt2.json` exited0; output in
`cells/logs/p9h-attempt2-wave-plan.txt`. No attempt2 state or generated spec exists at this checkpoint.
The committed read plan and filter now target fresh attempt2 state, with distinct unread console/wall/rho logs
and explicit scoreboard result path. Mechanics, then existing-bundle rho provenance, then paired read boundary
is unchanged. G-P1 zero-spec-edits-on-retry is UNMET; a later success cannot award G-P1. This preparation is not
a retry execution or scientific result. Await PM verification/release before launch; no next cell released.

## Attempt2 actual seal stop and prepared continuation

R15-H-LAUNCH2 committed `d5301d33`; attempt2 ran once, external131.2085962s, stopping at verify after completed
run67.828s and match59.406s. No judge/trial/paired read. Full stopped record, per-hit metadata sources, native reuse
proof and R15-SEAL-D / R15-H-VERIFY-RESUME preparation are in `cells/p9h-attempt2-seal-stop.md`.
Source checker37 controls PASS; actual29-log rescan A4/B2/C6/D16/OTHER0. All32 wave files preserved;197 additional
output/run files hashed; ledger134 unchanged. Frozen54/54 and CRT match, cache686/no index. Same-state resume
is prepared but not launched pending PM verification; first six stages must reuse. G-P1 remains unawarded.

## Completion read boundary: mechanics and rho logged before paired figures

R15-H-RESUME-GO committed `30d74551`; continuation exited0 after reusing all first six stages. Final source scan
40 logs: A7/B4/C6/D28, OTHER0. All32 stopped wave files and197 output/run files remain byte-identical; frozen
producers/CRT and six references match, cache686/no index. `cells/logs/p9h-mechanics-first.json` records all six
v8-mech checks PASS before this rho read; cell/parent NAV and eval pins match, C-13 binding verified.

First registered paired read: existing bundle SHA
`2fa545913cd34328d2d73de6775bd57eb1e80c851f247e9322d1976f6b51c951`, eval pin e2b48c46...d962c,
exact L/H pair validated against judge receipt. Sessions1005, rho0.8653199607726447. Neither one_minus_rho nor
degenerate is supplied; numeric stable difference is `not supplied by bundle`; threshold flag FALSE inferred
from documented nondegenerate encoding under R15-V2-RHO, not a calculated difference. Log
`cells/logs/p9h-attempt2-rho-first.txt` saved before any other paired figure. No rho/statistical rerun.

## Final registered result and prints

Native `08-judge.json` decides only paired S2 net dSR>0 AND mechanics, pm7-34. ACCEPTED: cell net SR
1.8750221368920863, parent1.8494820263298601, dSR0.02554011056222616. Printed paired Memmel SE
0.2615496420444564, t0.09764918950982554; LW two-sided p0.9322 (one-sided0.4744), SE0.27003020920357984,
95% CI[-0.5241771391575851,0.5752827923080717]; CBB95% CI[-0.49947667984762295,0.5797562683890513].
These uncertainty lines do not change the registered point-dSR decision. Nondegenerate encoding as above;
C.11 fallback is not needed. Engine eval producer is frozen e2b48c46...d962c; no Python statistic substitute.

| Mechanics check | Value | Fixed limit | Result |
|---|---:|---|---|
| gross_all_rows | 0.986241893721804 | [0.9,1.05] | PASS |
| abs_net_all_rows | 0.0050699651270206605 | <=0.02 | PASS |
| tau_mean | 0.02706227150226712 | <=0.2 | PASS |
| tau_p95 | 0.03234806696272993 | <=0.3 | PASS |
| max_return_identity_error | 3.7816971776294395e-16 | <=1e-9 | PASS |
| max_cash_book_relative_error | 1.2564514679600198e-13 | <=1e-9 | PASS |

The native PM6-6 engine calibrated one NAV process to final L1.1674 (parent1.1828). Its plain recipe of record is
`scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json`, commit `bd837403`, SHA-256
`1d8eecaa077844d093b05a861db18787ead3223317154c25094cfc98f029e88f`; paired NAV
`build-equity/mega-nav-p9-theme-cov-hedge-gm`. Gross-matching and all printed fields use the registered S2
`modeled-1bn-stale5-v1+swap-fin-v1` scenario. Native generated commits each contain only their named one spec
and the required trailer/exclusion; no changes on resume. The original failed attempts and all receipts remain.

| Registered printed field | H | L parent |
|---|---:|---:|
| C++ ann_vol | 0.030344611264587394 | 0.030569717335626075 |
| C++ ann_mean_gross, gross of costs and financing | 0.07061758625960741 | 0.07067817348886382 |
| turnover per unit gross | 0.027439791063976805 | 0.028527696266223746 |
| C++ cost_bps_per_traded_dollar, capacity x1 | 12.490795824692793 | 12.657953278624722 |
| C++ net Sharpe at capacity x4 | 1.759566530994928 | 1.6994264697927697 |

Registered predictions: realised-vol ratio H/L0.9926363051196309 <1, MET; gross-alpha ratio
0.9991427731325576 in[.90,1.10], MET. These ratios divide the two existing C++ prints; no new return statistic.
Expected dSR[+.05,+.15] print-only estimate was NOT MET (actual+.0255401); ex-ante prediction stays CUT.
Native printed criteria all met and decide nothing. `cells/logs/p9h-registered-prints.json` SHA
`d3c71d24c8b6b3598e04b9829846297a20bf1123b114be219487ce109c816633` contains exact selections/provenance.

IC's registered hedge-ratio print is copied without recomputation: all12 theme names and all55 block h-vectors
in `/roles/0/composition_hedge_trace` from `mega-p9w-train-theme-cov-hedge-1/summary.json` (source SHA
`1807c2eb01f300624b03ba8def18e266176efc93a33c2cfb8e83ac9ea068acab`). Complete artifact
`cells/logs/p9h-hedge-traces.json`, SHA `da37c75bf1ad98be85a812447e00443e8283f3aec4956df098aa5678b49ae5d6`.
No unrelated candidate IC block was printed. `cells/tools/p9h_complete_reads.py` performs only allowed-field
selection, registered ratios and ledger accounting after the rho log; it launches no engine or process.

## Trial accounting, void checks and preservation

Exactly one native-judge construction trial:94a77d4af05ac735, count1, origin prior, H's gm NAV above.
Ledger before134 lines / `b5eeab30594960c6bb52af2366b7a6d5d3ab90b0c84b13a05de0ab14a015ae61`;
after135 lines / `7b87d61ccb3285e9ea94aefb52222e610f74287b5559988339eccf09aed07e00`.
The first134 complete raw lines retain the before hash. No manual trial write, new admission or second H trial.
Evidence `cells/logs/p9h-ledger-completion.json` and `p9h-accounting.txt`; native recorded chain head
`1568e4a9ef832de9fc76493579fc0119f4c8898ee7a1638395746d8ca50c1cd2` is distinct from the whole-file SHA.

Appendix A: TRAIN construction cells64; historical admission trials61, P9 additions0 (v8:12, X hand-written25,
X-4 re-screens9, Y hand-written15, P9 hand-written0); K_a40 historical hand-written strings; P9 mined campaigns0,
inherited v9-mine-c1 M110; N_hand125, N_tot235. Window research-window-v2(2020-2023); hidden2024+ unread in
this sprint; validation reads before v8:2(2023-2024); history reads0; 2025+ never read. Eight window reruns and
one protocol line add no trials. Native n_before63 -> n_after64. P9-S/W remain lapsed; no later-cell work.

Void checklist **all held**: (1) registered locks/re-pin/ALD-DIAG preconditions and explicit requires closure
preceded data execution; (2) accepted L parent, rule constants, fields v15 pin, inputs, eval/fit modes and
registered flags agree, with only the explicitly ruled metadata clones and PM6-6 calibration; (3) all raw
statistics withheld until source/pin gates and mechanics passed, rho logged before paired figures;
(4) no session at/after2024-01-01 opened, every final seal hit has A/B/C/D provenance, OTHER0;
(5) frozen builds unchanged, Release27/27 and Debug27/27 plus final eval/IC/NAV disk checks match;
(6) CRT version/hash matches P9-B0; (7) no cache-index flag or index.sqlite, cache remains686 files.
`p9h-final-receipt-flags.json` verifies all completed receipt outcomes/flags: fit flag-absent, no engine timing,
both required NAV flags exactly once. The source classifier examined only console text, never PNG contents.
All six reference file listings still match. Initial guard/seal stops are driver checkpoints, not scientific
rejections or voids, and are retained in this record. No permission refusal.

Identity-bridge flag: universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on
2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured.

Result `build-equity/waves/p9h-cov-hedge-attempt2/wave-result.json` SHA
`e93f09532bdc4b12ffc3d2e51f69edef4e15454997cde62dd56ca327663cbf67`; bundle SHA2fa54591...51c951.
The original32 wave files and197 run/output files remain unchanged after resume. Additional final64 wave,
monitor and cycle files are fully hashed in `cells/logs/p9h-final-wave-monitor-cycle-sha256.txt`.
The native result/log/ledger-head copies are retained under `cells/waves/p9h-cov-hedge/`.

## G-P1 factual timing and completion

Exact native commands/environment and stop/resume boundaries are in the committed read plan and stop record.
No hand script orchestrated a research stage; extraction helpers only filter existing outputs. One native attempt1
stopped before any data, one fresh attempt2 performed the registered computation, and one same-state continuation
reused all first six successful stages then ran verify/judge/record. No fit/w/card/NAV rerun. The registered
kind rule has67 inherited members, one rule and zero new admissions; it is not the73-member add-alpha example.

| Timing scope | Seconds |
|---|---:|
| Attempt1 native invocation, stopped before data | 3.7267768 |
| Attempt2 initial native invocation, seal checkpoint | 131.2085962 |
| Attempt2 verification/judge/record continuation | 24.3026102 |
| Attempt2 summed invocation walls | 155.5112064 |
| All three invocation walls summed | 159.2379832 |
| Attempt2 start-to-final completion, including PM checkpoint | 680.5601286 |
| Fresh core phase receipt sum, fit/card/w/NAV/monitor/summ | 132.610 |
| Fresh phase sum including bounded readers/bundle | 135.596 |
| Inherited u receipt (prior invocation, separately reused) | 51.578 |
| Full phase receipt sum including inherited u | 187.174 |
| Native recorded process rows sum, overlapping phase scope | 150.267 |

Attempt2 continuation start14:51:45.1249547Z/end14:52:09.5042300Z; free RAM4133 MiB/C:39.0 GB, no RAM wait.
First attempt2 start14:40:48.9441014Z. All bounded fresh phases completed within their registered caps/floor.
The phase sums are below plan0.4's300s target and600s hard threshold. The680.560s elapsed checkpoint span is a
different metric and is not labeled a phase-wall failure or an uninterrupted run.

Registered `scoreboard --results build-equity/waves/p9h-cov-hedge-attempt2/wave-result.json --ledger
build-equity/trials.jsonl --timings` ran once. Raw output withheld; timing-only extract
`cells/logs/p9h-scoreboard-timings.txt`. Its displayed phase-table total152.4 follows the documented
`wave_scoreboard.py:161-164` stage_seconds override (exact152.405 through judge), not the sum of printed phase
columns. Record receipt0.344 and failed verify0.297 are separately retained; the result is built before its
record receipt exists. This is a timing-label/total-semantics caveat, not an assertion of incorrect computation.
Git processes2.422s also appear as a table column; receipt phase sum187.174 above excludes this process column.
Full metadata/process rows in `p9h-timing-summary.json`, with source receipt details in `p9h-native-timing-details.json`.

**G-P1 is unawarded.** Zero-spec-edits-on-retry is UNMET under R15-H-RESTART, and interrupted/restarted execution
and67-member rule scope remain disclosed. Timing success does not repair either limitation.

**P9-H complete; STOP before P9-M.** Native next parent is the accepted H plain gm spec/hash and NAV above.
M requires the PM's separate release. No open question, extra statistic, permission refusal or next-cell action.
