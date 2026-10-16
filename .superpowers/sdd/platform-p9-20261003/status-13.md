# P9 sprint: PM handoff 13 (owner stop 12, 2026-10-15 16:24)

For the next parent (PM) agent. Supersedes `status-12.md`. The goal prompt to start the next session is in
`next-goal-prompt.md` (same dir, refreshed at this stop). Written at the owner's stop ("stop here and write handoff
file", 16:24) while root GATE agent 1 was in the gate's early steps and the PRE agent was filling round 2b-ii part A.
Their stop records are marked (pending) where the hand-back had not arrived when this file was written; the ledger
block from "- RESUME 12" (line 922) to the end records every hand-back and ruling of this session, and the two
agents' handoff files are the truth. Verify every head with `git log` before dispatch.

## Read order

1. This file.
2. The contracts every agent gets: `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md` (TEST-1), `resume10-addendum.md` (owner
   rules), `resume11-addendum.md`, `resume12-addendum.md` (wins where they differ). Per-agent text this session:
   `r12-dispatch.md` ("ROOT M6B-FINISH" done; "ROOT GATE" filled: head `06dc9e5a`, tag `p9-3f`, GATE-IDX, GATE-PAR),
   `r12-post-gate.md` (PRE-2BII parts A / B, GATE-REV parts 1 / 2, CELLS runbook with the per-cell hand-back
   protocol, OD3-PREP, OD-3 with the OD3-GO checkpoint, DERIV, FREEZE), and the READY prompts in `r12-prompts/`:
   `gate-prompt-template.md` (fill `@@N@@` and the previous-handoff line for gate agent 2+), `gaterev-prompt.md`,
   `cells-prompt.md`, `od3prep-prompt.md`, `deriv-prompt.md`, and `ledger-cells-rulings-draft.md` (CELLS-1 .. 6,
   ADOPT-1, OD3-1 / OD3-GO: NOT yet in the ledger; append them with `cat >>` at the cells dispatch, before any cell).
3. The ledger `progress.md` from "- RESUME 12" to the end. Every "Ruling" line is binding.
4. `handoff-root-m6b.md` (M6B-FINISH: SQL4 checks green; "What the gate root must know"), `handoff-root-gate-1.md`
   (pending at the stop: the gate root's state and "Exact next step"), `root-gate-report.md` as far as written.
5. `p9-prereg.md` (round 2b-ii part A, pending commit at the stop), `rulings-index.md` (`5f48a25d`, 483 rows),
   `root-gate-brief.md`, `r11-dispatch.md` "ROOT GATE" sections (still the gate's rulings text).

Plan: `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md`. Root tree `C:/atx-wt/pool-2`, branch
`feat/platform-p9-20261003`. Trial ledger `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f`,
unchanged (0 trials this session). `build-equity/research-store/` absent (SQL3-2).

## State in short

M6B-FINISH DONE: every SQL4 post-merge check green (`handoff-root-m6b.md`); SQL4 is in the gate head. Gate head =
code head `06dc9e5a` (no slip). Docs head at the stop `5f48a25d` (IDX's commit) or above (the gate root's and the
PRE agent's stop commits land after this file was written: `git log`). The gate (GATE-1) was dispatched at ~15:55
to root GATE agent 1 (steps 1-7 and 9; 0 and 8 DONE) and stopped by the owner at 16:24: its handoff
`handoff-root-gate-1.md` says what ran (pending). Round 2b-ii part A (lane-delivered placeholders) was in progress
(pending). The rulings index is done. 630 MB of editor caches deleted (DISK-4). Known-red set EMPTY. Next free
build tag: whatever `handoff-root-gate-1.md` says (`p9-3f` if the gate built nothing).

No gate step after the dispatch is known complete at this writing; no P9 cell run; no OD-3 read; the prereg is
at round 2b-ii part A.

Host at dispatch: free RAM 3.65 GB of 15.7; C: about 61 GB free before the gate's builds; every pool lease
`owner=dead` except pool-32 (`owner=alive`): LEASE-DEAD, untouched.

## Tracks at the stop

| track | where | state | next |
|---|---|---|---|
| root GATE (chain) | pool-2 | agent 1 stopped at the owner's word (pending: `handoff-root-gate-1.md`) | fresh root agent 2 from `r12-prompts/gate-prompt-template.md` (`@@N@@` = 2; "read `handoff-root-gate-1.md`: resume at its Exact next step") |
| GATE-IDX | `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` | present at the stop; deletion refused to root (PERM-1), NOT re-done by the PM | OWNER deletes by hand; until then no run of record names that cache (steps 1-2 may run) |
| PRE round 2b-ii | sprint dir `p9-prereg.md` | part A DONE `7890883e` (items 14, 16-18, 31-33; Memmel SE = yes; B.10 lapse table with P9-MV / P9-W / P9-X pending the gate); its four questions ruled: COVMV-PIN, MVX-F0H, MVX-PRE13, PRE-2BI-REST | part B after the gate (a fresh PRE agent with the part-B text of `r12-post-gate.md` + those four rulings) |
| GATE-REV | - | not started | part 1 on the gate root's final hand-back (`r12-prompts/gaterev-prompt.md`), part 2 after PRE part B |
| IDX | `rulings-index.md` `5f48a25d` | DONE (483 rows; the PM re-checks before the final report) | - |
| cells | - | not started | after GATE-REV PROCEED and the PM's PRE-2BII-RULED line: `r12-prompts/cells-prompt.md` + the draft rulings appended |
| OD3-PREP | pool-21 | not dispatched: waits for the owner's OD3-DB answer | yes -> `r12-prompts/od3prep-prompt.md` during the cells; no / silence -> OD-3 "not read in P9" |
| SQL4 | merged `06dc9e5a` | checks green (SQL4-CUTOFF-2) | gate red in its 24 files attributes to `06dc9e5a` |
| A4 flips + fix-1 tests | pool-27 `87df9e00` | P10 whole (unchanged) | owner by hand if wanted |

Frozen pools (TEETH-POOL): pool-8 and pool-24 untouched. Pool-23 `build-equity/` untouched (TEETH-BIN-23).

## Open with the owner (surfaced this session; none blocks the gate's builds and suites)

1. **GATE-IDX (blocks the gate from step 3 on):** delete one file by hand:
   `del C:\atx-wt\pool-2\build-equity\mega-candidate-cache-v8-lo3\index.sqlite` (72 KB empty SQL4 index left by
   M6b's check 1b; refused to root: `m6b-logs/perm-refusals.txt` #2). While it exists the bounded runner stamps a
   `store` block into every receipt naming that cache (against SQL3-2). After deletion the cache listing is
   `155872de50833fca` again.
2. **OD3-DB (decides whether the OD-3 history read happens):** read-only access to
   `C:/atx/atx-db/data/alpha_panel/v1/{export/identity-bridge-v2-pit, delisting, fundamentals, security_master, ftd,
   regsho_threshold, short_volume_ext, thirteenf}` and `C:/atx/atx-db/data/raw/finra_short_volume` for the era
   builds (sessions 2013-2019; no write, no job). No `od3-*` era dir exists on disk. Default on silence: OD-3 "not
   read in P9: era inputs under atx-db (A4-DB, OD3-DB)", G-B8 "not evaluated", the freeze verdict says so.
3. **Clean-up owner-calls (DISK-4):** `recent-fast-validation-2023-2024-v1-fields-v4/v5` `.f64` payloads
   (1.67 GB; 2023-2024 validation data), `p9-m1d-{dbg,rel}-{u,w}` (380 MB), the ccache cap (`C:\atx-cache`,
   max_size 100 GB > free disk). After the freeze: Debug objects (~9.5 GB), `build-equity-rel` (4.1 GB), `deps/`.
4. **Permission refusals this session (PERM-1, not retried):** the PM's bulk disk-classification command
   ("Modify Shared Resources"); root M6B-FINISH's two deletions (`m6b-logs/perm-refusals.txt`: the previous M6b
   agent's scratch catalog under `...\2a54b18d-...\scratchpad\root-m6b\`, and the `index.sqlite` above). Carried
   from earlier sessions: the A4 re-apply (status-12 #1) and status-11's list.
5. **Unchanged owner items:** A4 flips (P10), frozen pools 8 / 24, leases, `C:\atx\data` (DATA-3),
   `atx-db-request.md` (AL-DATA), OD5-2 / OD5-3 / OD5-5 / OD5-7, SCRATCH-2, the broken `dev` preset,
   SPDLOG-RELINK, the P10 list (`rulings-index.md` Table 3).

## Gates and rules the next PM must hold (new this session; status-12 lists the earlier ones)

- PAR-1 (the owner's parallel note): parallel tracks wherever the hard rules allow; still ONE C++ build among our
  pools, ONE bounded real-data run per pool, ONE root agent in pool-2's working tree for code / builds / runs; docs
  agents beside root commit ONLY by pathspec (`git commit -m <msg> -- <file>`, never `git add`, never a bare
  commit); re-read before every write.
- SQL4-CUTOFF-2 (met; checks green), M6B-SCOPE, GATE-DISPATCH-12, GATE-PAR (low-RAM suites concurrently; builds and
  bounded runs alone), GATE-IDX (above), PRE-2BII-AGENT + PRE-2BII-PAR (parts A / B), GATE-REV + GATE-REV-PAR
  (parts 1 / 2), OD3-PAR (era data prep in pool-21 during the cells, only on OD3-DB yes), OD3-DB (owner),
  IDX-1, DISK-4 (the RESUME 12 clean-up ruling, re-labelled from DISK-3), R12-RESUME.
- Drafted, NOT in the ledger yet: CELLS-1 .. CELLS-6, ADOPT-1, OD3-1 / OD3-GO (`r12-prompts/ledger-cells-rulings-draft.md`):
  append before the first cell, after reading them against the gate report and the prereg.
- Everything of status-12 and earlier (TEST-1, PERM-1, LEASE-DEAD, HOST-4 / HOST-5, REV-FRESH, MERGE-SHA,
  TEETH-BIN-23, TEETH-POOL, COMPOSE-1, MV-FLAG-P0, B2-N37, TEETH-CRLF, EXE-FREEZE from the re-pin, the known-red set
  EMPTY, the one named Release skip, ledger appends by scratch file + `cat >>`, never edit earlier lines, the PM
  never commits in pool-2: DISPATCH-COMMIT).

## Next steps, in order

1. Verify the stop state: `git log` / `git status` in pool-2; read `handoff-root-gate-1.md` and the PRE agent's
   stop commit; trial ledger 133 `27e40f9f`; `lease-worktree.ps1 -Status` read-only; whether `index.sqlite` is gone.
2. Resume the gate: fresh root agent 2 from `r12-prompts/gate-prompt-template.md` (`@@N@@` = 2; the previous-handoff
   line = "read `handoff-root-gate-1.md` whole and resume at its Exact next step"; the head and tag lines updated if
   the handoff moved them). Chain through `handoff-root-gate-<n>.md`. GATE-IDX: if the owner has not deleted the
   file, the gate runs steps 1-2 and hands back BLOCKED-OWNER before step 3.
3. On the gate's final hand-back: PRE part B (continue the PRE agent or dispatch a fresh one with the part-B text
   of `r12-post-gate.md`) and GATE-REV part 1 (`r12-prompts/gaterev-prompt.md`, fill the three `@@` tokens) in
   parallel; GATE-REV part 2 after part B's commit; the PM rules both files ("Ruling PRE-2BII-RULED"; PROCEED).
4. Cells: append the draft CELLS rulings; dispatch `r12-prompts/cells-prompt.md` (fill `@@N@@`, heads, the lapse
   table B.10 as ruled, the first cell); one hand-back per cell, the PM's ruling line per cell; DERIV agents for
   the hand cells (`r12-prompts/deriv-prompt.md`); the adoption print; OD3-PREP in pool-21 during the cells iff
   OD3-DB is yes; OD-3 with the OD3-GO checkpoint; the freeze verdict (prereg G) and the final report
   (`rulings-index.md` re-checked against the ledger first).

## Rulings made this session (full text in the ledger from "- RESUME 12")

R12-RESUME, SQL4-CUTOFF-2, M6B-SCOPE, GATE-DISPATCH-12, PRE-2BII-AGENT, GATE-REV, PAR-1, PRE-2BII-PAR,
GATE-REV-PAR, GATE-PAR, OD3-PAR, IDX-1, DISK-4 (the clean-up; executed: 32 cache dirs, 630 MB), OD3-DB (owner
decision surfaced), GATE-IDX, COVMV-PIN, MVX-F0H, MVX-PRE13, PRE-2BI-REST (the PRE agent's four questions).
Verdicts recorded: M6B-FINISH checks green (SQL4 in the gate head); IDX DONE `5f48a25d`; PRE part A DONE
`7890883e`. Owner's words recorded: the /goal text of 2026-10-15 (STOP 11 refresh), the clean-up request, the
parallel note, STOP 12. Permission refusals this session: three (the PM's classification command; root's two
deletions).

## Standing warnings (in addition to status-12)

- The gate's P9-B0 and every identity run read real data: READ NO STATISTIC; the PM reads only the registered
  prints after the cells.
- Two agents may share pool-2's sprint dir (PAR-1): never let two agents stage in one index; docs agents commit
  by pathspec only; root adds explicit paths.
- The owner's hard rule on `atx-db/` is not the PM's to waive (OD3-DB): the OD-3 read waits for the owner's word
  or lapses with the stated wording.
