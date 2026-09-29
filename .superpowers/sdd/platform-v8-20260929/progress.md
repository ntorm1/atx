# SDD ledger -- plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md

Recovery map: newest entries at the bottom of this head block ("Execution log"). Older planning entries follow below it.
Root: C:/atx-wt/pool-2, branch feat/platform-v8-20260929, base main 7fbfc379. PY="C:/Program Files/Python312/python.exe".

## Owner directive for execution (2026-09-29, /goal)
"implement sprint plan using sub agent driven dev using multiple parallel opus 5.5 sub agents. Speed up development by
not using test driven dev and not deploying review agents after each task. Only use for larger complex implementations
and logical breaking points. You are approved for all recommendations. Focus on rapid development, high quality /
modular / reusable code, and overall building out the atx-engine to support future work and the atx-impl to prove it works."

## Rulings made before dispatch
- Ruling E-1: owner decisions OD-2, OD-4, OD-5, OD-8 take the plan defaults -- owner approved all recommendations --
  cost if wrong: one re-run of the affected cells. OD-2: IC-pass cap 2,560 MiB and 300 s. OD-8: children are Opus 5.5,
  trailer `Co-Authored-By: Claude Opus 5.5`.
- Ruling E-2: OD-3 (history read 2013-2019) and OD-7 (mining campaign) keep the plan default "tooling only, no read, no
  campaign" -- the plan's recommendation is to build the tools and leave the read to an explicit owner ruling; a read
  cannot be undone -- cost if wrong: the owner grants it later and one session is lost.
- Ruling E-3: OD-6 data asks cannot be served by this sprint (atx-db is another session's tree). Members that need absent
  data are withdrawn before any read -- cost if wrong: R-7 is smaller.
- Ruling E-4: no TDD and no per-task review (owner directive). Tests named in the plan are still written, after the code.
  Review lanes run at wave ends and on B-3 (move-only split), R-6 (optimiser) and H-3 (mining verb) -- cost if wrong:
  defects are found at integration instead of at the task.
- Ruling E-5: child pools 3, 4, 7, 8, 9, 10, 11 are used by branch switch on their existing dead leases, as in v7
  (handoff-1 section 4: "all v7 lanes ran on existing dead leases; that was fine"). No `git worktree add` -- cost if
  wrong: a foreign session leases a pool in use; all pools were clean at start and `-Status` is checked at each wave.
- Ruling E-6: children never build (host: 15.7 GB RAM, 3.5 GB free, atx-db session active). Root serialises builds and
  real-data runs in pool-2. Root may use one integrator agent at a time in pool-2 for merge, build, test, compile fixes --
  keeps the PM context for coordination -- cost if wrong: integration defects fixed without a second reader; the wave
  review covers them.
- Ruling E-7: child reports are written and committed in the child's own worktree (not in pool-2), so pool-2 stays clean
  between runs.
- Ruling E-8: all lanes branch from one base and W0-1 lands by merge. W0-1 edits are one-line constant replacements;
  textual merges with C, F and V lanes are expected to be clean or trivial.

## Pre-flight scan (shared files and interfaces)
| tasks | shared file or interface | finding | ruling |
|---|---|---|---|
| B-1, F-2 | `atx-impl/tools/equity_strategy_ic.cpp` | both add CLI surface | F-2 adds one dispatch line for verb `marginal`; logic in `strategy_marginal_ic.cpp`. Merge B before F |
| B-3, B-1, B-2 | `strategy_ic_runner.cpp` | sequence inside one lane | one lane, order B-3, B-1, B-2 |
| A-3, A-2, H-1 | `scripts/research_cycle.py` | sequence | one lane A, then H-1 after A merges |
| W0-1, C-1, C-3, F-1 | constant sites in C and F files | one-line edits | Ruling E-8 |
| A-1, B-3 | K1 plan rows | A-1 codes against the contract before B-3 merges | contract text in section 6.3 is binding; A-1 test uses a recorded plan JSON fixture |
| E-3, A-3 | K3 `--no-git` | E-3 consumes before A-3 lands | E-3 test skips when the flag is absent; root records goldens after A-3 merges |
| V-1, H-1 | `atx-impl/tools/nav_summ.py` | V-1 moves the file, H-1 adds pooling | H-1 after V merges |
| D-0, D-1, R-4, R-5 | `strategy_nav_replay.cpp`, `strategy_target_replay.cpp` | sequence | D first; R-4 then R-5 in one lane |
| R-1, B | `strategy_ic_composition.cpp` | B-1 skips composition, R-1 edits it | R-1 starts after B merges |
| C-2, F-2, G | K6 `marginal_ic.json` | reader before writer | contract binding; readers tolerate absence |
| W0-4 step 2, D | warm start D-0 is specified in the W0-4 block, owned by lane D | plan text split across two tasks | D brief carries the W0-4 step 2 text |
| each task | tests vs code inside the task | names and flags agree within each block | none |

## Lanes (wave 1 dispatch)
| lane | pool | branch | tasks in order |
|---|---|---|---|
| W0E | 3 | feat/platform-v8-w0e-20260929 | W0-1, E-1, E-4 |
| EV | 8 | feat/platform-v8-ev-20260929 | E-3, V-1, V-2 |
| B | 10 | feat/platform-v8-b-20260929 | B-3, B-1, B-2 |
| C | 9 | feat/platform-v8-c-20260929 | C-1, C-2, C-3 |
| A | 11 | feat/platform-v8-a-20260929 | A-3, A-1, A-2 |
| D | 4 | feat/platform-v8-d-20260929 | D-0, D-1, D-2 |
| F | 7 | feat/platform-v8-f-20260929 | F-2, F-1 |

## Execution log
- 2026-09-29: root branch created from main 7fbfc379. Plan, inputs, briefs, lane rules committed. Lanes dispatched.

---

# platform-v8-20260929 -- ledger (newest first; `git add -f` this dir)

Goal (owner, 2026-09-29): build v8 of atx-impl. Number one goal: a mega alpha implementation of a US equity long/short
strategy that is high Sharpe, high capacity, low turnover. (1) Raise Sharpe: more orthogonal alphas, better existing
alphas, better combination and trading. (2) Keep building the atx-engine platform so research and backtesting iterate faster.

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md

Inputs written this session (read-only work; nothing built, nothing run on book data):
- code-review-v8-platform.md (P-1..P-15), code-review-v8-signal.md (S-1..S-13, C-1..C-9), code-review-v8-engine.md (O1..O10)
- reports/Equity long short alpha v8.md (literature report) and research_notes/Equity long short alpha v8/ (5 note files)

Standing rules: as platform-20260928, with the owner change below.

## 2026-09-29 sprint planned; owner ruling on the window
- OWNER RULING (2026-09-29, during planning): "expand the TRAIN window to include 2023 to give us more sample size, keep
  2024+ hidden and out of sample." TRAIN becomes [2020-01-01, 2024-01-01). Hidden: 2024-01-01 and later.
- Disclosure recorded with the ruling: 2023-2024 was read twice at book level as validation in earlier sprints, so 2023
  enters TRAIN partly selected and 2024 is hidden but not pristine. 2025 and later has never been read.
- Nothing has been read on the new window. No code changed. No trial run. Appendix A unchanged from platform-20260928:
  TRAIN construction cells 37; validation trials 2 spent; 2025+ reserved.
- Open owner decisions: OD-2 (IC-pass memory cap for the 4-year role), OD-3 (history 2013-2019), OD-4 (DSR variance
  convention), OD-5 (report seal regex), OD-6 (atx-db data asks), OD-7 (mining), OD-8 (implementer model). See plan section 2.
- Caveat from the research lanes: all five literature lanes ran out of web-search budget. Items listed as gaps in the
  notes are unresearched, not rejected. Effect sizes in the plan are estimates.
- Next: owner reviews the plan and rules on OD-2. Then Wave 0 (W0-1 window constants, W0-2 role and fields, W0-3
  pre-registration, W0-4 baseline cells) beside Wave 1 platform lanes.
