# P9 sprint: PM handoff 15 (owner stop 14, 2026-10-16 15:50 EDT)

For the next parent (PM) agent. Supersedes `status-14.md`. The goal prompt to start the next session is in
`next-goal-prompt.md` (same dir, refreshed at this stop). Written at the owner's stop ("stop here and write handoff
file for next parent agent") while the cells root agent 1 was mid P9-L; its hand-back arrived minutes later and is
recorded (ledger :1046; its handoff `handoff-root-cells-1.md` committed `31036825`): P9-L STOPPED at the w pass on the
IC exe's memory admission (no read; 0 trials); Ruling P9L-WBUDGET (:1047) sets the resume. The ledger block from
"- RESUME 14" (line 1020) to the end records every hand-back and ruling of this session. Verify every head with
`git log` before dispatch.

## Read order

1. This file.
2. The contracts every agent gets: `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md` .. `resume12-addendum.md` (the last wins). This session's dispatch files, all in `r14-prompts/`:
   `root-flags-prompt.md` (the RQ-1 close: DONE), `pre-flags-prompt.md` (DONE), `rev-flags-prompt.md` (DONE),
   `cells-prompt-1.md` (the cells root's dispatch of record; it supersedes `r13-prompts/cells-prompt-1.md`; its facts
   bind every cells agent: the FLAGS-ROUTE route, the parent's SHA, LAPSE-TABLE, SG4-REF, HOST-6a), `deriv-p9l-prompt.md`
   (READY but for `@@OUTPUT-DIRS@@`), `deriv-p9v-prompt.md`, `deriv-p9x-prompt.md` (tokens), `idx-final-prompt.md`
   (the freeze; token `@@LEDGER-LINES@@`). `r12-post-gate.md` (the CELLS runbook steps 0-5, DERIV, FREEZE); `r13-prompts/`
   and `r12-prompts/` as history (`ledger-cells-rulings-draft.md` is now IN the ledger, :1035-1042).
3. The ledger `progress.md` from "- RESUME 14" (:1020) to the end. Every "Ruling" line is binding; REPIN-UW / REPIN-R2
   are superseded by REPIN-REV; FLAGS-ROUTE (:1019) stands, extended by FLAGS-ROUTE-2 (:1022) and FLAGS-ORDER (:1028).
4. `handoff-root-cells-1.md` (the cells root's stop handoff, `31036825`: Done / Not done / "Exact next step" of P9-L;
   its two questions ruled P9L-WBUDGET / SEAL-HAND), `cells/p9l-label-terminal-plan.md` (committed `0950cb57`),
   `cells/p9l-label-terminal-record.md` (`31036825`; STOPPED at w, no verdict), `handoff-root-gate.md` (the gate's FINAL handoff + "FLAGS-ROUTE amendment"),
   `root-gate-report.md` ("Status by step ... FINAL" + "FLAGS-ROUTE (RQ-1 close)"), `gate-report-review.md` (PROCEED),
   `prereg-2bii-review.md` (part 2 + "RQ-1 re-check: CLOSED (APPROVE)" at `a32b6f4c`).
5. `p9-prereg.md` at `0950cb57` (round 2b-ii COMPLETE + part B3 `ef849874` + the cells root's item-7 / SG4-REF edit;
   RULED: PRE-2BII-RULED :1034), `rulings-index.md` + `rulings-index-check.md` (IDX-FINAL at the freeze),
   `p9-od3-runbook.md` (section-0 line "F.1 governs", `cb051169`).

Plan: `docs/plans/2026-10-03-p9-sprint-plan.md`. Root tree `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`.

## State in short

GATE-1 DONE and reviewed PROCEED (part 1 `6c608ff3`; part 2's one Required RQ-1 CLOSED by FLAGS-ROUTE: spec commit
`f8d852f3` adds `--ann-mean-gross --info-clock-print` to `scripts/specs/v8/lib-v8ysb-gm.json` `nav.flags`, its SHA-256
`3631679c...` -> `a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a`; `verify` holds; fixture digest
`a50fdcf9...100c`; the spec-route Y-F0 NAV `build-equity/p9-fr-yf0-nav` is 27 / 27 byte-identical to the reference
`p9-b0g-yf0-nav`; re-check `a32b6f4c` "RQ-1: CLOSED (APPROVE)"). Re-pin of record `2078f877` (unchanged; EXE-FREEZE from
it; no exe touched). `p9-prereg.md` RULED (PRE-2BII-RULED, :1034) at `ef849874` (+ the cells root's registration edits
since). The CELLS rulings (CELLS-1 .. CELLS-6, ADOPT-1, OD3-1 / OD3-GO) are in the ledger (:1035-1042). Cells root agent 1
dispatched (:1044) and mid P9-L at the stop: registration commits `0bba6c89` (template `label-terminal.json`), `6ea79e5a`
(`label-terminal-cal.json`, the PM6-6 run recipe), `0950cb57` (plan file, prereg item 7 + SG4-REF + SG-3, S2-6 scan =
no stale `imputed-v1` entry, cache listing before, `cells/tools/`); run 1: fields / u / fit / card completed, the
admission gate PASS, then the w pass REFUSED by the pinned Release IC exe before any payload load ("required_bytes=
3448251496 ... exceeds configured memory budget": `--label-terminal` adds `label_terminal_bytes`; the registered w
budget `--max-memory-mib 3072`), cycle `HARD-STOP [w]` exit 4; the record `cells/p9l-label-terminal-record.md` and
`handoff-root-cells-1.md` committed `31036825` (= docs head). A driver stop before any read: not a void, not UNDEFINED
(P9L-WBUDGET :1047): the next cells root redoes CELLS step 1 for the w budget (3,584 MiB, runner cap alike) and resumes
step 2 with `--attempt w=2`. No verdict, no DERIV, no cell ruled. Trial ledger 133 / `27e40f9f`;
`build-equity/research-store/` absent; `index.sqlite` absent; the six P9-B0 reference dirs intact (PM re-hash);
known-red set EMPTY; no permission refusal this session.

Lapse table of record (LAPSE-TABLE :981; CELLS-4): P9-L, P9-H, P9-M, P9-C, P9-MV, P9-V RUN; P9-W LAPSED (0); P9-X pending
PRE-30 / PRE30-MVX. Fit mode flag-absent. Parent of P9-L: the re-based Y-F0 (`p9-b0g-yf0-nav`; u / w resume roots the
v8-era dirs pinned in `lib-v8ysb-gm.json`, REPIN-REV). Every spec over the lineage parent now inherits both opt-in
flags (FLAGS-ROUTE-2 (a)); a hand cell against `p9-b0g-y1-nav` may carry `--info-clock-print` one-sidedly: disclosed,
not a void (FLAGS-ROUTE-2 (b)); argv token order is not an input (FLAGS-ORDER).

## Tracks at the stop

| track | where | state | next |
|---|---|---|---|
| RQ-1 close (FLAGS-ROUTE) | root FLAGS agent; `f8d852f3`, docs `5ec208b4` | DONE; identity 27 / 27; released | - |
| PRE B3 + SG-1 / SG-2 | `p9-prereg.md` `ef849874`, `bc034c4a`; runbook `cb051169` | DONE; released | - |
| RQ-1 re-check | `prereg-2bii-review.md` `a32b6f4c` | CLOSED (APPROVE); SG-4 ruled SG4-REF (:1033), applied by the cells root in `0950cb57` | - |
| PRE-2BII-RULED | ledger :1034 | DONE | - |
| cells root agent 1 | pool-2; `r14-prompts/cells-prompt-1.md`; stop commit `31036825` | DONE_WITH_CONCERNS: P9-L STOPPED at w (IC memory admission; no read; 0 trials); handoff `handoff-root-cells-1.md` ("Exact next step"); rulings P9L-WBUDGET (:1047), SEAL-HAND (:1048) | the next PM: dispatch cells root agent 2 from the handoff (a chained prompt over `cells-prompt-1.md`: "agent 2; read `handoff-root-cells-1.md` WHOLE and the rulings P9L-WBUDGET / SEAL-HAND; redo CELLS step 1 for the w budget only, then step 2 with `--attempt w=2`; step 0's checks first"); P9-L's own verdict -> DERIV (`deriv-p9l-prompt.md`, fill `@@OUTPUT-DIRS@@`) -> "DERIV agrees" -> the ledger line "Cell P9-L: ..." -> P9-H |
| OD3-PREP / OD-3 | - | OD3-DB unanswered: default "not read in P9" | only on the owner's yes |
| freeze | - | - | after the last cell: adoption print (ADOPT-1), (OD-3), `freeze-verdict.md`, `rulings-index-final.md` (IDX-FINAL, `idx-final-prompt.md`), `final-report.md` |

Frozen pools 8 / 24 untouched (TEETH-POOL); pool-23 `build-equity/` untouched (TEETH-BIN-23); foreign atx-vol sessions
alive on pools 1 / 3 / 4 / 33 / 34 (HOST-6 / HOST-6a; never touched); pools 35 / 36 free; every P9 lease `owner=dead`
except pool-32 (LEASE-DEAD).

## Open with the owner

1. **OD3-DB** (decides whether the OD-3 read happens): unanswered; default on silence stands (OD-3 "not read in P9",
   G-B8 "not evaluated", the freeze verdict says so).
2. **FIELDS-GONE (record only):** unchanged (status-14 #2); FIELDS-RISK did NOT surface at P9-L (the cycle's preflight
   passed; fields / u / fit / card ran). The P9-L stop is a memory-admission refusal of the IC exe, not a fields one.
   Host note for you: free RAM was 4.0 of 15.7 GB with the vol sessions building; P9-L's w needs about 3.6 GB.
3. **Clean-up owner-calls (DISK-4, unchanged)** plus this session's regenerable additions: `build-equity/p9-fr-yf0-nav`
   + `-run` (10.85 MB; the FLAGS-ROUTE identity evidence; P10 clean-up), P9-L's run dirs once its record is committed
   (the cells root deletes its own regenerable scratch by name; never the cell of record's output dirs).
4. **Permission refusals this session (PERM-1):** none (root FLAGS, PRE, reviewer, cells root 1 so far, the PM).
   Carried: status-14 #4 (the GATE-REV reviewer's four scratch scripts under the previous PM scratchpad stay).
5. **Unchanged owner items:** A4 flips (P10), frozen pools 8 / 24, leases, `C:\atx\data` (DATA-3),
   `atx-db-request.md` (AL-DATA), OD5-2 / OD5-3 / OD5-5 / OD5-7, SCRATCH-2, the broken `dev` preset, SPDLOG-RELINK, the
   P10 list (`rulings-index.md` Table 3 + RESUME 13's + RESUME 14's: `p9-fr-yf0-nav` clean-up).

## Gates and rules the next PM must hold (new this session; status-14 lists the earlier ones)

- HOST-6a (:1021): every pool this sprint does not lease is foreign (today 1, 3, 4, 33, 34); ignored in wait loops;
  C: >= 15 GB free before any run of ours that writes, else BLOCKED-HOST.
- FLAGS-ROUTE (:1019) executed; FLAGS-ROUTE-2 (:1022): inheritance of both flags on every spec over the parent; the
  one-sided `info_clock` block on a hand cell vs `p9-b0g-y1-nav` is disclosed, not a void; the parent's SHA is now
  `a4ef6c04...c06a` (every `3631679c` citation restated beside, PRE-40); the re-pin of record stays `2078f877`.
- FLAGS-SEQ (:1023): the RQ-1 close order (done). FLAGS-ORDER (:1028): argv token order is not an input. RUNBOOK-B6
  (:1026): no further runbook edit; "F.1 governs". SG4-REF (:1033): the "references carry both" wording restated by
  the cells root (done in `0950cb57`).
- PRE-2BII-RULED (:1034): the prereg at `ef849874` is the registration of record; later edits are PRE-40 restatements
  beside, by pathspec (the cells root fills Appendix IV items 7, 10, 11, 13, 19, 21, 16-18 at each registration commit).
- CELLS-1 .. CELLS-6, ADOPT-1, OD3-1 / OD3-GO (:1035-1042) as drafted: one cell at a time, one hand-back each, the PM's
  ruling line per cell; hand cells get DERIV (CELLS-2); the PM reads only the registered prints (CELLS-3); the void
  checklist before each cell (CELLS-6).
- Everything of status-14 and earlier (TEST-2, GATE-TMP-2, GATE-MYPY exclude on every commit, PAR-1 pathspec commits,
  PERM-1, LEASE-DEAD, TEETH-POOL / TEETH-BIN-23, REV-FRESH, EXE-FREEZE from `2078f877`, the known-red set EMPTY, ledger
  appends by scratch file + `cat >>`, never edit earlier lines, the PM never commits in pool-2: DISPATCH-COMMIT: the next
  root agent's step 0 commits `progress.md`, `next-goal-prompt.md`, `status-15.md`, `r14-prompts/` as they stand).

## Next steps, in order

1. Verify the stop state read-only: `git log` / `git status` in pool-2 (docs head `31036825`; code head `6ea79e5a` =
   P9-L's two registration specs over `f8d852f3` over `f8a85ad4`; `git diff 2078f877 HEAD` outside `.superpowers/` =
   `lib-v8ysb-gm.json` + the two new P9-L templates ONLY; tracked changes: `progress.md` (:1046-1049),
   `next-goal-prompt.md`; git-ignored, uncommitted: `status-15.md`; the five PNGs untracked); trial ledger 133 /
   `27e40f9f`; `research-store/` absent; `index.sqlite` absent; `lease-worktree.ps1 -Status`; `handoff-root-cells-1.md`
   (its "Exact next step"); the ledger's last lines (:1045-1049).
2. Resume P9-L: write `r14-prompts/cells-prompt-2.md` = `cells-prompt-1.md` + "You are agent 2 of the chain: read
   `handoff-root-cells-1.md` WHOLE first (its 'Exact next step') and the rulings P9L-WBUDGET (:1047) and SEAL-HAND
   (:1048); step 0's checks first (`cells/tools/start_state.py --refs`, `exe_sha_end.py` both builds, the ledger 133 /
   `27e40f9f`, free RAM and C:); then CELLS step 1 again for the ONE edit P9L-WBUDGET names (the w IC argv
   `--max-memory-mib 3584`, the runner's w-stage `--max-rss-mib 3584` alike, free RAM stated; re-`lock --write --exes`;
   the `-cal` recipe re-derived; the JSON diff in the code commit; the PRE-40 bracket in B.1 / Appendix IV item 7; the
   plan-file line); then step 2 with `--attempt w=2` (never re-run u / fit / card; the refused `-run1` dir kept); then the
   plan file's steps unchanged through the mechanics verdict; STOP for DERIV"; dispatch a fresh cells root agent 2
   (Opus; the wrapper form: "your full dispatch text is the file ...; read it WHOLE; never ask mid-run; no subagents").
   If the w pass refuses again (another admission term, or free RAM too low for 3,584 + overhead on this 15.7 GB host
   while the vol sessions build), it hands back BLOCKED-HOST / STOP and the PM rules (UNDEFINED (0) is the fallback of
   B.1 if the feature cannot run on the frozen exe at any admissible budget; a rebuild is never the answer, EXE-FREEZE).
3. DERIV for P9-L: fill `@@OUTPUT-DIRS@@` in `deriv-p9l-prompt.md` (the record names the dirs: the cell of record NAV at
   L-prime, the `-cal` run, the parent `p9-b0g-yf0-nav`), dispatch a fresh DERIV agent (Opus); compare the two
   derivations; "DERIV agrees" (or rule on a disagreement with both in the ledger); the PM's line "Cell P9-L: ACCEPTED /
   REJECTED / UNDEFINED (0) / VOID per `cells/p9l-label-terminal-record.md`", naming P9-H's parent (P9-L's cell of record
   iff ACCEPTED, else `p9-b0g-yf0-nav`); message the cells root ("DERIV agrees; Cell P9-L: ...; next P9-H, parent ...").
4. P9-H -> P9-M -> P9-C -> P9-MV -> (P9-W skipped, LAPSED (0), the ledger line :975) -> P9-X (PRE30-MVX decides run / N_c
   +0 with PRE-34's check) -> P9-V (DERIV) -> adoption print (ADOPT-1; the PM's "Adoption print read" line with G-B1 /
   G-B2 / G-B4 / G-B6 / G-B9 met / not met) -> OD-3 per OD3-DB (default: `od3-read.md` "not read in P9") -> the freeze:
   `freeze-verdict.md` (plan 4.4 / prereg G; "passed with the identity-bridge flag" on a pass), `rulings-index-final.md`
   (IDX-FINAL: fill `@@LEDGER-LINES@@` in `idx-final-prompt.md`), `final-report.md`, committed by a fresh root agent
   (DISPATCH-COMMIT); leases untouched.

## Rulings made this session (full text in the ledger from "- RESUME 14", :1020 on)

HOST-6a (:1021), FLAGS-ROUTE-2 (:1022), FLAGS-SEQ (:1023), RUNBOOK-B6 (:1026), FLAGS-ORDER (:1028), SG4-REF (:1033),
PRE-2BII-RULED (:1034), CELLS-1 .. CELLS-6 (:1035-1040), ADOPT-1 (:1041), OD3-1 / OD3-GO (:1042), P9L-WBUDGET (:1047),
SEAL-HAND (:1048). Verdicts recorded: GATE-REV part 2 RQ-1 CLOSED (APPROVE) (`a32b6f4c`); "GATE-REV: PROCEED" (:1043).
Hand-backs: PRE PART 1 (:1025), root FLAGS (:1027), PRE PART 2 (:1030), the re-check (:1032), the cells root's stop
record (:1046). Owner's words recorded: the /goal re-issue (RESUME 14, :1020), STOP 14 (:1045). Owner actions observed:
none. Permission refusals: none. Final state line :1049.

## Standing warnings

- The cells read real data: READ NO STATISTIC outside the registered prints; the PM reads only the cells' verdict
  lines, mechanics rows and printed-only lines, and the adoption print's G-B lines.
- Two agents may share pool-2's sprint dir (PAR-1): docs agents commit by pathspec only; root adds explicit paths
  with the GATE-MYPY exclude; never a bare `git commit`.
- The owner's hard rule on `atx-db/` is not the PM's to waive (OD3-DB); the fields dirs are not rebuilt.
- FIELDS-RISK may still surface at a later cell's preflight: a driver stop (UNDEFINED (0) as B.0 words it), the PM rules.
- A hand cell's trial-ledger line is written only after "DERIV agrees" (CELLS-2); a v2 cell's is the driver's at judge
  time. The trial ledger is 133 / `27e40f9f` until then.
