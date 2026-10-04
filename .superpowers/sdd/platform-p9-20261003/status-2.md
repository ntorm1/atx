# P9 sprint: status 2 (PM handoff, 2026-10-03, end of PM session 2)

Plan: `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md`. Ledger (authoritative): `progress.md` in this dir.
Trust the ledger and `git log` over this file if they disagree. Root = `C:/atx-wt/pool-2`, branch
`feat/platform-v8-20260929` (v8 still open; P9 branch is cut at R0-14). Status 1 (`status-1.md`) covers session 1.

**At the stop: every subagent is stopped** (root-r0-11 was killed after its last commit). No research, build or
compiler process is running. Every wave-1 pool is clean at its approved head. The pool-2 tree is clean except the
owner's untracked plot `docs/plans/2026-10-02-x5-equity-curve.png` (leave it; ruling P1).

## 1. Where things stand (one line each)

- **Phase 0:** R0-0..R0-11 done. Next: R0-11 report (the agent was killed before writing it), then R0-12, R0-13, R0-14.
- **Wave 1:** all 8 lanes reviewed APPROVE. Not merged (merges come after R0-14). The merge brief is ready.
- **Wave 2:** briefs + carry sheet ready (T2 brief written, pool remap ruled). Not dispatched.
- **Waves 3 / PRE / P9 cells / adoption / OD-3 / freeze:** not started.
- **Trial budget:** v8 construction **N 62 of 62 (cap reached)**. Ledger `build-equity/trials.jsonl` 133 lines,
  sha256 `27e40f9f…`. Admission 40 of 40.

## 2. Phase 0 results (registered verdicts, as logged in the v8 integration log)

| step | cell | verdict | key numbers (vs parent) | trial | commits |
|---|---|---|---|---|---|
| R0-6 | Y-S (b library v8ysb, L 1.1828) | **ACCEPTED** (PM7-34: dsr>0, mechanics) | net SR 1.8495 vs 1.7695; dSR +.080 (SE .150); DSR .847; PBO .077 | 11c10def, N 57 | d66f93f6..ff0c5552; driver c0f1fae6 |
| R0-7 | Y-3 norm-score | NOT ACCEPTED (rejected, counted) | 1.8119 vs 1.8495; dSR -.038 (SE .040) | ee548710, N 58 | manifest c18a92b7; report 629abbc8 |
| R0-8 | Y-2 theme-tsmom | NOT ACCEPTED | 1.5182 vs 1.8495; dSR -.331 (Memmel SE .231, 1,005 sessions; rule acts 2021-01-05..2023-12-06); CM-2 noted | aeeb2073, N 59 | manifest 15017bb4; report 020cc018 |
| R0-9 | Y-5 two-speed | NOT ACCEPTED (P13 passed first) | 1.7294 vs 1.8495; dSR -.120 (SE .075) | cc150c21, N 60 | manifest 43128be8; report 74184cce |
| R0-10 | X-10 (L 2.0 on Y-F0) | **ACCEPTED** by hand under PM7-34 (3) | net 9.31% > 5.65%; SR 1.8083 >= 1.7495; vol 5.15%; max DD 4.33% | 25f3b27a, N 61 | spec 42ddf3ba; report dbc2b975 |
| R0-11 | Y-1 vol-target on Y-F0, child of X-10 | **ACCEPTED** by hand (v8y §6/§8 rule) | dSR vs X-10 +.0215 (SE .0518); net 8.80% > 5.65%, SR 1.8298 >= 1.7495; mechanics 6/6 (gross 1.5557); mean L_t 1.8671; 4x 1.6281; vol 4.809%; max DD 3.602%; DSR(N62) .8251; PBO .1526 | 0ad7ea4c, N 62 | d1f2d7f2, ac74a48c, 00f0521a, 0829f784; **no report** |

**Consequences:** Y-F0 = `scripts/specs/v8/lib-v8ysb-gm.json` (the unlevered claims book). **Y-1 replaces X-10 as Y-F**
(the levered book offered to the owner; L_t <= 2.0 per OD-P9-3). Y-1 gets no OD-3 read (YP-11). X-10 and Y-1 verdicts
were applied **by hand** (no tool prints those rules): R0-12 must re-derive both independently before printing (ledger
ruling).

## 3. Wave-1 lanes (all APPROVE; merge in this slot order after R0-14)

| slot | lane | pool | approved head | review files (this dir) |
|---|---|---|---|---|
| 1 | E1 tasks 1+ (task 0 merged 1b9b37e8) | 17 | fdd2bda3 | task-E1-review, -rereview-1, -rereview-2 |
| 2 | T1 | 19 | 0a61b706 | task-T1-review, -rereview-1 |
| 3 | A1 | 12 | f8edca96 | task-A1-review, -rereview-1..3 |
| 4 | A2 | 13 | 2b6f8e6f | task-A2-review |
| 5 | S1 | 18 | 34ef92dd | task-S1-review, -rereview-1 |
| 6 | B1 | 14 | 01f20405 | task-B1-review |
| 7 | D1 | 20 | 757ba582 | task-D1-review (B1 needs no adaptation) |
| 8 | C1 | 15 | 10c35df3 | task-C1-review, -rereview-1 |

Merge recipe, per-lane notes, gates and P9-B0: **`root-wave1-merge-brief.md`** (complete; E1 head filled in).

## 4. What the next PM does, in order

1. **R0-11 close-out (root agent, no runs):** write `root-R0-11-report.md` from the v8 integration log section "R0-11"
   (line ~7714 on) and commits d1f2d7f2..0829f784, then commit it with `git add -f`. Confirm ledger 133 lines and N 62.
   Run nothing.
2. **R0-12 adoption print (v8y §7; plan line 183), root agent.** First re-derive X-10's and Y-1's hand verdicts from the
   registered rule text (v8y §6 / §8, PM7-34 (3)) and their receipts / bundles; a mismatch stops R0-12. Then the print:
   Y-F0 deployable iff S2 net Sharpe >= 1.0 AND mechanics; DSR_tot / hand / v8, PBO, both p, 4x, year table; bundles
   R-2 vs Y-F0, X-F0 vs Y-F0, B0c vs Y-F0. G-B1 printed for Y-F0.
3. **R0-13:** reads nothing (DEC-3 / OD-P9-2). Ledger one line.
4. **R0-14 (root agent):** final v8 report (status 8): Y-F0, X-10, Y-1 side by side (net, 1x and 4x Sharpe, vol, max DD,
   mean L_t); the leverage decision for the owner; ledger N_c, K_a, M, N_tot recorded as P9's `n_before`; then cut
   `feat/platform-p9-20261003` from the v8 head. Phase-0 exit gate (plan §4.3).
5. **Wave-1 merges (root agent):** `root-wave1-merge-brief.md`. Confirm every wave is complete (`wave status`) before
   merging E1 (E1-STALE). Then the §4.2 suites, canary goldens Debug + Release, Release IC adoption (G-P3 IC half),
   G-P4, G-P8, P9-B0. Then one read-only whole-wave-1 review of the integration head; fix I/M findings before wave 2.
6. **Wave 2 (9 lanes):** dispatch from `briefs/brief-<ID>.md` + **`wave2-carry.md`** (per-lane binding rulings and
   carried minors) with the post-P9-B0 head as base. Release wave-1 pools first (`lease-worktree.ps1 -Release pool-N
   -RunId p9-<id>-20261003`). Pools: E2 17, A3 12, S2 18, B2 14, D2 20, C2 15, AL-COMB 19, AL-SIG 13, **T2 fresh pool-21**
   (`-MaxPool 21`). Pool-16 is unavailable (foreign WIP), and pools 7/8/10 have dead foreign owners: do not recover them.
   Then reviews, merges (`p9-2*`), P9-B0, the AL-COMB zero-trial diagnostic, and K1 `--plan-only` of every AL-SIG string.
7. **Wave 3:** C3, D3, AL-DATA (gated: builds nothing unless data landed), A4 (stretch), AL-CLOCK as 3b after C3
   (DEC-16 already ruled distinct), PRE after AL-CLOCK's report (P1). Merges + P9-B0 on `p9-3*`.
8. PM rules `p9-prereg.md` (plan §5.2, DEC-18), using R0-14's `n_before`. Root runs the P9 cells P9-L, P9-S, P9-H, P9-M,
   P9-C, P9-W, P9-X. Then the P9 adoption print (G-B1..G-B9), the OD-3 read once (§5.3), and the freeze gate (§4.4; the PM
   writes the verdict).

## 5. Rulings made in session 2 (all in the ledger, binding)

- **Resume:** killed lanes' dirty edits are finished by fresh implementers. The stale lock is removed only once its
  holder is dead.
- **R0-6-ATT:** a killed or failed attempt is re-run once with `research_cycle.py ... --attempt <phase>=N`; the driver
  then adopts it.
- **SEAL-ALLOW** (standing, R0-6..R0-11): `--seal-allow` only for the png file name `2026-10-02` and the run's own UTC
  date as `started_utc`. Check every hit with `tools/sealsrc.py` first; any other source means stop.
- **R0-7-MAN:** a Y-cell manifest is written mechanically from the registration, the chain rule and y-s's shared keys,
  with no free constant. **R0-7-EOL:** the y-*.json templates are restored to LF bytes, and E2 adds `.gitattributes`.
  **R0-9-PIN:** two-speed pins e29365d1 (a description-only diff from 69cf6134).
- **Y1-PARENT:** Y-1's parent is X-10's spec. X-10's and Y-1's hand verdicts are re-derived at R0-12.
- **One fresh root agent per cell or step group;** the root reports `root-R0-<n>-report.md`.
- **T2-GOLD** (3 golden-hash modules keep their hashes and get a module-scoped 2025 window). T2 brief written.
- **Wave-2 pool remap** (above).
- **D1-PSCHEMA:** `params_schema` describes manifest params. D2 fixes it, and E2 reads it at run time.
- **A1-M3:** the adopted seal edit. **C1-SPO:** flag-absent declaration bytes. **C1-PROD:** exe identity goes in
  `summary.producer`.
- **E1-STALE** (refusal unflagged; never merge under a running wave). **E1-REUSE-a2:** the exe-SHA refusal applies only
  against an `exes_sha256` pin; inherited outputs are judged by the parent's pin; **every P9 cell pins exes**.
  **E1-N3:** walking the whole template chain is a required E2 fix.

## 6. How this PM ran it (keep it)

- SDD with Opus 5.5 subagents (`model: opus`), named agents, all in the background. The controller never builds, runs,
  or edits code; it writes only ledger, briefs, rulings and handoff files.
- **Lane dispatch:** brief path + plan line ranges + binding rulings + pool/branch/base + "no TDD" + report path + a
  reply of at most 15 lines.
- **Fix rounds:** resume the same implementer with the findings verbatim (rounds 1-3). The scoped re-review resumes the
  same reviewer over FIX_BASE..HEAD. Minors are ledgered as deferred, unless they move bytes, reuse stale output or can
  kill a live run; then the PM raises them into the round.
- **Root:** one fresh agent per Phase-0 cell. It follows the previous `root-R0-<n>-report.md`; it waits for the memory
  gate (free >= peak + 1,536 MiB; host is tight: Pylance ~3.4 GB) and never launches under it; it stops on any
  ambiguity and the PM rules.
- Agents killed by an owner stop leave their state in git + the integration log. Re-derive it from those, never from
  memory.

## 7. Goal prompt for the next parent agent

```
/goal Resume and complete C:\atx-wt\pool-2\docs\plans\2026-10-03-p9-sprint-plan.md end to end as PM, using subagent-driven development (superpowers:subagent-driven-development) with Opus 5.5 subagents (model: opus) for every lane, reviewer and root-integrator step. No test-driven development: implementers implement first, then write the tests the briefs name. Preserve your own context: never build, run real data or edit code yourself; hand artifacts over as files.
First read, in order: C:\atx-wt\pool-2\.superpowers\sdd\platform-p9-20261003\status-2.md (this handoff; section 4 is your work list), then the ledger progress.md in the same dir (every "Ruling" line is binding, incl. P1-P19, A1-C, B1-H, K-P9-4a, T1-SE, T1-HIST, S1-PIN, A1-SHIM, DEC-16, SEAL-ALLOW, R0-6-ATT, R0-7-MAN, R0-7-EOL, R0-9-PIN, Y1-PARENT, T2-GOLD, D1-PSCHEMA, A1-M3, C1-SPO, C1-PROD, E1-STALE, E1-REUSE-a2, E1-N3, the wave-2 pool remap), then plan §0.6, §1.1, §2.1-§2.3, §3, §4 (lane block bodies only as needed). Do not re-dispatch anything the ledger marks complete: all 8 wave-1 lanes are APPROVE and Phase 0 R0-0..R0-11 is done (v8 N 62 of 62).
Then, in order: (1) a root agent writes the missing root-R0-11-report.md from the integration log (no runs); (2) R0-12 adoption print, after independently re-deriving the hand-applied X-10 and Y-1 verdicts (mismatch = stop); R0-13 reads nothing; R0-14 v8 report + cut feat/platform-p9-20261003; (3) wave-1 merges per root-wave1-merge-brief.md, suites, canary, Release IC, P9-B0, whole-wave review; (4) wave 2 from briefs + wave2-carry.md (incl. T2 on a fresh pool-21; pool-16 unavailable), reviews, merges, P9-B0; (5) wave 3, PRE, p9-prereg.md, the P9 cells, P9 adoption print, OD-3 read once, freeze gate. One fresh root agent per cell or step group. Record every decision in the ledger as "Ruling: decision -- why -- cost if wrong" before the measurement it could bias. Stop only for irreversible/destructive actions, security-sensitive actions, pushes/merges outside the worktrees, or a plan so broken every path is a guess.
```
