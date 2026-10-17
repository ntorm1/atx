# P9 RESUME 12: post-gate dispatch (PM, drafted 2026-10-15 while root M6B-FINISH ran; filled at each dispatch)

ROOT-SDD = `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`. Every agent: Opus; reads the contract
(`resume4-dispatch.md`, `resume6-addendum.md`, `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md`,
`resume10-addendum.md`, `resume11-addendum.md`, `resume12-addendum.md`), then its section here, then the files its
section names, then greps its ruling ids in `progress.md`. Hand-back under 200 words. Commit trailer
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Work ONLY in pool-2 (`cd` there first; nothing is ever run
in `C:/atx`). One agent at a time in pool-2's working tree. `@@...@@` tokens are filled by the PM at dispatch.

## PRE-2BII (one fresh PRE agent, continued by message for part B; blind; PRE-2BII-AGENT, PRE-2BII-PAR, PAR-1)

You work in pool-2's sprint dir BESIDE a running root agent (M6B-FINISH, then the gate). You touch ONE file,
`p9-prereg.md`, and commit it ONLY by pathspec: `git -C C:/atx-wt/pool-2 commit -m "<msg>" --
.superpowers/sdd/platform-p9-20261003/p9-prereg.md` (the file is already tracked; never `git add`, never a bare
`git commit`; on `index.lock` wait 10 s and retry, bounded 10 min). Re-read the file immediately before each edit
session (root does not edit it, but you re-read anyway). Blind: no 2020-2023 return / IC / Sharpe / NAV statistic is
read or written; rho and counts are not statistics.

**Part A (now; the lane-delivered half).** Read `p9-prereg.md` WHOLE (round 2b-i), Appendix IV items 1-33, the
ledger lines carrying "PRE 2b-ii list" (`progress.md` :699, :814, :815, :861, :862, :876) and the rulings PRE-MVX,
ALCLOCK-S2, COVMV-S2, NAV-S, PY-S (S-4), D3W-S, D3W-STALE, D3WPB-Q2, SIGN-SUBST, A4-P10-FLIPS, A4-DB, PRE-14, PRE-30,
PRE-34, PRE-35, PRE-36, PRE-39, PRE-40, PRE-2BII-PAR; then the merged lanes' "For PRE" blocks in pool-2's sprint dir
(`task-AL-CLOCK-report.md` + its fix-1 report, `task-COV-MV-report.md` + fix-1, `task-D3-report.md`,
`task-D3W-report.md` + `task-D3W-PB-report.md`, `task-OD3P-report.md` + its follow-up, `task-SIGN-report.md`,
`task-A4-report.md`; grep "For PRE" / "PRE" in each) and the registrations under `scripts/specs/p9/rules/` and
`scripts/specs/p9/templates/` (SHA-256 by `sha256sum`). Fill, in place, keeping the bracket form
`[ROUND 2b-ii: <value> - <source file:line or SHA>]`:
- items 14, 16, 17, 18, 31, 32, 33 (the lane blocks: B.3 sleeve line keep / cut per D3W; B.4 the P9-C block; B.5
  the P9-MV block; B.6 the P9-W block; F.1 / Appendix III `source_sample_end` of `info-clock-rate-v1` and
  `mv-aim-v1`; F.1 the era fields argv from OD3P's runbook);
- items 7, 10, 11, 13, 19, 21: replace the bracket with `[ROUND 2b-ii: filled at the cell's registration commit by
  root - Appendix IV item N]` (their value exists only then; the cells root fills it in that commit);
- the "PRE 2b-ii list" disclosures into their blocks (ALCLOCK-S2 S-A into B.4; COVMV-S2 S-b and the eigen-share
  null note into B.5; D3W-S S-6 and D3W-STALE into B.3 / B.6; A4-P10-FLIPS into B.0 / C.1: Python stays the
  producer of `ftd_shares_ratio21` / `regsho_threshold_days63`, Y-F0's fields manifest `26fee5ce` unchanged);
- PRE-MVX: B.7 "Parent" = P9-F0h; B.8; E: P9-F = P9-X's book if accepted; NAV-2STORE P10;
- PY-S S-4's note (the plan's Appendix A line is stale text; B.0 / E of this file is the form);
- E: answer the "Memmel SE" question from the v8 driver of record (which SE the v8 driver printed on the four
  cited lines: read the driver source at the v8 commit, `platform-v8-20260929` records, not a result line) and
  write the P9 ledger-citation line the placeholder asks for as a sentence in E (the PM copies it to the ledger);
- a new subsection "### B.10 Lapse table (round 2b-ii)": one row per cell (P9-L, P9-H, P9-M, P9-C, P9-MV, P9-W,
  P9-X, P9-V) with columns "lane-decided" (merged + APPROVE: yes / no, the merge commit from
  `root-wave3-merge-report.md` / the ledger) and "gate-decided" (D3-Q1a for P9-W, COMPOSE-1 for P9-MV, PRE-30 for
  P9-X: `pending the gate`), and "status" = `runs` / `lapses (0)` / `pending`;
- Appendix IV: tick each item you filled with "(filled, part A)".
Edit no pinned registration, no expected hash, no standing line (PRE-40 form: a stale line is stated beside, never
rewritten). Commit: `docs(p9): p9-prereg.md round 2b-ii part A (PRE-2BII-PAR)`. Hand back: STATUS, docs head, the
items filled, the Memmel SE answer in one line, the lapse table rows, open questions one line each. Then WAIT for
the PM's part-B message.

**Part B (on the PM's message, after the gate root's hand-back).** Read `root-gate-report.md` ("round-2b-ii facts
by Appendix IV item"), `handoff-root-gate.md` (and every `handoff-root-gate-<n>.md`), the rulings D3-Q1a,
COMPOSE-1, SQL4-CUTOFF-2, GATE-EXES, EXE-FREEZE, OD5-1, MV-FLAG-P0, the ledger from "- Dispatch root GATE" on. Fill
items 1-6, 8, 9, 12, 15, 20, 22, 23, 25, 26 and every "root" item; the outcomes of D3-Q1a (P9-F0h with
`--engine-fit` iff identity held, else P9-W lapses, PRE-35, stated in B.6), COMPOSE-1 (P9-MV lapses on a refusal,
B.5), SQL4-CUTOFF-2 (C.13), the eval and fit modes (B.0); complete B.10's gate-decided columns; a placeholder the
gate did not deliver is filled `[ROUND 2b-ii: NOT DELIVERED - <why>; the cell <id> lapses / the line is cut per
<ruling>]`, never left open; add "## Round 2b-ii record" at the end: the gate head, the re-pin's exe table (tag,
SHA-256, source commit per exe), the reference dirs with SHA-256, the `ucrtbase.dll` record, rho of the P9 pairs
(value and the "1 - rho < 1e-6" flag only), the eval and fit modes, the gate's catalog digest, every cut line.
Re-state any part-A line the gate contradicts. Commit: `docs(p9): p9-prereg.md round 2b-ii part B (PRE-2BII-AGENT)`.
Hand back: STATUS, docs head, placeholders filled / NOT DELIVERED counts, the lapse table, every cut line, open
questions one line each.

## GATE-REV (one fresh reviewer; blind; after PRE-2BII's commit; GATE-REV)

State at dispatch: docs head `@@PRE-DOCS-HEAD@@`. Read `root-gate-brief.md`, `r11-dispatch.md` "ROOT GATE" +
"ROOT GATE: rulings from the whole-wave review", `r12-dispatch.md` "ROOT GATE", then `root-gate-report.md`,
`handoff-root-gate.md` (and every `handoff-root-gate-<n>.md`), `integration-log.md` (the gate rows), then
`p9-prereg.md` (round 2b-ii) and `r12-post-gate.md` "CELLS".

Part 1, `gate-report-review.md`: for every step 0-9 of the brief and every ruling of the "ROOT GATE: rulings" list,
one row: claimed / evidence path / checked (the file exists, its exit code or count or SHA says what the report
claims; re-run nothing that reads real data; you may re-run a gtest filter or a pytest file the report names) /
verdict. The whole-suite counts (0 failed, the named skips listed), the identity results (byte-identical outside the
allowed paths; D3-Q1a's stripped-weights SHA pair; COMPOSE-1's parse check), P9-B0 (substitution list committed
first; every reference dir with SHA-256; READ NO STATISTIC: you check that the report prints none), the re-pin
commit's diff (pins only; `exes.eval`; EXE-FREEZE from it), the store step (digest reproduced by a second catalog;
`verify --pins` counts; the `classes.json` SHA by hand), the scoreboard (`ledger_n` vs the verb's N: "equal"),
G-P ticks with evidence rows, the CRT record, rho logged with the flag only, 0 trials (133 / `27e40f9f`), the SQL4
verdict (SQL4-CUTOFF-2) and any BLOCKED attribution by merge commit. Verdict: PROCEED (the cells may start) or
RETURN (the step and the lane by merge commit; what root must redo). A step the report leaves half-recorded is
RETURN.

Part 2, `prereg-2bii-review.md`: every `[ROUND 2b-ii: ...]` placeholder filled with a value whose source exists and
says so (check each by path / SHA); the lapse table B.10 agrees with the gate report and the rulings (PRE-14,
PRE-30, PRE-35, D3-Q1a, COMPOSE-1, PRE-MVX); every cell block's argv is final: the parent named, the template /
hand-spec SHA-256 pinned, `--seal-allow`, `--ann-mean-gross` and (P9-C) `--info-clock-print` named alike on cell and
reference, no cache index flag (SQL-11), no `--risk-model` outside P9-MV / P9-X / P9-V, the store pins equal
`handoff-root-m2i.md`'s; the G-B7 step table complete (the "Memmel SE" question of E answered from the v8 driver of
record: state what you found); the OD-3 runbook F.1 consistent with the lapse table (P9-F0h). Findings as
Required / Suggested with file:line; verdict APPROVE / CHANGES_REQUIRED. You edit nothing.

One commit of the two files (`git add -f`). Final message: the two verdicts, Required findings one line each.

## CELLS (the cells root agent; a chain through `handoff-root-cells-<n>.md`; CELLS-1 .. CELLS-6)

Start state at dispatch: code head `@@CELLS-CODE-HEAD@@`, docs head `@@CELLS-DOCS-HEAD@@`; the exes of record =
the re-pin's table in `handoff-root-gate.md` (EXE-FREEZE: nothing is rebuilt; a needed rebuild = STOP, hand back);
the references = the P9-B0 dirs of that file; trial ledger 133 / `27e40f9f`; `p9-prereg.md` at round 2b-ii, ruled
(the ledger line "Ruling PRE-2BII-RULED"); `gate-report-review.md` PROCEED; the lapse table B.10 (what runs).
Read `p9-prereg.md` WHOLE: it is the registration and the runbook of every cell; this section only orders the work
and fixes the hand-back protocol. Then `handoff-root-gate.md`, `root-gate-report.md`, `root-gate-brief.md` step 4
(the run conventions), the v8 precedent lines of the ledger (:156 X-10, :167 Y-1, :184 the re-derivation: the form of
a hand-cell record), the rulings CELLS-1 .. CELLS-6, PRE-2 .. PRE-40, B2-13, PRE-12, PRE-33, SEAL-ALLOW-3, PRE-7a,
ALCLOCK-1, COV-9, RUN-1, RUN-2, COVR-1, EXE-FREEZE, EXE-CUR, CRT-2, SQL-11, OD5-1, MV-FLAG-P0, E2-14, E2-15, E2-16,
E2-17, C2-6, PRE-27, PRE-37.

Order (A.7): P9-L -> P9-H -> P9-M -> P9-C -> P9-MV -> P9-W -> P9-X -> P9-V -> adoption print (E) -> OD-3 (F) ->
freeze (G, the PM). A lapsed cell is skipped with a ledger line of its own (the PM's); the order never changes.

Per cell, in this order, and NOTHING of the next cell before the PM's ruling line for this one:

0. **Pre-cell checks** (B.0 "Void" items 1-7; C.5, C.6, C.8, C.13, C.14, C.16): `ucrtbase.dll` version + SHA-256
   equal to the P9-B0 record; every exe the cell names present with the re-pin's SHA-256 (re-hash on disk;
   EXE-FREEZE); timings off; no cache index flag on the argv (grep); `--seal-allow` only after the sprint dir's `tools/sealsrc.py`
   (ROOT-SDD `tools/sealsrc.py`) shows every hit is a wall-clock stamp (SEAL-ALLOW-3); for a `--risk-model` cell (P9-MV, P9-X, P9-V) the role's
   `universe.delisting.returns_applied == false` (COV-9) and the store's manifest SHA-256 equal to its pin (B.5 /
   B.7); P9-L's S2-6 cache check (no `imputed-v1` entry from a pre-`110fd735` build under any cache root the cell
   reads); the trial ledger's line count and SHA before the run; free RAM and disk.
1. **Registration commit** (before any read): v2 cells (P9-H, P9-M, P9-C, P9-MV, P9-W): the template's `parent` set
   to the last accepted book's cell of record, the stated text edits only (B.2: the "ex-ante" cut; B.3: PRE-8's
   description and the sleeve sentence per D3W's outcome), the template re-pinned in its registration
   (`template_sha256`); the allowed diff shown by JSON diff (`/parent` + the stated cuts) in the commit body; the
   registration's `manifest_note` is NOT edited (PRE-40); the template names `gross_match: pm6-6-engine` (checked).
   Hand cells (P9-L, P9-X, P9-V): the spec written from its block (B.1 `label-terminal.json`; B.7 / B.8 hand specs
   from P9-F0's NAV argv with the stated change only), the scaled mechanics limits and the verdict arithmetic written
   in the cell's plan file (`cells/<id>-plan.md` in the sprint dir: the R0-10 form: r, the gross band, |net|, tau
   mean, p95 limits; for P9-L the G-B9 limits unscaled), committed with SHA-256s before the run.
2. **Run** through the driver of record (v2: `research_cycle.py` wave of the registered template; hand: the v1
   cycle `run` of the spec), every real-data run through `scripts/run_bounded_research.py` (`--seconds` <= 600 per
   stage, the registered phase caps of C.14; longer work split by stage, RUN-2), `--max-rss-mib 2048` unless C.14's
   known need applies (state the free-RAM figure). P9-L: the calibration run (`-cal`) first, `matched` true, then
   the cell of record at the published L-prime with no calibrate flag (PRE-3, PRE-27, C2-6). P9-X: if NO P9 cell is
   accepted before it, no run: the re-based Y-1 reference is the cell of record (PRE-30) and the PRE-34 agreement
   check (its S2 net Sharpe and net annual return vs v8's ledgered Y-1 at the ledger's printed precision: agree /
   differ by path, the figures themselves to the record only) is done before the adoption print; a difference beyond
   one unit in the last printed place = STOP, the PM rules. Every argv recorded verbatim in the cell's record.
3. **rho before any paired read** (B2-13): the pair's rho logged (value and the "1 - rho < 1e-6" flag) in the cell
   record and the hand-back; the fallback rule of C.11 applies as fixed (PRE-12 / PRE-33 / B2-13 per cell).
4. **Verdict.** v2 cells: the driver's registered rule (`pm7-34`: paired S2 net dSR > 0 AND mechanics `v8-mech`;
   `scripts/wave_rules.py`) as the driver prints it; P9-W: no verdict (decides nothing; its dSR printed). Hand cells:
   the verdict derived by you from the output files against the plan file's limits (P9-L: mechanics only, PRE-5;
   P9-X: X-10's rule at the scaled limits, B.7; P9-V: paired dSR > 0 vs P9-X AND mechanics at P9-X's scaled limits,
   B.8), written in the cell record as the R0-10 / R0-11 lines are written; then STOP for the DERIV agent (CELLS-2):
   the PM dispatches a fresh agent that re-derives the verdict from the same files before the trial-ledger line is
   written; you write nothing to the trial ledger for a hand cell until the PM's message says "DERIV agrees".
   (For a v2 cell the driver writes the trial-ledger line itself at judge time: that is the registered route.)
5. **Record and hand-back** (`cells/<id>-record.md`, committed; the docs commit carries the ledger as it stands):
   the registration commit SHA; every argv; every output dir with the SHA-256 of every file; the trial-ledger line
   count and SHA before / after (and the new line's trial id); the verdict line with the deciding figures ONLY (the
   dSR, its SE and p, the mechanics rows vs their limits; for P9-X the net return and Sharpe comparisons; nothing
   else); the printed-only lines of the cell's block (each with its producer); the registered predictions printed
   against outcomes; the Appendix A block (E); rho; the identity-bridge flag line; the void checklist (B.0) each
   "held". Hand back to the PM in under 200 words: STATUS, the cell id, the verdict and its deciding line (the
   figures of the verdict line are allowed in the hand-back: the PM reads the registered prints), the record path,
   the trial ledger's new count / SHA, the next cell's parent as you understand it, open questions. Then WAIT: the
   PM's reply names the ruling (ACCEPTED / REJECTED / UNDEFINED / VOID / LAPSED) and the next cell.

Stops (hand back, run nothing further): a void condition of B.0 that fails; an exe or `ucrtbase.dll` mismatch; a
driver refusal at preflight; an undefined parent (the cell is UNDEFINED at 0, logged, the order continues); a
PRE-34 disagreement; a hand-cell verdict the DERIV agent disputes; any 3221225794 exit (ENV-1: re-run once on a
quiet host, both runs recorded); a permission refusal (PERM-1). Never retry a rejected cell (E-45); never
re-parameterise; never read a statistic of a variant not in section B.

**Adoption print (E; ADOPT-1)** after the PM's ruling on the last defined cell: the pinned verb (`nav_summ ...
--engine-stats <exes.eval>`, its SHA-256 printed from `nav_summ_run`, B2-11), every line G-B1 .. G-B9 as the E table
names its producer; G-B5 from the verb (`dsr.tot.tot`, `house_v1`, DSR_hand per PRE-15's state); G-B6 the bundle
R-2 vs P9-F0 (seed 20260929, 4,999 resamples, one-sided); G-B7 the verb's `conditional` block with `--step` rows
(PRE-36's list: the v8 steps + the accepted P9-H / P9-M / P9-C / P9-MV; P9-L, P9-X, P9-V, P9-W are not steps); G-B9
mechanics; the lines printed beside (PBO both readings, the year table, the side-by-side books, each cell's verdict,
P9-W's dSR, the spo-v2 prior line, the power line, ALD-DIAG, the cost-model line); the Appendix A block; the
identity-bridge flag on G-B1 / G-B2 / G-B8; P9-F0 and P9-F0h side by side if they differ (PRE-10); P9-F per PRE-13.
Record `adoption-print.md` (the print verbatim, every producer and SHA), committed; hand back the G-B1 .. G-B9
verdict lines only.

**OD3-PREP (OD3-PAR; pool-21; dispatched ONLY on the owner's word under OD3-DB, in parallel with the cells):**
`p9-od3-runbook.md` section 2 steps P1 (stage pins re-hashed against `build-equity/train-2020-2023-lo3/manifest.json`
and Y-F0's fields manifest `26fee5ce`), B1 (projection 2013-06-01 .. 2020-01-01), B2-B5 (era base roles with the
TRAIN argv and era dates; the factor-break scan's session list handed to the PM for review BEFORE the repair, OD3P
runbook "STOP if unreviewed"), B7 (era fields from the TRAIN v15 receipt's command with exactly the three values
replaced and the reuse options removed; the self-check first; a 600 s refusal = staging as the TRAIN v15 lineage
did, `-v15a` then `--reuse`, each stage under 600 s, the PM's standing word), writing under pool-21
`build-equity/od3-*` only, every run through the bounded runner on the frozen exes of pool-2's re-pin by absolute
path (EXE-CUR), reading the atx-db stage paths READ-ONLY and only as the TRAIN receipts bind them, opening no
session dated 2024-01-01 or later; yields (waits, 60 s polls) whenever a cell's bounded run or a build is in flight
on the host; records `od3-prep-report.md` (every receipt, every manifest SHA-256, the era member counts per scored
session, the factor-break list) in pool-21's sprint dir, committed there. The cells root then link-copies the era
dirs by name into pool-2 `build-equity/od3-*` (hard links on the same volume; manifests re-hashed, P1 form) before
B6.

**OD-3 (F.1; OD3-1, OD3-GO)** after the PM's "adoption print read" line (and only if OD3-DB is answered yes; else
the OD-3 step records "not read in P9: era inputs under atx-db (A4-DB, OD3-DB)" and G-B8 "not evaluated"): the runbook F.1 step by step on P9-F0h
(and Y-F0, R-2, the clean-source book beside), through OD3P's merged tooling (`scripts/research_roles.py`,
`carry_weights.py`, `nav_summ --pool`, the era tools of F.2), `atx-impl/tools/holdout_gate.py` NEVER (PRE-21; the
owner's rule reads "except as F.1 orders it": F.1 orders it never). MV-FLAG-P0 at the P0 step as ruled. Steps B1-B7,
B6, lock and plan, then P1-P7 pre-read checks: STOP and hand back the P1-P7 results (every pin equal, the four
books' TRAIN NAVs reproduced byte for byte on the frozen build, score_begin 399, era ends, E1 member counts, the
audit, K1 caps, the plan's eras and carry) BEFORE R0; the PM's "OD3-GO" line releases R0-R4. Any stop condition of
F.1 = hand back, read nothing. Record `od3-read.md` (predictions against outcomes, the four history lines, G-B8's
two tests with both p, coverage by year, the contamination flags, the identity-bridge flag), committed; hand back
the G-B8 line and the prediction / outcome table.

## DERIV (a fresh agent per hand cell; CELLS-2)

Filled at dispatch: the cell id, the record path, the plan file, the output dirs. Read the cell's plan file and the
prereg block of the cell ONLY (not the cell record's verdict line: the PM's prompt names the files you may open);
derive the verdict from the output files against the plan file's limits; write `cells/<id>-deriv.md` (your
derivation, every figure you used with its file path, your verdict, an anchoring disclosure: what you saw before
deriving); commit (`git add -f`). Final message: your verdict and the deciding line; agree / disagree with nothing
(you have not seen the root's line); open questions.

## FREEZE (the PM)

The PM ticks `p9-prereg.md` section G from the records (`adoption-print.md`, `od3-read.md`, the gate report, the
integration log, the ledger), writes `freeze-verdict.md` (plan 4.4 / prereg G; wording "passed with the
identity-bridge flag" on a pass; a G-B8 fail stated, nothing re-run) and `final-report.md` (every ruling of the
sprint with its line, the owner items, the P10 list, `main` / `origin/main` untouched, the permission refusals
with exact commands, the trial ledger's final count / SHA, `al-data/atx-db-request.md` handed over). A fresh root
agent commits them (DISPATCH-COMMIT) and releases nothing (LEASE-DEAD: leases stay as they are; G item 5 is stated
as the owner's).
