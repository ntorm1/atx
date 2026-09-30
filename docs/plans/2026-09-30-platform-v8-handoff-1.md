# platform v8 handoff 1 (2026-09-30)

For: the next parent (PM) agent finishing atx-impl v8. Read this, then `progress.md` and `integration-log.md` in the
sprint directory, then the plan. Everything below is verified from git and lane reports unless marked [lane claim]
(written by a lane that could not compile or run data).

## 0. One-paragraph state

The v8 sprint plan (`docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md`) is being executed with parallel Opus 5.5 lanes,
no TDD, no per-task review (owner directive 2026-09-29). Wave 1 (platform) is code complete and merged into the root
branch; the root build is clean and its test suites pass except one pre-existing failure; identity checks against the
accepted v7.1 outputs pass wherever they have been run. Wave 0 (the 4-year TRAIN window 2020-2023) is coded (window
constants, runbook, overlap tool, pre-registration) but **no role, field or cell has been built on the new window**.
Waves 2-4 research code is partly written (composition v8, hysteresis, ADV cap, engine solver for the optimiser,
diagnostics), none built by its lane, none run. Wave 5 tooling (era shards, mining) is design-only plus one engine
commit. Nothing has been committed to `C:/atx` main, nothing pushed. Trial count N is still 37; no read on the new window.

## 1. Where things are

| item | value |
|---|---|
| root worktree | `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, base main `7fbfc379` |
| root HEAD at handoff | `ca0e59d8` (integration 3 stopped after Part 3; tree clean) |
| sprint directory | `.superpowers/sdd/platform-v8-20260929/` (gitignored; commit with `git add -f`) |
| ledger | `progress.md` (rulings, disclosures, lane table); `integration-log.md` (per-integration merges, builds, tests, identities) |
| plan | `docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md` (tasks by id; Appendix A pre-registration) |
| pre-registration | `v8-prereg.md` (pins still `<fill>`) |
| runbook for Wave 0 | `w0-2-runbook.md` (R1-R15 exact commands) and `w0-2-runbook-open-questions.md` (34 items, all ruled in progress.md) |
| library v8 draft | `library-v8-draft.md` (blind; READY 7, NEEDS-FIELD 6, WITHDRAWN 3; rulings R2-a..h, R7-a..c in progress.md) |
| build tags used | v7-* (all), v8-1, v8-1a, v8-1b, v8-2, v8-3, v8-3a, v8-3b, v8-3c. Next free: v8-4. Receipts `build-equity/mega-<tag>-receipt.json` |
| build script | `scripts/research-build.ps1 -Tag T -Targets "a,b" -Preset equity-dev` (tracked, task E-1; presets are `equity-dev` and `equity-rel`; the plan's `equity` is wrong). Fallback `build-equity/mega-build.ps1` (untracked) |
| executables | `build-equity/bin/` Debug: atx-equity-strategy-ic, -targets, -risk; tests atx-impl-strategy-ic-tests, -target-tests, atx-impl-tests, atx-engine-*-tests |
| Python | `"C:/Program Files/Python312/python.exe"`; exes need `PATH` prepended with `C:/atx-cache/vcpkg_installed/x64-windows/debug/bin;C:/atx-cache/vcpkg_installed/x64-windows/bin` |
| host | 15.7 GB RAM, about 3.5 GB free while the atx-db session runs in `C:/atx`; 16 logical cores; C: about 70 GB free |
| disk needs | W0-2 plus B0a, B0b about 16 GiB; B0c about 7 GiB; superseded caches up to v6.1 hold about 25.6 GiB (deletion is an owner decision) |

## 2. Lane branches at the stop (all trees clean, all reports committed in the lane's own worktree)

Merged into root (integrations 1-3): B (all), A (all), EV (all), F (F-2, F-0, F-1), D (all), W0E (all), C (all),
R1 overlap tool 39bacec7, G-0 ec206a59.

Not yet merged:

| pool | branch | head | contains | state |
|---|---|---|---|---|
| 10 | feat/platform-v8-r1-20260929 | ec2dfd16 | R-1 composition `ew-theme-std-v1` (C++ + `composition_rules.py`) | done [lane claim]; needs build, identity, trial |
| 11 | feat/platform-v8-r45-20260929 | 638957ab | recipe pin for warm start 08931479; R-4 hold band 63e65514; R-5 ADV cap 638957ab | done [lane claim]; R-5 two tests missing; two rulings open (section 5) |
| 8 | feat/platform-v8-g-20260929 | fea9b6e9 | G diagnostics `book_diagnostics.py` a5c83802 (+ report) | done, 16 tests pass; runtime at real size unmeasured |
| 7 | feat/platform-v8-r6-20260929 | 52501f31 | warm-up exclusion in v7 side files 75774cd8; engine solver `solve_tracking` d695cbd8 | part 1 of 2; spo-v3 rule in atx-impl NOT written; S_prior finding (section 5) |
| 4 | feat/platform-v8-h3-20260929 | 763b0759 | mining glue part 1 11ff84c3 (role adapters lifted to `atx-engine/.../data/role_panel`, label accessor, instrument-stride racing helpers; pins re-set) | part 1; verb, fitness functor, `mined-v1`, fixture not written |
| 3 | feat/platform-v8-h1-20260929 | a30b6038 | report only (design + 3 blockers) | nothing implemented |
| 9 | feat/platform-v8-f3-20260929 | 79e5e006 | F-A `grp_ff12f49` 230b39e1 (`research_fields_v8.py`) | F-B, F-C, F-D not started; F-1 reuse-interface defect found (section 5) |
| 10 | feat/platform-v8-report-20260929 | 01158204 | E-11 seal exemption + memmap seal check 09d14980 | tasks 2-4 (v8 components, config scaffolds, scorecard template) not started |

Branch bases: R1 on lane B head 6c7cb27e; R45 and R6 on root 492c1208; G on root + EV + C-2; H3 on root + B-2; H1 on
G-0 + C-3; F3 on F-1 + C-3; REPORT on W0E head. Merging them in the order R1, R45, G, R6, F3, REPORT, H3, H1 keeps the
conflicts small (R45 and R6 both touch `strategy_nav_replay.cpp` CLI; H3 and root both re-pinned the source digests).

Pools: 2 root; 3, 4, 7, 8, 9, 10, 11 hold the branches above on dead leases (Ruling E-5). Never use pool 1 or 6.
`powershell scripts/lease-worktree.ps1 -Status` for state. Switch branches in place; never `git worktree add`.

## 3. What integration 3 verified (final; details in `integration-log.md` "integration 3")

Root HEAD at handoff: `ca0e59d8`, tree clean. Parts 1-3 done, Part 4 (identities a-g on the merged tree) NOT started.

- Merges: W0-1 880faac7, W0E 40163643, EV a27e69fe, B 6c7cb27e, C 07be7eff, A 59b89d07, F 1e44f90a, D 83375f5b
  (W0-1's `research_window.hpp` kept), R1 tool 39bacec7, G-0 ec206a59. Conflicts were import blocks and docstrings only.
- Literal seal fallbacks removed (588de0b6, ff90d14e, 16bd52ca). Grep gate leaves history comments and test fixtures.
- Source pins: `dsl_vm_sources` 31 paths, digest afbae65d, no semantics bump (5c280b64). `ic_result_sources` unchanged.
  Note: lane H3 (unmerged) re-pinned both again; recompute once after integration 4.
- Builds: v8-3 and v8-3a failed (JSON include path for two engine data tests; fixed cc62ff6f, 159d265f); v8-3b and
  v8-3c clean through `scripts/research-build.ps1` (receipts carry an Executables block). v8-3c exes: ic 3f43cef0,
  targets 6f277869.
- C++: new suites 46/46. Whole executables: data 284 passed 14 skipped; combine 215; ic 96; target 187; impl 913 passed,
  5 skipped, 1 pre-existing failure. **factory 375/377: two `FactoryOos` version-id pin tests fail; the integrator found
  no sprint change on their path. Triage this first (compare against main 7fbfc379 in a child pool).**
  `ParallelLockstepGrid` did not run: the parallel test group is not configured in build-equity (configure it or run in
  the dev build).
- Python: strategies 163, engine tools 200, impl tools 282 (2 skipped), scripts/tests 95 (3 skipped).
- tiny_world: one golden re-recorded (5cc9eb0d): only the admission digest moved because W0-1 changed `train_window_ns`;
  restoring the old bound reproduces the old golden exactly. Legitimate.
- Fixes: `--help` now names `marginal` (464e9858), else the marginal phase was always skipped.
- **Findings for the next PM:** (a) the real v7.1 `--plan-only` output (`build-equity/v8-i3-plan-v71.json`) differs from
  the recipe's static figures on 5 of 154 values (node counts of q5_eg, ftd_fail, ea_overdue, sv_flow; slots of
  res_mom_ind): the registry budgets, not code, need the real figures (lane A concern 5). (b) The marginal step in
  `research_cycle.py` passes no `--role` and its `--themes` flag has no file: fix before `run --screen` on real data.
  (c) C-1 `context_sha256` ruling still pending (integration 2 identity: admission.json differed only in two provenance
  hashes; admission.csv identical). (d) Integration 2 identities that passed pre-W0-1: B-3 (10 of 12 files identical,
  2 identical after dropping timings), B-1, D-0, A-1.

## 4. Rulings made so far (all in progress.md; the list is exhaustive for this session)

E-1 owner decisions OD-2/4/5/8 take plan defaults (IC cap 2,560 MiB / 300 s; Opus 5.5 lanes). E-2 OD-3, OD-7 tooling
only. E-3 OD-6 data asks unserved; absent-data members withdrawn. E-4 no TDD, no per-task review; reviews at wave ends
and for B-3, R-6, H-3. E-5 pools on dead leases. E-6 children never build; one integrator at a time. E-7 reports in the
child's worktree. E-8 W0-1 by merge. W0-d..W0-m runbook rulings (regsho pin, rebuild bridge and events under the new
seal, slots 8, overlap differences with identified cause disclosed not blocking, repair rule applied mechanically, integer
dsr_n until A-3, prep caps 600 s / 2,560 MiB, B0c on lo1 with delisting via F-0, sealed-row counters are metadata,
skip warehouse check, monitor without holdings/bias). E-9 one N (defect-rule count). E-10 B0c changes only returns and
warm start. E-11 seal exemption for document roots. E-12 preset names. E-13 OD-2 trigger by role dates > 1,200.
R2-a..h and R7-a..c (library draft). Lane D: timers behind `--stage-timers`.

Disclosures (hidden-data rule): runbook agent printed per-year coverage counts 2009-2026 from two atx-db manifests;
lane C opened six validation-window field manifests (field count, last session, reuse-block presence). No return, IC or
signal statistic was read. Both in progress.md.

## 5. Findings the next PM must rule on before the affected step (each needs "decision -- why -- cost if wrong")

1. **R-6 S_prior [lane claim, synthetic]:** at the registered S_prior 1.0 the tracker follows factor loadings; after 60
   decisions correlation with the aim is about .35 and gross .37 L (aim-partial: .82 / .99). S_prior 5 gives .81, 20 gives
   .96. Options: keep 1.0 and register the mechanical criterion "correlation with the aim >= .9" (the cell then almost
   surely fails on mechanics and costs a trial), or amend the registration to S_prior 20 before any read and log it as a
   pre-read amendment. Recommendation: amend to 20 with the .9 criterion; the constant was a guess, not literature.
2. **R-5 NAV in the ADV cap:** the cap uses the run's `initial_nav` for every capacity book, so at 4x the cap is still
   the $1bn cap (holdings up to about 40% of ADV). The acceptance is judged at 4x. Either accept (the cap is a $1bn rule
   and the 4x figure is a stress) or ask for a per-book desired target (larger change). Recommendation: accept, disclose.
3. **R-4 decide parity:** `decide --check-replay` cannot match a hold-band book because `--emit-holdings` (lane D
   layout) carries no band state. Fix: add `rank_set` / `desired_prev` to the holdings export (small, lane D file).
4. **F-1 reuse interface defect [lane F3 finding]:** `research_fields_price.py` lacks `HOST_HANDLES`, `producer_group`,
   `field_spec`, `reuse_inputs`, `entry_inputs` and records `producer_code` not `producer`; any `--reuse` build that
   includes an F-1 field raises `AttributeError` after creating the output directory. About 20 lines; spelled out in
   task-F-3-report.md. Must be fixed before fields v10.
5. **Fields v10 size:** v9 + F-1 (6) + F3 (4) = 73 rows > the old 64 cap; B-2 lifts the cap to 1,024 and is merged.
   For the R-2 cell alone, v9 + `grp_ff12f49` (64) suffices.
6. **H-1 blockers:** the IC runner refuses frozen weights on another role (`strategy_ic_admission.cpp:491-494`) and a
   validation role starting before TRAIN end (`strategy_ic_runner.cpp:602-603`); `backtest_integrity.window_of` rejects
   sessions before 2020; the fitter masks decisions to TRAIN. Era shards therefore mean pooled re-admission (Python) or a
   runner change. Design in task-H-1-report.md.
7. **G diagnostics runtime:** G-1b plus G-2b may exceed 180 s at real size; use `--only` splits or the prep caps ruling.
8. **Two definitions of N are unified (E-9, G-0)** but check `test_dsr_n_equals_trial_counts_with_defect_and_rerun_lines`
   passes at root after all merges.

## 6. Next steps, in order (this is the remaining plan; the goal prompt names it)

1. Finish integration 3 if its log says parts were skipped: identities a-g (W0-1 NAV identity, C-1 provenance ruling,
   C-3 reuse identity, B-2 workers 4 vs 12 under 2,560 MiB, D-1 timers, F-0 lo1 role identity, E-1 receipt). Fold
   `ledger-pending*.md` into progress.md if still present.
2. Integration 4: merge R1, R45, G, R6, F3, REPORT, H3, H1 (that order). Fix the F-1 reuse defect (item 5.4). Build
   (tag v8-4...), run every new suite (CompositionV8.*, GroupRerank.*, HoldBand.*, AdvHold.*, BookTargetShaping.*,
   StrategyLive.*, TargetTracking.*, NavV7Hook.*, StrategyIcRunner.* incl. both pin tripwires), Python sets, tiny_world.
   Identities on the 3-year role: R-1 identity (weights file + `theme_standardise {rerank:false}`; combined and planned
   targets byte-identical), R-4 `--hold-band 0`, R-5 `--adv-hold-q 1e9`, H3 warm u pass 48/48 hits and SHA-equal outputs.
3. **Wave 1 review (owner rule: reviews at logical break points):** one read-only adversarial review lane over the whole
   root diff since ef11f462, on the most capable model, with the ledger's deferred minors. Fix I and M findings with one
   fix lane, one scoped re-review.
4. **Wave 0 data build** (root only, runbook R1-R15; every command through `scripts/run_bounded_research.py`, clean tree):
   re-project `--end 2024-01-01`; base role 2020-2023 (start 2018-06-01); repair; r4 bridge `--seal 2024-01-01`;
   fundamental-events-v3; roles lo1 and lo3 (F-0 gives lo1 the delisting options); `--plan-only` (expect about 1,952 MiB;
   caps 2,560 / 300); fields v9 on both roles (prep caps 600 s / 2,560 MiB); cold v7.1 u pass on lo1; overlap reports
   with `compare_window_overlap.py` (field first, then signal, then daily_ic, `--before 2022-09-30`), ruling W0-a as
   extended. Fill the pins in `v8-prereg.md`; write the `protocol` ledger line (`research_cycle.py ledger-protocol`,
   chained). Commit before every run.
5. **W0-4 cells** B0a (lo1), B0b (lo3), B0c (winner + `--delisting-returns` role for labels/NAV + `--warm-start-sessions 60`,
   Ruling E-10) through `research_cycle.py run scripts/specs/v8/base-*.json` (spec drafts in the runbook section 4;
   `dsr_n: "ledger+1"`; `fit.work_dir` and card `--work-dir` must be set). Year tables and Appendix A block on each.
   Re-run ledgered v7 cells reproducible from specs on the 4-year role (0 trials) for OD-4 variance.
6. **Diagnostics on B0c** (`book_diagnostics.py`, needs the NAV cell with `--emit-holdings`, the risk verb run with
   `--book-weights`, and the three lagged NAV cells via `lag-combined`; none ledgered). Commit `diagnostics-v8.json`.
7. **E-2 Release A/B** on a quiet host: u, w, NAV of B0c under `equity-rel`; adopt only on byte identity and >= 25% CPU
   saving; the fixture must agree under both builds.
8. **Research cells in order** (each: identity first, then one cell, parent = last accepted, acceptance rule from the
   plan, `nav_summ --protocol v8`): R-1 composition v8 (N 41); R-2 library v8.0 via `add-alpha` for the 7 READY members
   plus the 8 `_f49` re-screens (needs `grp_ff12f49` in fields v9+1), `run --screen`, then `run` (N 42); R-3 aim gain
   spec only (N 43); R-4 hold band .10 (N 44); R-5 ADV cap .10 with `--capacity-curve` (N 45); R-6 spo-v3 after part 2
   is written (risk model on the 4-year role first; identities; tripwire) (N 46); R-7 library v8.1 after F3 finishes
   F-B..F-D and fields v11 (N 47); optional R-8, R-9.
9. **V8-F** cumulative test `nav_summ.py --bundle B0c V8-F`; freeze gate as registered (S2 net >= 1.0, dSR > 0 with
   p < .10, cell-count DSR >= .95). If unmet, the scorecard says so and names OD-3.
10. **Wave 5 tooling** (no reads): finish H-3 parts 2-3 with the fixture acceptance; H-1 per its design after ruling on
    item 5.6; H-2 AuditExact measurement on E3 and the fixture.
11. **Wave 6:** REPORT lane tasks 2-4, scorecard `docs/plans/<date>-mega-alpha-scorecard-v8.md`, pitch config and
    render (0 unavailable blocks; re-render the v7 pitch too, E-4 step 3), handoff 2, merge command for the owner.

## 7. Standing rules (unchanged from v7 plus this sprint)

Root alone builds and runs real data; children never build, never run data, never spawn agents. Never build, switch or
commit in `C:/atx`; never touch `atx-db/` or its processes. Hidden data: nothing dated 2024-01-01 or later is opened by
any tool or agent (two disclosures so far; the rule is enforced in the tools now). Every ruling before the measurement
it could bias, as "decision -- why -- cost if wrong". Every cell pre-registered. Appendix A block on every result, now
with "plus 8 re-screens". Commit the sprint dir with `git add -f` before every real-data run. Commit trailer for child
lanes: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; for the PM: `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
No pushes, warehouse writes or broker actions. Shell hook breaks heredocs: use Write/Edit. Lane briefs and rules:
`lane-rules.md`, `integrator-rules.md`, `task-<ID>-brief.md`.

## 8. Pitfalls met this session

Permission system refused a `git merge` inside an integrator agent once (the PM ran the command; the agent resolved the
conflicts). Cold IC pass on an empty cache under host memory pressure exceeded 300 s (field hashing 100 s against 2 s);
a second run on the populated cache finished in 176 s. Windows 260-character path limit broke the fixture test under
pytest's tmp_path (fixed: short temp root). Lanes that cannot compile leave `/W4 /WX` slips: budget one build-fix pass
per merged C++ lane. Lanes re-pin `dsl_vm_sources` / `ic_result_sources` independently (root, B-3, B-2, H3): recompute
once after all merges. Lane D and lane H3 each carry their own copy of `research_window.hpp`: W0-1's is authoritative.
