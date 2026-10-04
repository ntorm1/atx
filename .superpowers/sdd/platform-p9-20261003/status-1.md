# P9 sprint — status 1 (PM handoff, 2026-10-03)

Plan: `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md` (committed 84f06f6b). Briefs: `docs/plans/2026-10-03-p9-lane-briefs.md`, split per lane in `briefs/brief-<ID>.md` (rules 1-10 + lane section) and `briefs/review-template.md`.
Ledger (authoritative, every ruling and result): `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/progress.md`. Trust it and `git log` over memory.
Root = `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929` (v8 still open; P9 branch not cut yet).
This file and the sprint dir are git-ignored and NOT committed (root agent was committing concurrently); commit with `git add -f` when no root agent is running.

## 1. Done

| item | result | evidence |
|---|---|---|
| Pre-flight scan | 19 conflicts found, ruled P1-P19 | `preflight-scan.md`; ledger "Pre-flight scan" |
| Owner-decision defaults | OD-P9-1 hurdle 8.0% placeholder; OD-P9-2 defer OD-3 to freeze; OD-P9-3 L <= 2 until after v8 report; OD-P9-4/5/6 not executable (AL-DATA gated, builds nothing); OD-P9-7/8/9 per plan | ledger "Controller rulings" |
| R0-0 | DEC-1..4, DEC-19 + PM8-16/18 text into v8 progress.md | commit 6ec4c7cb |
| Docs | plan, briefs, 6 reviews, lit review, status 7 committed (png left untracked: NAV plot) | 84f06f6b |
| Pools | 12-15 released; wave-1 leased at d7c1c520: A1 12, A2 13, B1 14, C1 15, E1 17, S1 18, T1 19, **D1 20**; pool-16 left leased (YARCH slice-3b WIP in a conflicted stash — not ours, untouched) | `pools.md` |
| R0-3 | PASS: X-5 under v8-16d (NAV 27/27; w 10/12 + 2 timing-only; fit equal after PM7-30 subs) | d23efa5a |
| R0-4 + P8 | PASS: v15 manifest 26fee5ce = pin, 83/1; X-5 NAV on v15 S2 daily 529062d6 | fd962cb9 |
| R0-5 | PASS: Y-S plan-only 1,952.5 MiB < 2,560 (no cap ruling) | fd962cb9 |
| R0-2 | P0-FIX (E1 task 0, review APPROVE) merged 1b9b37e8; y-s amended 4492f301 (600 s; prefixes ["v8x","v8ys"]); tests 0 failed; `wave plan y-s.json` exit 0 | 4a5e84a0 |
| DEC-16 | ruled blind before any Y read: info-clock distinct from Y-5; mom-volman distinct from Y-2 / Y-1 | ledger |

## 2. In flight at the stop (agents were NOT killed; their outputs land in files)

| agent | doing | where the result lands | next action |
|---|---|---|---|
| root-r0-6 | R0-6 Y-S wave, stage by stage. Done: 01 preflight (= hand plan), 02 register (a5914373), 03 screen (gate PASS, kept 9 / dropped 6, marginal 223.5 s of 600), 04 cell library committed 816be40b; later stages running | integration log (`.superpowers/sdd/platform-v8-20260929/integration-log.md` tail), `root-R0-6-report.md`, `build-equity/waves/y-s/wave-result.json` | read report / log tail; if the agent died mid-stage, resume with `wave run y-s.json --until <next stage>` (driver resumes; check receipts first) |
| review-c1 | adversarial review of C1 @ f6cd387d | `task-C1-review.md` | APPROVE -> complete; BLOCK -> fix round 1 to C1 |
| review-d1 | review of D1 @ 757ba582 incl. "B1 adaptation" section (D1 changed `strategy_ic_detail.hpp` signatures B1 includes) | `task-D1-review.md` | as above; rule who adapts B1 call sites at merge |
| lane-e1 | E1 tasks 1+ (driver hardening) + 4 task-0 minors + `--runner-override` cap | pool-17 branch (head e93d5c2b, dirty at stop), `task-E1-report.md` "Tasks 1+" | review E1 head (base 3fa2dd4a for tasks 1+) |
| lane-a1 | fix round 1: accept engine-only registry rows (M1) | pool-12, report "Fix round 1" | scoped re-review FIX_BASE 88c9efa2 |
| lane-s1 | fix round 1: Release must not read Debug cache via legacy DIR fallback | pool-18 (1 dirty at stop), report "Fix round 1" | scoped re-review FIX_BASE 17a885a8 |

If a lane agent died mid-edit, its pool shows dirty files: dispatch a fresh Opus implementer with the brief path, the report path and the open finding (SDD rounds 1-3 rule).

## 3. Wave-1 lane state

| lane | pool | head | review | state |
|---|---|---|---|---|
| E1 task 0 | 17 | 3fa2dd4a | APPROVE | complete, merged (R0-2) |
| E1 tasks 1+ | 17 | (in progress) | - | coding |
| A1 | 12 | 88c9efa2 | BLOCK (1 major) | fix round 1 running |
| A2 | 13 | 2b6f8e6f | APPROVE | complete |
| B1 | 14 | 01f20405 | APPROVE | complete |
| C1 | 15 | f6cd387d | running | review in flight |
| D1 | 20 | 757ba582 | running | review in flight |
| S1 | 18 | 17a885a8 | BLOCK (1 major) | fix round 1 running |
| T1 | 19 | 0a61b706 | APPROVE after round 1 | complete |

Wave-1 merges happen only after R0-14 (PM8-12 (e)), in slot order E1, T1, A1, A2, S1, B1, D1, C1. Merge notes collected in the ledger (grep "Merge note"):
- T1: merge tasks 1-4 + dd5c4f5e now; class-C deletion dee23d2a + 10493041 only after root's `generate_from_spec.py --spec specs/library-v71.json --check` (v71 787c802e, recipe.v2 69e95298, 19 artefacts). Canary goldens recorded after C1 merges, on equity-dev and equity-rel.
- A1 before A2 (2 gtests + `test_real_executable_identity` need A1's `expected/`); flip si_shares, si_dtc, vol_126 to `kind: engine` via A1's generator round-trip.
- B1 includes D1's `strategy_ic_detail.hpp` (signatures changed by D1) and S1's `load_pinned_f64`: rebuild B1 targets after D1 and S1.
- S1 and D1 both append the tail of `atx-impl/CMakeLists.txt`: keep both.
- C1 NAV re-pin list in its report; must be ruled before the identity run (DEC-20).
- Marginal with many candidates: `--workers <= 8` (pair buffer ~67 MB at 256 x 16).

## 4. Remaining (plan order)

1. Finish R0-6 (Y-S), then R0-7 Y-3, R0-8 Y-2, R0-9 Y-5, R0-10 X-10, R0-11 Y-1, R0-12 adoption print, R0-13 (no read, OD-P9-2), R0-14 v8 report + cut `feat/platform-p9-20261003`.
2. Close wave-1 review loops (C1, D1, A1, S1, E1 tasks 1+); per-wave whole-wave review; triage the deferred minors in the ledger.
3. Wave-1 merges + builds (`scripts/research-build.ps1`, tags `p9-1<letter>`) + gtests + identity runs; G-P4, G-P8, Release IC (G-P3 IC half); P9-B0 re-base.
4. Wave 2: E2, A3, S2, B2, D2, C2, AL-COMB, AL-SIG + **new lane T2** (re-date 25 legacy tools test modules; ruling A1-C; 3 modules' expected hashes change with window -> needs a PM ruling first). Pool remap: pool-16 unavailable, D1 used 20 -> reassign D2 and AL-SIG pools. Carry rulings P14-P19, K-P9-4a, B1-H to the relevant briefs.
5. Wave 3: C3, D3, AL-CLOCK (3b, after C3; DEC-16 already ruled distinct), AL-DATA (gated: builds nothing unless data landed), A4 (stretch), PRE after AL-CLOCK's report (P1).
6. PM rules `p9-prereg.md`; root runs P9-L, P9-S, P9-H, P9-M, P9-C, P9-W, P9-X; P9 adoption print; OD-3 read once; freeze gate §4.4.

## 5. How this PM ran it (keep the same discipline)

- SDD with Opus 5.5 subagents (`model: opus`), named agents (`lane-<id>`, `review-<id>`, `root-<step>`), all background; controller never builds, runs or fixes code.
- Lane dispatch = brief path + plan sections/group-block lines + binding ledger rulings + pool/branch/base + "no TDD: implement first, then tests" + report path + <=15-line reply.
- Review dispatch = `briefs/review-template.md` + brief + rulings + report via `git show` + diff via `git -C <pool> diff <base> <sha>`; writes `task-<ID>-review.md` in pool-2 sprint dir. Fix rounds resume the lane with the open findings verbatim; scoped re-review over FIX_BASE..HEAD. Minors never enter the loop: ledger as "minor (deferred)".
- Root dispatch = one bounded step group per agent in pool-2; logs to the v8 integration log; SHA/JSON-path comparisons only where blind; free memory >= peak + 1,536 MiB before each real run.
- Group block lines in the plan: A 289-324, B 325-349, C 350-383, D 384-412, E 413-439, S 440-474, T 475-489, AL 490-537.

## 6. Goal prompt for the next parent agent

```
/goal Resume and complete C:\atx-wt\pool-2\docs\plans\2026-10-03-p9-sprint-plan.md end to end as PM, using subagent-driven development (superpowers:subagent-driven-development) with Opus 5.5 subagents (model: opus) for every lane, reviewer and root-integrator step. No test-driven development: implementers implement first, then write the tests the briefs name. Preserve your own context: never build, run real data or edit code yourself; hand artifacts over as files.
First read, in order: C:\atx-wt\pool-2\.superpowers\sdd\platform-p9-20261003\status-1.md, then the ledger progress.md in the same dir (all rulings P1-P19, A1-C, B1-H, K-P9-4a, T1-SE, T1-HIST, S1-PIN, A1-SHIM, DEC-16 are binding), then plan §0.6, §1.1, §2.1-§2.3, §3, §4 (lane block bodies only as needed). Do not re-dispatch any task the ledger marks complete.
Then: (1) collect the results of the agents that were in flight at the stop (root-r0-6 Y-S, review-c1, review-d1, lane-e1 tasks 1+, lane-a1 and lane-s1 fix round 1) from their files and git heads; re-dispatch fresh only what died; (2) close every wave-1 review loop; (3) continue Phase 0 R0-7..R0-14 on root; (4) wave-1 merges + P9-B0, wave 2 (incl. new lane T2; remap pools: 16 unavailable), wave 3, PRE, the P9 cells, adoption print, OD-3 once, freeze gate. Record every decision in the ledger as "Ruling: decision -- why -- cost if wrong" before the measurement it could bias. Stop only for irreversible/destructive actions, security-sensitive actions, pushes/merges outside the worktrees, or a plan so broken every path is a guess.
```

## 7. Addendum: all agents killed (owner "stop")

Section 2's in-flight agents were stopped with TaskStop after this file was written: root-r0-6, review-c1, review-d1, lane-e1, lane-a1, lane-s1. No research process is left running. What that means for the next PM:
- **Y-S (R0-6):** stopped after stage 04 (cell library committed 816be40b). Check the integration-log tail and `build-equity/waves/y-s/` receipts before resuming with `wave run y-s.json --until <next stage>`. A stage dir left partial counts as a failed attempt; it is not a cell.
- **lane-e1** (pool-17), **lane-a1** (pool-12), **lane-s1** (pool-18) were stopped mid-edit and may have uncommitted changes. Look at `git status` and `git diff` in each pool, then either dispatch a fresh Opus implementer to finish from the brief, report and finding, or discard with `git checkout -- .` after looking.
- **review-c1, review-d1:** no review was written. Re-dispatch both, at C1 f6cd387d and D1 757ba582.
