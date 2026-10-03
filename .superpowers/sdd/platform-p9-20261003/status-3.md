# P9 sprint: PM handoff 3 (owner stop, 2026-10-03 evening)

For the next parent (PM) agent. Read this, then the last ~80 lines of `progress.md` (the ledger; every line with
"Ruling" is binding), then `wave2-carry.md`. Do not re-dispatch anything marked complete here or in the ledger.

Plan: `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md` (COV + SQL amendments applied, tagged
"(amended: COV)" / "(amended: SQL)"). Sprint dir (this dir): `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`.
Root tree `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`, head `e4486bdc` (docs) on wave-1 code head `b52a5de7`.

## How the owner wants it run

- PM only: write ledger / briefs / rulings; never build, run real data or edit code. Opus 5.5 subagents
  (`model: opus`) for every lane, reviewer and root step; one fresh root agent per step group; hand artifacts over as
  files. No TDD: implement first, then the tests the briefs name.
- Priority (owner, this session): real engine + implementation progress with many parallel subagents; move from JSON
  artifacts and repeated Python scripts to SQLite and core C++.
- Every decision goes in the ledger as `Ruling: decision -- why -- cost if wrong` before the measurement it could bias.
  Stop only for irreversible / destructive or security-sensitive actions, pushes / merges outside the worktrees, or a
  plan so broken every path is a guess.
- Standing: never build / switch / commit in `C:/atx`; never touch `atx-db/` (another session runs jobs there); no
  push; leave untracked `docs/plans/2026-10-02-x5-equity-curve.png`; never `git worktree prune`; open nothing dated
  >= 2024-01-01; never edit an expected hash; subagent commit trailer
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; replies to the owner terse ("caveman" mode).
- Machine: 1.5-3 GB free RAM. Builds only through `scripts/research-build.ps1` (memory gate; one at a time).
  PY-HYG: pytest with explicit paths only, agents kill their own children.
- Reusable dispatch contracts: `wave2-lane-dispatch.md` (lanes), `wave2-review-dispatch.md` (reviewers),
  `root-wave1-merge-brief.md` (root wave-1 recipe).

## Done

- Phase 0 (R0-11..R0-14): complete. Y-F0 deployable; X-10 / Y-1 verdicts re-derived and match.
- Wave 1: all eight lanes merged and verified per slot (E1, T1, A1, A2, S1, B1, D1, C1). Reports:
  `root-wave1-merge-report.md` sections M1a, M1b, M1c. Debug + Release NAV identity for C1 hold. Release tree exists
  (`build-equity-rel`). Trial ledger unchanged: 133 lines, sha `27e40f9f`.
- Plan amendments (COV, SQL) applied; `wave2-carry.md` rewritten for wave 2.
- Lane SQL1 (SQLite foundation): complete, review APPROVE + re-review APPROVE at `1a5b051d` (pool-21). Not merged.

## Stopped mid-flight (each has a handoff file; resume, do not restart)

| item | pool / branch | head | state | handoff file (in that pool's sprint dir unless noted) |
|---|---|---|---|---|
| root M1d (wave-1 gates) | pool-2 | `e4486bdc` | block 1 partial, block 2 partial, 3-6 not started | `handoff-root-m1d.md` (root sprint dir) |
| SQL2 catalog + verify CLI | pool-23 `feat/p9-sql2-20261003` | `47349322` | ~85%: 21/21 gtests, 16/16 identity pytests; 2 pytests unrun; no report | `handoff-SQL2.md` |
| T2 test tooling | pool-22 `feat/p9-t2-20261003` | `2e9d6813` | lane DONE_WITH_CONCERNS; review PARTIAL (no verdict) | `task-T2-report.md`, `task-T2-review.md` (uncommitted) |
| E2 cycle runner | pool-17 `feat/p9-e2-20261003` | `a304ed2d` | ~85%: deliverables + required items committed; full suite not re-run; no report | `handoff-E2.md` |
| AL-SIG sources | pool-13 `feat/p9-al-sig-20261003` | `80074679` | ~85%: C++ uncompiled; pytest 52p; no report | `handoff-AL-SIG.md` |
| AL-COMB | pool-19 `feat/p9-alcomb-20261003` | `dc02090a` | ~75%: C++ uncompiled; registration files, templates, Q5 spec, report missing | `handoff-AL-COMB.md` |
| D2 composition | pool-20 `feat/p9-d2-20261003` | `a17319fb` | ~70%: one WIP commit to split; `--engine-fit` untested; one pytest red (old header) | `handoff-D2.md` |
| S2 IC engine | pool-18 `feat/p9-s2-20261003` | `eff81c86` | ~65%: all source written; no tests; two digest re-pins pending | `handoff-S2.md` |
| A3 engine fields | pool-12 `feat/p9-a3-20261003` | `83d06664` | ~60%: M1a-RED fix committed; ported fields uncompiled; leak / identity tests missing | `handoff-A3.md` |
| B2 ledger + stats | pool-14 `feat/p9-b2-20261003` | `b0b0fbf2` | ~45%: ledger lib + chain head coded; no exe, CMake, gtests | `handoff-B2.md` |
| COV | pool-24 `feat/p9-cov-20261003` | `16233715` | ~30%: `.atxcov` container + 18 gtests done; recipe half; atx-impl does not link | `handoff-COV.md` |
| C2 book calibration | pool-15 `feat/p9-c2-20261003` | `0c27cf79` | ~25%: no code; M1c-RED root cause found; designs in handoff | `handoff-C2.md` |

Bases: SQL1 `20443022`; SQL2 `51ce19f2` (= `1239a5ff` + SQL1, then SQL1 fix head merged, `afea225d`); C2 and COV
`ad406718`; all other wave-2 lanes `1239a5ff` (wave-1 slots 1-7; C1 not in base). Leases for pools 21-24 are held.
All agents reported no process left running. WIP commits are titled `wip(...)`; several do not build.

## Next steps, in order

1. Root M1d: resume from `handoff-root-m1d.md`. Left: all pytest suites + whole `atx-impl-tests`; canary goldens
   Debug + Release (both entries null: first record, no `--repin`); Release IC adoption (G-P3 IC half); P9-B0;
   scoreboard + timings; G-P ticks. Gate: failure set within the five named known-reds (M1a-RED x3, M1c-RED x2
   Release only). Next build tag `p9-1l`.
2. Before P9-B0, rule the open problem below (Y-1 vol-target rows under sqrt).
3. Whole-wave-1 review (one reviewer, most capable model) once M1d passes.
4. Resume each wave-2 lane with a fresh agent: give it `wave2-lane-dispatch.md`, its brief, the carry and its handoff
   file. Answer its open questions first (below). Then one reviewer per lane (`wave2-review-dispatch.md`), fix rounds,
   scoped re-reviews.
5. T2: dispatch a fresh reviewer to finish the review (points 2, 3/4 remainder, 5, full-suite run).
6. Wave-2 merges by root in carry slot order (E2, A3, S2, B2, D2, C2, AL-COMB after rebase on D2, AL-SIG after A3,
   COV, SQL1 with the N1 one-line fix, SQL2, T2 last and as a unit); lanes based on `1239a5ff` meet C1 at merge.
   Wave-2 gate: 0 failed, no known-red exception. Then P9-B0 again, D-COV, whole-wave review.
7. Wave 3 (C3, D3, A4, AL-DATA, AL-CLOCK, SQL3 incl. SQL1-MON; 3b: COV-MV, SQL4), PRE, `p9-prereg.md`
   (N_c <= 71, N_tot <= 252), P9 cells, adoption print, OD-3 read once, freeze gate (G-P1..G-P9 + G-P11; G-P10
   print-only), final "Rulings I made" list. Doc pass for stale plan text and briefs SQL3 / SQL4 before wave 3.

## Open questions waiting for a PM ruling (none ruled yet)

- **M1d / P9-B0:** Y-1 is a vol-target book; C1's sqrt change may move its `vol_target` rows and the blocks of books
  L1-L4, which are not on the ruled C1 substitution list. Rule the list before the run (detail in the M1d handoff).
- **SQL2:** new `atx.<name>/v<n>` schema literals from other lanes turn the classes guard red after merge: does root
  add them to `classes.json` at merge, or each lane?
- **B2:** (1) Python-statistics deletion as a commit after root's identity run, or a side branch; (2) is K-P9-5
  `paired.se` the Ledoit-Wolf studentized SE; (3) tie test adds `schema` to eval_tie ledger records it writes.
- **C2:** (1) calibration record in `summary.json` rather than `recipe.json` (C1 precedent); (2) may the run stage
  stop before the NAV in engine mode; (3) pass 2 misses tolerance: publish `matched: false` and let the match stage
  stop? M1c-RED cause (for the record): the test's `norm_ppf` literals are constant-folded in Release with a fused
  multiply-add, the runtime kernel does not fuse; planned fix is a contraction-free polynomial in `stats_ext.hpp`.
- **S2:** (1) re-pin both source digests without a semantics version bump; (2) advertise label-terminal /
  min-coverage / ic-hac-rule in `exe_capabilities` (D-lane file); (3) cache the missing-exit-v1 IC that runs beside
  `--label-terminal`.
- **AL-COMB:** (1) brief puts `rules:` in templates, schema refuses it, E2 puts it in the v2 wave manifest: follow E2
  or widen the schema; (2) cross-lane edits beyond P17's three.
- **E2:** (1) K-P9-10 `--attempt k` argv change now; (2) composition fixture refused until D2 fixes `params_schema`;
  (3) NAV-rule and horizon fixtures need C1: root runs them after merge.
- **AL-SIG:** WYZ `source_sample_end` 2006 is from memory, must be checked; `gia_13f` literature rho .15 is above the
  .1 bar; `ENGINE_FIELDS` does not route engine-only rows.
- **D2:** (1) `--engine-fit` writes Python bytes and reports C++ agreement in the summary, or writes C++ values under
  the P12 tolerance; (2) should CM-5 also fingerprint the composition modules' `module_sha256`; (3) must
  ew-theme-aim-v2 and the resid diagnostics get a C++ path before `composition_*.py` is deleted.
- **A3:** (1) the registry flip needs the six fields routed by `prepare_research_fields_engine`, outside A3; (2) P13
  untouched; (3) `--reuse` re-hashes the vendor file once per field; (4) fixture ~320 KB. Also: A3 decided both
  M1a-RED gtests were wrong and the code right (against numpy 1.26.4 and the field spec). That changes test
  expectations, so its reviewer must check the evidence, not accept it.
- **COV:** (1) split the remainder into two lanes; (2) eigen-adjustment memory limit: kernel default or the verb's
  `--max-bytes`; (3) is "asset-level MRAD" per series. Its as-of trace still needs the check of whether the role's
  adjusted close embeds anything after session d (COV-4, G-P11).
- **T2 review (minor):** new module `t2_gold.py` is outside the brief's file list; bulk script edits against lane
  rule 1.

## Unresolved from the owner

- "pytest runs forever and spins CPU": not reproduced, not fixed. `scripts/tests` finishes in ~436 s; the heavy python
  processes seen were another session's atx-db jobs. Guard in place (PY-HYG). If it recurs, ask the owner which
  command and tree.
- Uncommitted at stop: this file and the last ledger lines if the commit below failed; review files in pools 21 and 22.
