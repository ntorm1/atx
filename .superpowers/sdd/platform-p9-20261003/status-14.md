# P9 sprint: PM handoff 14 (owner stop 13, 2026-10-16 13:20 EDT)

For the next parent (PM) agent. Supersedes `status-13.md`. The goal prompt to start the next session is in
`next-goal-prompt.md` (same dir, refreshed at this stop). Written at the owner's stop ("stop here and write a handoff
file") while the GATE-REV reviewer was in part 2 (the prereg review); its stop record is marked (pending) where the
hand-back had not arrived when this file was written: the ledger block from "- RESUME 13" (line 952) to the end
records every hand-back and ruling of this session, and `prereg-2bii-review.md` as committed is the truth for part 2.
Verify every head with `git log` before dispatch. The PM's "~" clock times in the ledger from about :996 on ran
ahead of the wall clock by roughly an hour (the shell's `date` at the stop line said 13:20 EDT); the order of the
lines is right.

## Read order

1. This file.
2. The contracts every agent gets: `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md` .. `resume12-addendum.md` (the last wins). The dispatch files of this session, all in
   `r13-prompts/`: `gate-prompt-2.md`, `gate-prompt-3.md`, `gate-prompt-4.md` (the gate chain's facts: every one
   still binds the cells where it names them), `pre-2bii-b-prompt.md`, `idx-check-prompt.md`, `gaterev-prompt-1.md`
   (filled), `cells-prompt-1.md` (READY but for `@@DOCS-HEAD@@`), and the unchanged `r12-prompts/` set
   (`deriv-prompt.md`, `od3prep-prompt.md`, `ledger-cells-rulings-draft.md` = CELLS-1 .. 6, ADOPT-1, OD3-1 / OD3-GO:
   NOT yet in the ledger; append with `cat >>` at the cells dispatch, before any cell). `r12-post-gate.md` (the CELLS
   runbook, DERIV, FREEZE).
3. The ledger `progress.md` from "- RESUME 13" (:952) to the end. Every "Ruling" line is binding; REPIN-UW and
   REPIN-R2 are superseded by REPIN-REV.
4. `handoff-root-gate.md` (the gate's FINAL handoff: the exe table of the re-pin, the six reference dirs with
   SHA-256, the substitution list, `ucrtbase.dll`, the cache listings, "what the cells agent must know"),
   `root-gate-report.md`, `gate-report-review.md` (part 1: RETURN R-1, closed by `d78f5cc2`; "PROCEED" line pending
   part 2's commit), `prereg-2bii-review.md` (part 2; pending at the stop).
5. `p9-prereg.md` at `a40db317` (round 2b-ii COMPLETE), `rulings-index.md` + `rulings-index-check.md` (IDX-FINAL),
   `p9-od3-runbook.md`.

Plan: `docs/plans/2026-10-03-p9-sprint-plan.md`. Root tree `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`.

## State in short

GATE-1 COMPLETE (four chain agents; every step holds; 0 failed in every suite that ran; TEST-2 cut nothing that
had not already run). Re-pin of record `2078f877` (pins only; a second pins commit `3a1a4759` was reverted by
`f8a85ad4`, REPIN-REV); EXE-FREEZE runs from `2078f877`. Code head `f8a85ad4`. Docs head at the stop `d78f5cc2`
(R1-FIX) or above (the reviewer's part-2 commit lands after this file). Gate review part 1: RETURN on records only
(R-1, the OD5-1 flag line per reference), closed by `d78f5cc2`; part 2 in flight at the stop. Prereg round 2b-ii
COMPLETE (`7890883e` A, `aef2e033` B1, `1e2dbf3c` B2a, `a40db317` B2b); the registration-commit placeholders (items
7, 10, 11, 13, 19, 21, the registration parts of 16-18) and the cell-time items stay open by design. Rulings index
re-checked (`rulings-index-check.md` `73be0a13`; IDX-FINAL at the freeze). No P9 cell run; no OD-3 read; trial
ledger 133 / `27e40f9f`; `build-equity/research-store/` absent; `index.sqlite` gone (the owner); known-red set EMPTY.

Lapse table of record (Ruling LAPSE-TABLE): P9-L, P9-H, P9-M, P9-C, P9-MV, P9-V RUN; P9-W LAPSED (0) (D3-Q1a
DIFFERENT; the ledger line "Cell P9-W: LAPSED (0)" is written); P9-X pending PRE-30 / PRE30-MVX. Fit mode of every
v2 cell: flag-absent. Parent of the first cell: the re-based Y-F0 (`p9-b0g-yf0-nav`; the cycle's u / w resume roots
are the v8-era dirs pinned in `lib-v8ysb-gm.json`, byte-identical to `p9-b0g-yf0-u` / `-w`). R-2 = `p9-b0-r2-nav`
(FIELDS-SUB-A, read by path). Y-1 = `p9-b0g-y1-nav` (the PRE-34 printed-precision check is the cells root's at the
P9-X step, PRE34-WHO). X-10 = `p9-b0g-x10-nav`. rho logged: P9-X pair 0.9947035686613187, X-10 / Y-F0
0.9994863034956717, Y-1 / X-10 0.9947113980219978 (none degenerate).

## Tracks at the stop

| track | where | state | next |
|---|---|---|---|
| root GATE chain | pool-2 | DONE (agents 1-4; `handoff-root-gate.md`); released | - |
| GATE-REV part 1 | `gate-report-review.md` `81900ce9` | RETURN R-1 (records); closed by root `d78f5cc2`; "PROCEED" line = part 2's commit | (pending) |
| GATE-REV part 2 | `prereg-2bii-review.md` `6c608ff3` | DONE after the stop line: CHANGES_REQUIRED, one Required RQ-1 (no route puts `--ann-mean-gross` / `--info-clock-print` onto the driver-built cell NAV argv; every v2 cell VOID under B.0 (2) until fixed); R-1 closed: gate PROCEED | Ruling FLAGS-ROUTE (ledger, the last lines): a root agent's one-commit spec fix + verify + one bounded `--spec` NAV identity; a fresh scoped reviewer re-checks RQ-1 (REV-FRESH); then PRE-2BII-RULED |
| PRE | `p9-prereg.md` `a40db317` | COMPLETE; the agent released | after FLAGS-ROUTE's re-check: the PM's "Ruling PRE-2BII-RULED" (a PRE docs agent restates B.0 / B.4 "the route" if the reviewer asks) |
| IDX-CHECK | `rulings-index-check.md` `73be0a13` | DONE (5 unfaithful gists, 6 missing rows, 4 missing items; delta from :952) | IDX-FINAL at the freeze |
| cells | - | not started; `r13-prompts/cells-prompt-1.md` READY (fill `@@DOCS-HEAD@@`) | after PRE-2BII-RULED + PROCEED + the draft CELLS rulings appended: dispatch cells root agent 1; first cell P9-L |
| OD3-PREP / OD-3 | - | OD3-DB unanswered: default "not read in P9" | only on the owner's yes |
| freeze | - | - | after the last cell: adoption print, (OD-3), `freeze-verdict.md`, `rulings-index-final.md` (IDX-FINAL), `final-report.md` |

Frozen pools 8 / 24 untouched (TEETH-POOL); pool-23 `build-equity/` untouched (TEETH-BIN-23); pools 33 / 34 / 36 are
foreign atx-vol sessions (HOST-6; leases `owner=alive`, never touched); every P9 lease `owner=dead` except pool-32
(LEASE-DEAD).

## Open with the owner

1. **OD3-DB** (decides whether the OD-3 read happens): unanswered; default on silence stands (OD-3 "not read in P9",
   G-B8 "not evaluated", the freeze verdict says so).
2. **FIELDS-GONE (record only):** `build-equity/train-2020-2023-lo3-fields-{v9,v10,v13,v14,v15a}/` and v11's
   payloads were deleted from disk between 2026-10-15 ~15:00 and 2026-10-16 ~09:40 by no ledgered action; the gate
   worked around it (R-2 by fields substitution on v15, witness 70 / 70; X-5's identity cut). Who deleted them is the
   owner's to say. Consequence still live: FIELDS-RISK (Y-F0's own cycle "alone" resolves a `baseline_fields` v13
   through its template chain; as a parent it resolves; a cell preflight refusal on it = a driver stop, the PM rules).
3. **Clean-up owner-calls (DISK-4, unchanged):** the v4 / v5 `.f64` payloads (1.67 GB), `p9-m1d-{dbg,rel}-{u,w}`
   (380 MB), the ccache cap; after the freeze: Debug objects (~9.5 GB), `build-equity-rel` (4.1 GB), `deps/`, the
   Release subtree of the real cache (4.47 GB, regenerable; GATE-RELCACHE).
4. **Permission refusals this session (PERM-1):** two: root GATE agent 2's unstage of the editor-side
   `gate-logs/tools/.mypy_cache` (`gate-logs/perm-refusals.txt`; closed by the owner's hand before `1e2dbf3c`), and
   the GATE-REV reviewer's deletion of its four scratch scripts under the PM session scratchpad
   `C:/Users/natha/AppData/Local/Temp/claude/c--atx/cbb67823-1fc6-4abf-a38b-a72955c9e69f/scratchpad/gaterev/`
   (`mask.py`, `rehash.py`, `decscan.py`, `paths.py`; regenerable, nothing reads them; yours to delete or leave).
   The PM had none. Carried from earlier sessions: status-13 #4 (the `index.sqlite` deletion: done by the
   owner on 2026-10-16; the old scratch catalog; the PM's classification command; the A4 re-apply; status-11's list).
5. **Unchanged owner items:** A4 flips (P10), frozen pools 8 / 24, leases, `C:\atx\data` (DATA-3),
   `atx-db-request.md` (AL-DATA), OD5-2 / OD5-3 / OD5-5 / OD5-7, SCRATCH-2, the broken `dev` preset, SPDLOG-RELINK,
   the P10 list (`rulings-index.md` Table 3 + this session's: SQL4-COLD-P10 n/a, the `test_mega_report_v8.py` TMP-length
   fragility (GATE-TMP-2), the Release cache subtree clean-up, P9-W's D3-Q1a finding, NAV-2STORE).

## Gates and rules the next PM must hold (new this session; status-13 lists the earlier ones)

- TEST-2 (the owner, 2026-10-16): no large suite runs; a single named test only where a step needs it as its own
  evidence. The cells run no suite.
- The owner's /model switch to Fable 5.1 changes nothing for subagents: Opus, trailer `Claude Opus 5.5`.
- GATE-TMP-2 (pytest TMP roots outside the tree, short), GATE-MYPY (explicit pathspecs + the exclude on every
  commit; the editor's type-checker writes into the sprint dir), HOST-6 (pools 33-36 foreign), GATE-RELCACHE +
  RELCACHE-ALLOW (the Release cache subtree; the allowed provenance classes), COMPOSE-1a / COMPOSE-PRE, FIELDS-SUB /
  FIELDS-SUB-A / FIELDS-X5 / F1-FIELDS, LAPSE-TABLE, PRE30-MVX, PRE34-WHO, COVMV-PIN / COVMV-PIN-2, B8A-PROD /
  B8A-PROD-2, D3Q3-ARGV / D3W-TPL / D3W-V3READ, PW-LAPSE-PRINT, SEAL-REF, REPIN-REV (supersedes REPIN-UW /
  REPIN-R2), OD3-FIELDS-RISK, GP-TICKS (G-P5 PARTIAL; G-P1 ticked at P9-H by the cells root), R1-FIX / R2-IDENTITY /
  R1-WHERE, IDX-FINAL (`rulings-index-final.md` at the freeze; refusals labelled `PERM-R<n>`; DISK-4 cited).
- Drafted, NOT in the ledger yet: CELLS-1 .. CELLS-6, ADOPT-1, OD3-1 / OD3-GO (`r12-prompts/ledger-cells-rulings-draft.md`):
  they are consistent with LAPSE-TABLE and the gate records (the PM read them against both); append before the first
  cell.
- Everything of status-13 and earlier (PAR-1 and its family, TEST-1's attribution rule, PERM-1, LEASE-DEAD, HOST-4 /
  HOST-5, REV-FRESH, MERGE-SHA, TEETH-BIN-23, TEETH-POOL, MV-FLAG-P0, TEETH-CRLF, EXE-FREEZE from `2078f877`, the
  known-red set EMPTY, the one named Release skip, ledger appends by scratch file + `cat >>`, never edit earlier
  lines, the PM never commits in pool-2: DISPATCH-COMMIT: the next root agent's step 0 commits `progress.md`,
  `next-goal-prompt.md`, `status-14.md`, `r13-prompts/` as they stand).

## Next steps, in order

1. Verify the stop state read-only: `git log` / `git status` in pool-2 (code `f8a85ad4`; docs `d78f5cc2` or the
   reviewer's part-2 commit above it); trial ledger 133 / `27e40f9f`; `research-store/` absent; `index.sqlite`
   absent; `lease-worktree.ps1 -Status`; the reviewer's stop record (the ledger's last lines, or
   `prereg-2bii-review.md` as committed).
2. RQ-1 (FLAGS-ROUTE): dispatch ONE fresh root agent (Opus) in pool-2: step 0 commits the PM's files (DISPATCH-COMMIT:
   `progress.md`, `status-14.md`, `next-goal-prompt.md`, `r13-prompts/`); then the one-commit spec fix (the two
   flags into `lib-v8ysb-gm.json` `nav.flags`; children checked for `nav.flags` overrides), `verify` on the pinned
   lineage, the fixture chain digest, ONE bounded Release NAV re-run of Y-F0 through `--spec` compared by SHA-256 to
   `p9-b0g-yf0-nav` (allowed `/producer/*` only), records (`root-gate-report.md` addendum, `handoff-root-gate.md`
   code head), docs commit. A difference = STOP, the PM rules. Then a fresh scoped reviewer (Opus; REV-FRESH: reads
   `prereg-2bii-review.md` first) re-checks RQ-1 only, appends its verdict to that file by pathspec.
3. The PM rules: "Ruling PRE-2BII-RULED" (the prereg at `a40db317`, with B.0 / B.4 restated by a PRE docs agent iff
   the re-check asks) and records "GATE-REV: PROCEED" (already appended to `gate-report-review.md` at `6c608ff3`).
4. Cells: `cat r12-prompts/ledger-cells-rulings-draft.md >> progress.md`; fill `@@DOCS-HEAD@@` in
   `r13-prompts/cells-prompt-1.md`; dispatch cells root agent 1 (Opus) with the wrapper form of this session
   ("your full dispatch text is the file ...; read it WHOLE; never ask mid-run; no subagents"); one hand-back per
   cell, the PM's ruling line per cell ("Cell <id>: ACCEPTED / REJECTED / UNDEFINED (0) / VOID / LAPSED (0) per
   <record path>", naming the next cell's parent); DERIV agents for P9-L, P9-X (if it runs), P9-V
   (`r12-prompts/deriv-prompt.md`, fill its tokens); the adoption print (ADOPT-1); OD-3 per OD3-DB (default: not
   read); the freeze: `freeze-verdict.md`, `rulings-index-final.md` (IDX-FINAL), `final-report.md`, committed by a
   fresh root agent (DISPATCH-COMMIT); leases untouched (G item 5 is the owner's).

## Rulings made this session (full text in the ledger from "- RESUME 13", :952 on)

R13-RESUME, GATE-TMP, GATE-RELCACHE, COMPOSE-1a, HOST-6, PRE-2BII-B1, D3Q3-ARGV, D3W-TPL, D3W-V3READ, F1-FIELDS,
COMPOSE-PRE, IDX-FINAL, PRE30-MVX, COVMV-PIN-2, B8A-PROD, TEST-2 (the owner's note), GATE-TMP-2, GATE-MYPY,
FIELDS-SUB, FIELDS-X5, LAPSE-TABLE, GATE-3-ORDER, FIELDS-SUB-A, RELCACHE-ALLOW, PRE34-WHO, YF0-NAV-ORDER,
PRE-2BII-B2A, PW-LAPSE-PRINT, B8A-PROD-2, SEAL-REF, REPIN-UW and REPIN-R2 (superseded), GP-TICKS, REPIN-REV,
OD3-FIELDS-RISK, R1-FIX, R2-IDENTITY, R1-WHERE. Findings: FIELDS-GONE, FIELDS-RISK. Cell line: P9-W LAPSED (0).
Verdicts recorded: GATE-1 DONE; GATE-REV part 1 RETURN R-1 (records) closed; prereg 2b-ii COMPLETE; IDX-CHECK DONE.
Owner's words recorded: the /goal text of 2026-10-16 (STOP 12 refresh), the 2026-10-16 testing note (TEST-2), the
/model switch, STOP 13. Owner actions observed: `index.sqlite` deleted, the mypy cache unstaged (both 2026-10-16,
by hand); the fields dirs deleted (FIELDS-GONE, unattributed). Permission refusals this session: one (root GATE
agent 2, the mypy unstage; closed by the owner).

## Standing warnings

- The cells read real data: READ NO STATISTIC outside the registered prints; the PM reads only the cells' verdict
  lines, mechanics rows and printed-only lines, and the adoption print's G-B lines.
- Two agents may share pool-2's sprint dir (PAR-1): docs agents commit by pathspec only; root adds explicit paths
  with the GATE-MYPY exclude; never a bare `git commit`.
- The owner's hard rule on `atx-db/` is not the PM's to waive (OD3-DB); the fields dirs are not rebuilt (their
  builder reads atx-db).
- FIELDS-RISK may surface at P9-L's preflight: it is a driver stop (UNDEFINED (0) as B.0 words it), the PM rules.
