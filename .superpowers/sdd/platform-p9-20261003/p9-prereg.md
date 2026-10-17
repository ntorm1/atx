# P9 pre-registration (`p9-prereg.md`)

ROUND 2b-ii COMPLETE (parts A, B1, B2a, B2b); pending GATE-REV part 2 and the PM's ruling PRE-2BII-RULED.
[Part B3: GATE-REV part 2 returned CHANGES_REQUIRED (`prereg-2bii-review.md`, `6c608ff3`; RQ-1); RQ-1 closed by
`f8d852f3` (FLAGS-ROUTE); pending the scoped re-check and PRE-2BII-RULED.]

ROUND 2b-i PARTIAL (owner stop 2026-10-06): applied PRE-32 (B.0, B.7, D), PRE-33 (B.7, B.8, C.11), PRE-34 (A.6, B.7, D, E G-B2), PRE-35 as D3-2 reads it (B.0, B.3, B.6), PRE-36 with the G-B7 step values of R-2, X-2, X-3, X-5 (E), PRE-37 (A.4), PRE-38 (B.2, B.3, B.5), PRE-39 (A.4, B.6), PRE-40 (B.2, B.3), COV-14 (B.5, B.8 (e), C.17), ALCLOCK-1 / -2 / -3 (B.0, B.4, A.4), C3-13 (B.4, B.5), D3-1 (B.6), D3W-EVAL (B.3), GATE-1 (Conventions, C.8, C.15), placeholders of A-E relabelled ROUND 2b-ii; not yet applied: PRE-39, ALCLOCK-1, ALCLOCK-2, OD3P-1 and ABS-1 in F.1 / F.2, GATE-1 in G (item 7), C3-11 (G item 3), the extension of H (H12 still points to round 2a's item 2), Appendix II status of items 2 and 8, ALCLOCK-2 in Appendix III, the Appendix IV index, and the replacement of "Open for the PM (round 2a)" by "Rulings applied in round 2b-i" (that section still stands as round 2a wrote it); the placeholders of F.1, Appendix III and Appendix IV still read `[ROUND 2b: ...]`. [read (round 2b-ii part B1; PRE-2BI-REST, ledger :951): this "not yet applied" list is closed: ALCLOCK-2 in Appendix III, the Appendix IV index and the F.1 / Appendix III / Appendix IV tags were done in part A (no `[ROUND 2b: ...]` tag is left); every other item is applied in part B1, listed in the "ROUND 2b-ii part B1" paragraph below, each "(applied, 2b-ii part B1)"]

**ROUND 2b-i (rulings applied; values pending 2b-ii).** Lane PRE, round 2b-i (dispatch R8-1, `r8-dispatch.md`
"PRE-2b-i"; Ruling PRE-2B-SPLIT, ledger :646), doc agent, 2026-10-06. Round 2b-i writes into sections A-H the rulings
that answered round 2a's "Open for the PM" items (PRE-32 .. PRE-39) and the RESUME 7 / RESUME 8 rulings that bind the
cells (COV-14, PRE-40, ALCLOCK-1 / -2 / -3, OD3P-1, D3-1, D3-2, GATE-1, C3-11, C3-13; PRE-31 item 10 was in since round
2a), copies the G-B7 step values of R-2, X-2, X-3, X-5 from their accepted v8 ledger lines (PRE-36; E), and relabels
every placeholder still waiting `[ROUND 2b-ii: ...]` (index: Appendix IV). The list of what was applied where is
"Rulings applied in round 2b-i" at the end (it replaces round 2a's "Open for the PM") [read: written in round 2b-ii part B1 as "## Rulings applied in round 2b-i and 2b-ii", after "Open for the PM (round 2a)", which stays as round 2a wrote it, marked superseded (PRE-2BI-REST, PRE-2BII-B1)]. Round 2b-ii runs after the gate
(root's P9-B0 and re-pin, the merged lanes' "For PRE" blocks): one doc agent, one fresh review; nothing here binds until
the PM rules this file after round 2b-ii and its review (W3-14, PRE-2B-SPLIT). No P9 cell runs before that.

**ROUND 2b-ii part A (lane-delivered values; Rulings PRE-2BII-AGENT, PRE-2BII-PAR, PAR-1, ledger :927, :932, :931).**
PRE agent, 2026-10-15, in pool-2's sprint dir beside root's M6B-FINISH and the gate (`r12-post-gate.md` "PRE-2BII",
part A). Filled from the merged lanes' "For PRE" blocks and root's merge records: Appendix IV items 14, 16, 17, 18,
31, 32, 33; items 7, 10, 11, 13, 19, 21 re-bracketed "filled at the cell's registration commit by root" (their value
exists only then); the ledger's "PRE 2b-ii list" disclosures (:699, :814-815, :861-862, :876) into their blocks;
PRE-MVX (:889) in A.4, B.7, B.8 and E; PY-S S-4 (:904) in B.0; E's "Memmel SE" question from the v8 driver source; the
new B.10 (lapse table, gate-decided column pending). Part B (after the gate root's hand-back) fills the gate-delivered
items (1-6, 8, 9, 12, 15, 20, 22, 23, 25, 26), the outcomes of D3-Q1a, COMPOSE-1 and SQL4-CUTOFF-2, the eval and fit
modes, B.10's gate-decided column and "Round 2b-ii record", and re-states any part-A line the gate contradicts. A
stale standing line is stated beside, never rewritten (PRE-40 form); where a lane's block differs from this file or a
ruling, the lane's fact is filled and one sentence "(differs from <x>; stated for the PM)" stands beside it; nothing
is decided here. [read: part B is split by PRE-2BII-B1 (ledger :959) into part B1, written beside the gate (what no
gate fact decides; paragraph below), and part B2, after the gate's final hand-back (the gate-delivered items above)]

**Basis read (round 2b-ii part A).** `resume12-addendum.md`; `r12-post-gate.md` "PRE-2BII"; this file whole (round
2b-i: 1,238 lines, last commit `685e238b`); in the ledger `progress.md` (939 lines at drafting), by grep, the Ruling
lines PRE-12 / PRE-13 / PRE-14 (:524), PRE-30 (:570), PRE-34 .. PRE-36 (:599-601), PRE-39 (:604), PRE-40 (:623),
D3W-STALE (:676), D3W-S (:695), ALCLOCK-4 (:702), D3-Q1 (:726), D3-Q3 (:728), D3WPB-Q2 (:732), SIGN-SUBST (:803),
COVMV-S2 (:814), COMPOSE-1 (:832), ALCLOCK-S2 (:861), A4-DB (:867, which carries A4-P10-FLIPS; no separate "Ruling
A4-P10-FLIPS" line exists), A4-FIX1-P10 (:883), PRE-MVX (:889), NAV-S (:890), D3-Q1a (:898), PY-S (:904),
PRE-2BII-AGENT (:927), PAR-1 (:931), PRE-2BII-PAR (:932), OD3-PAR (:935), OD3-DB (:939), the five "PRE 2b-ii list"
lines, root's M5c hand-back (:875) and the "Task <lane>: APPROVE at" lines; `task-AL-CLOCK-report.md` (with "Fix round
1"), `task-COV-MV-report.md` (with "Fix round 1"), `task-D3-report.md`, `task-D3W-report.md`, `task-D3W-review.md`
(S-6, root item 6), `task-D3W-PB-report.md`, `task-OD3P-report.md` (with fix round 1 and the follow-up round),
`task-SIGN-report.md`, `task-A4-report.md`, `wave-review-python.md` S-4, `wave-review-nav.md` M-1;
`root-wave3-merge-report.md` (its "For PRE" sections and root items) and `integration-log.md` (merge rows) at root
docs head `0a481348`; `p9-od3-runbook.md` sections 0-2; the files under `scripts/specs/p9/rules/`,
`scripts/specs/p9/templates/` and `scripts/specs/p9/kinds/walk-forward.json` (SHA-256 by `sha256sum` of the working
bytes, each equal to `git show HEAD:<path> | sha256sum`); the blobs of `scripts/wave_kinds.py` /
`scripts/wave_stage_kinds.py` at HEAD; the v8 driver source by `git show <commit>:<path>` (`atx-impl/tools/nav_summ.py`
at `425d16db` .. `44a27d5d`, `scripts/cycle_verdict.py` at `fa61c67f` .. `e5c219d6`, `scripts/research_cycle.py` at
`44a27d5d`), `platform-v8-20260929/v8-prereg.md` :1-12, `platform-v8-20260929/task-CELLS-brief.md` :20-45 and
`platform-v8-20260929/v8x-prereg.md` :104-106. Root heads seen: `6618abe1`, then `0a481348` (`git log --oneline` only).

**Blind (round 2b-ii part A).** No 2020-2023 return, IC, Sharpe, turnover or NAV statistic is read for this round or
written: the four v8 result lines E cites were not opened (their SE basis is read from the driver source), and reading
this file whole showed only the public v8 figures rounds 1-2b-i copied (D; E's G-B7 table), none used or re-written
here. No data file, NAV dir, `trials.jsonl` or `build-equity/` file opened (one `ls` of
`build-equity/train-2020-2023-lo3-fields-v15-run/` printed four file names); nothing dated 2024-01-01 or later; no
build, no test, no real-data tool; the only git write is this file's pathspec commit (PAR-1).

**ROUND 2b-ii part B1 (the rulings no gate fact decides; Rulings PRE-2BII-B1, PRE-2BI-REST, ledger :959, :951;
PAR-1).** PRE agent (fresh; it read part A's commit `7890883e` first), 2026-10-16, in pool-2's sprint dir beside root
GATE agent 2 (`r13-prompts/pre-2bii-b-prompt.md`). Each item is written beside the standing text with its ruling id
(PRE-40 form); no standing line is rewritten:
- PRE-2BI-REST, the header's unapplied round 2b-i list: PRE-39 in F.1 (the prediction line, restated in part A; its
  pointer to round 2a's item 8 answered beside) (applied, 2b-ii part B1); ALCLOCK-1 in F.1 (both books of the OD-3
  turnover-share prediction name `--info-clock-print`) (applied, 2b-ii part B1); ALCLOCK-2 in F.1 (the clean-source book
  applies no `info-clock-rate-v1`) (applied, 2b-ii part B1); OD3P-1 and ABS-1 in F.1 / F.2 (what "absent" means; the
  `absent-members-v1` producer and its lane's merge) (applied, 2b-ii part B1); GATE-1 in G item 7 (applied, 2b-ii part
  B1); C3-11 in G item 3 (applied, 2b-ii part B1); D3-2 beyond PRE-35's reading: in B.0, B.3 and B.6 since round 2b-i
  (the fitter flag names; ALCOMB-6 / -7 as D3W's), nothing further of it binds a cell (already applied; nothing added
  in part B1); H: H12 answered
  beside and H27-H46 added (applied, 2b-ii part B1); Appendix II items 2 and 8 given their resolving rulings, items 11
  and 13 their status (applied, 2b-ii part B1); "## Rulings applied in round 2b-i and 2b-ii" written after "Open for
  the PM (round 2a)", which is marked superseded (applied, 2b-ii part B1).
- MVX-F0H (ledger :949): "[read: P9-F0h (PRE-MVX, MVX-F0H)]" beside every "P9-F0" of A.4, B.7 and B.8 that names P9-X's
  or P9-V's parent, reference, comparison base, NAV argv base, degenerate-pair base or side-by-side book; F.1 holds no
  such mention (its one "P9-F0", the P9-W prediction, is PRE-39's); G-B1 and G-B6 keep P9-F0, said once in E (applied,
  2b-ii part B1).
- MVX-PRE13 (ledger :950): the ruled P9-F reading in A.4 (P9-F bullet) and E (applied, 2b-ii part B1).
- COVMV-PIN (ledger :948) in B.5 (applied, 2b-ii part B1).
- Part A's "stated for the PM" sentences answered beside: D3Q3-ARGV (:960), D3W-TPL (:961) and D3W-V3READ (:962) in
  B.6; F1-FIELDS (:963) in F.1 (and Appendix II item 13, Appendix IV item 32); MVX-F0H in B.7 / B.8; MVX-PRE13 in E;
  COMPOSE-PRE (:964) in B.5 / B.10 (applied, 2b-ii part B1).
- B.10: "Gate-decided column: part B2." under the table; no row changed (applied, 2b-ii part B1).
One new sentence "stated for the PM" (B.7, PRE-30 read under MVX-F0H when P9-MV alone is accepted before P9-X);
nothing is decided here. No pinned registration, expected hash or other file is edited; no part-B2 placeholder is
filled.

**Basis read (round 2b-ii part B1).** `r13-prompts/pre-2bii-b-prompt.md`; `resume12-addendum.md`; `r12-post-gate.md`
"PRE-2BII" (parts A and B); this file whole at `7890883e` (1,644 lines; unchanged at root's docs head `0c8bc8da`); in
the ledger `progress.md` (965 lines at drafting), the Ruling lines PRE-10 (:522), PRE-12 / PRE-13 / PRE-14 (:524),
PRE-27 (:543), PRE-28 (:555), PRE-29 / PRE-30 (:569-570), PRE-31 (:573), PRE-32 .. PRE-39 (:597-604), C3-11 (:622),
PRE-40 (:623), OD3P-1 (:624), ALCLOCK-1 / -2 (:625-626), D3-2 (:629), GATE-1 (:630), ABS-1 (:643), D3-Q1 (:726),
COMPOSE-1 (:832), PRE-MVX (:889), D3-Q1a (:898), every line of the RESUME 12 block (:922-951) and of the RESUME 13
block (:952-965), and the lane lines "Task OD3P" (:690, :822), "Task ABS" (:738), "Task SIGN" (:824);
`integration-log.md` :700, :715, :732, :734 (merge rows); `root-wave3-merge-report.md` :826-834 (SIGN's post-deletion
state). `git merge-base --is-ancestor` showed the merges of ABS (`898bf120`), OD3P (`7f6f1c75`, `a2cf7e78`) and SIGN
(`338e6a7c`, `cdc753a8`) in the gate head `06dc9e5a`. Root heads seen: `7890883e`, `0c8bc8da` (`git log --format=%h`
only).

**Blind (round 2b-ii part B1).** No 2020-2023 return, IC, Sharpe, turnover or NAV statistic is read or written (the
ledger and merge rows read carry test counts and SHAs only); no data file, NAV dir, `trials.jsonl`, `build-equity/`
file or anything dated 2024-01-01 or later opened; no build, no test, no real-data tool; the only git write is this
file's pathspec commit (PAR-1).

**ROUND 2b-ii part B2a (the gate-delivered values that exist while the gate is owner-blocked; Ruling PRE-2BII-B2A,
ledger :990; PAR-1).** The same PRE agent, continued by the PM's message, 2026-10-16, beside the BLOCKED-OWNER gate
(GATE-IDX), from the gate records as they stand at root's docs head `65ce28f0`. Each value is filled in place in the
bracket form; stale lines are stated beside (PRE-40 form); nothing is decided here:
- Appendix IV item 1: R-2's re-based NAV reference `build-equity/p9-b0-r2-nav` (FIELDS-SUB-A, path A; the
  classification disclosed) (filled, 2b-ii part B2a).
- Item 3: the re-based Y-1 reference `build-equity/p9-b0g-y1-nav` (PRE-29's store substitution; no identity claimed;
  PRE34-WHO); the X-10 re-base `build-equity/p9-b0g-x10-nav` (holds) stated beside C.11 (A.4 names no X-10) (filled,
  2b-ii part B2a).
- Item 5: C3-6 held, cited from M2j (filled, 2b-ii part B2a).
- Items 9, 12, 15 and the fit-mode parts of items 16 and 17: flag-absent (D3-Q1a DIFFERENT), with B.0 "Fit mode"
  (filled, 2b-ii part B2a); item 8 is P9-L's parent dirs (= item 2, behind GATE-IDX), not a fit-mode cell: left for B2b.
- ALCLOCK-1's outcome (held) in B.4 and F.1; COMPOSE-1a ACCEPT (store-backed) in B.5; D3-Q1a DIFFERENT and P9-W LAPSED
  (0) with the finding in B.6 (filled, 2b-ii part B2a).
- Item 23: B8A-PROD in B.8 (a), with the FIELDS-GONE consequence for its step 2 stated beside (filled, 2b-ii part B2a).
- C.13: SQL4-CUTOFF-2's outcome; A.4 and F.1: FINDING FIELDS-GONE stated (R-2 re-based by substitution on v15, X-5's
  identity cut, FIELDS-X5); B.0 "Lane suites": the gate's step-2 counts (TEST-2) (filled, 2b-ii part B2a).
- B.10: the gate-decided column for P9-C, P9-MV and P9-W and the status column; P9-X stays pending PRE-30 as PRE30-MVX
  reads it; the table now reads as LAPSE-TABLE states it (filled, 2b-ii part B2a).
Left for part B2b (brackets kept): Appendix IV items 2, 4, 6, 8, 20, 22, 25, 26; the cache listings; the catalog
digest; "## Round 2b-ii record". The exe table is provisional until the re-pin: every exe cited here is "provisional,
re-pin pending" (`handoff-root-gate-1.md` "Exe table").

**Basis read (round 2b-ii part B2a).** The PM's B2a message; this file at `aef2e033` (unchanged at `65ce28f0`);
`root-gate-report.md` whole (723 lines at `65ce28f0`); `handoff-root-gate-1.md`, `-2.md`, `-3.md`; in the ledger
`progress.md` (990 lines at drafting) every line from "- RESUME 13" (:952) to the end, including SQL4-CUTOFF-2 (:924),
COMPOSE-1a (:956), COMPOSE-PRE (:964), PRE30-MVX (:969), COVMV-PIN-2 (:970), B8A-PROD (:971), TEST-2 (:973), the
hand-backs of root GATE agents 2 and 3 (:974, :984; agent 1 left no hand-back line: OWNER STOP 12, :946), "Cell P9-W:
LAPSED (0)" (:975), GATE-TMP-2 (:976), GATE-MYPY (:977), FINDING FIELDS-GONE (:978), FIELDS-SUB (:979), FIELDS-X5
(:980), LAPSE-TABLE (:981), GATE-3-ORDER (:982), FIELDS-SUB-A (:985), RELCACHE-ALLOW (:986), PRE34-WHO (:987),
YF0-NAV-ORDER (:988), the PM's GATE STATE (:989) and PRE-2BII-B2A (:990); `handoff-root-m2j.md:42-44` and
`handoff-root-m2j-fin.md:44` (C3-6). `gate-logs/` file names only (`ls`); no gate log was opened. Root heads seen:
`c99a76e4`, `2e08fa1c`, `e5a2fe68`, `778d34b8`, `65ce28f0` (`git log --format=%h` only).

**Blind (round 2b-ii part B2a).** The gate records read print no 2020-2023 return, IC, Sharpe, turnover or NAV statistic
(file SHA-256, JSON paths, exit codes, counts, wall seconds and rho only); none is written here, and rho values are left
to B2b (items 20, 22, 26). No data file, NAV dir, `trials.jsonl` or `build-equity/` file opened; nothing dated 2024-01-01
or later; no build, no test, no real-data tool; the only git write is this file's pathspec commit (PAR-1, GATE-MYPY).

**ROUND 2b-ii part B2b (the gate's final values; Rulings PRE-2BII-AGENT, PRE-2BII-B2A, ledger :927, :990; PAR-1).** The
same PRE agent, continued by the PM's message, 2026-10-16, after the gate's final hand-back (GATE-1 DONE; code head
`f8a85ad4`, the re-pin of record `2078f877`; docs head `d22246f6`). Each value is filled in place in the bracket form;
stale lines are stated beside (PRE-40 form); nothing is decided here:
- Appendix IV item 2 (Y-F0's re-based u / w / NAV `p9-b0g-yf0-{u,w,nav}`, with REPIN-REV's two roles) and item 8 (=
  item 2 for P9-L) (filled, 2b-ii part B2b).
- Item 4: the exe table of the re-pin of record `2078f877`, EXE-FREEZE from it, the follow-up `3a1a4759` reverted by
  `f8a85ad4` (REPIN-REV) (filled, 2b-ii part B2b); the exes parts B2a cited "provisional, re-pin pending" are the same
  p9-3f exes the re-pin names: final now.
- Item 6: eval mode research-fast; items 20, 22, 26: rho value and flag (P9-V's own pair: the cells root's, before
  its read); item 25: `ucrtbase.dll` at P9-B0 (filled, 2b-ii part B2b).
- The cache listings (C.13) and the catalog digest (G item 6) (filled, 2b-ii part B2b).
- The rulings of the final records stated where they bind: REPIN-REV (A.4, B.1, C.15), FINDING FIELDS-RISK (A.4),
  PW-LAPSE-PRINT (B.6, F.1, E), B8A-PROD-2 (B.8 (a)), SEAL-REF (B.0, B.1), GP-TICKS (G items 3 and 6), and the
  re-pinned spec SHA-256 beside every pre-re-pin SHA-256 or line citation of the three re-pinned specs (A.4, B.1, B.2,
  Appendix I) (applied, 2b-ii part B2b).
- "## Round 2b-ii record" at the end (applied, 2b-ii part B2b).
Earlier lines the final records contradict, each re-stated beside (PRE-40 form): part B2a's A.4 "Fields inputs" line
("every input present" for Y-F0: its spec built alone resolves fields v13, FIELDS-RISK); part A's / round 2b-i's
"parents re-pinned ONCE, to the references of the single P9-B0" (C.15: Y-F0's u / w pins and R-2 are not re-pinned,
REPIN-REV); B.0's seal-allowance "and identity run" and B.1's "carried alike on cell and reference" (SEAL-REF); my B2a
"stated for the PM" sentences in B.6, F.1 and B.8 (a), each answered beside by its ruling (PW-LAPSE-PRINT,
B8A-PROD-2); the round-2a SHA-256 of the three re-pinned specs (A.4, Appendix I) and two line citations of
`lib-v8ysb-gm.json` (B.1, B.2). Placeholders still open after this part: only the registration-commit items (Appendix IV
7, 10, 11, 13, 16, 17, 18 (theirs), 19, 21) and the cell-time ones (each cell's own pair rho before its read; G-B7's P9
steps after each cell).

**Basis read (round 2b-ii part B2b).** The PM's B2b message; this file at `1e2dbf3c` (unchanged at `d22246f6`);
`handoff-root-gate.md` (FINAL, 155 lines); `root-gate-report.md` at `d22246f6` (1,119 lines: the status after agent 4,
the round-2b-ii facts "Agent 4 (FINAL)", "Chain agent 4" whole incl. step 6's follow-up and REPIN-REV);
`integration-log.md` :799-927 (the gate rows); in the ledger `progress.md` (1,006 lines at drafting) RELCACHE-ALLOW
(:986), YF0-NAV-ORDER (:988), the P9-W cell line (:975), FIELDS-GONE (:978), FIELDS-X5 (:980), LAPSE-TABLE (:981)
and every line from :985 to the end, incl. PW-LAPSE-PRINT (:994), B8A-PROD-2 (:995),
SEAL-REF (:996), REPIN-UW / REPIN-R2 (:999-1000), GP-TICKS (:1001), REPIN-REV (:1003), FINDING FIELDS-RISK (:1005);
`git diff 06dc9e5a 2078f877 -- scripts/specs/v8/` (hunk positions; `--stat`) and
`git show <commit>:scripts/specs/v8/lib-v8ysb-gm.json` at the two cited ranges (spec text, no data). Root heads seen:
`bfbe64d2` .. `d22246f6` (`git log --format=%h` only).

**Blind (round 2b-ii part B2b).** The final records print no 2020-2023 return, IC, Sharpe, turnover or NAV statistic
(file SHA-256, JSON paths, exit codes, counts, bytes, wall seconds and rho); rho is copied as value and flag only
(B2-13). No data file, NAV dir, `trials.jsonl` or `build-equity/` file opened; nothing dated 2024-01-01 or later; no
build, no test, no real-data tool; the only git write is this file's pathspec commit (PAR-1).

**ROUND 2b-ii part B3 (FLAGS-ROUTE restatement; Rulings FLAGS-ROUTE, FLAGS-ROUTE-2, FLAGS-SEQ (2), FLAGS-ORDER,
PRE-40, PAR-1; ledger :1019, :1022, :1023, :1028).** A fresh PRE docs agent, 2026-10-16, in pool-2's sprint dir beside
the root FLAGS agent (`r14-prompts/pre-flags-prompt.md`), after GATE-REV part 2 (`prereg-2bii-review.md` at
`6c608ff3`: CHANGES_REQUIRED, Required RQ-1, Suggested SG-1 .. SG-3) and root's spec commit `f8d852f3`
(`lib-v8ysb-gm.json` `nav.flags` += `--ann-mean-gross --info-clock-print`, nothing else; SHA-256 `3631679c...edea` ->
`a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a`; the identity HOLDS; root docs head `5ec208b4`).
PRE-40 form: every stale sentence stands; the route is stated beside it with the ruling id and the commit SHA (bracket
`[FLAGS-ROUTE, f8d852f3: ...]`); nothing is decided here:
- GATE-REV SG-1: F.1's FIELDS-RISK sentence answered beside by OD3-FIELDS-RISK (ledger :1008) (applied, 2b-ii part B3;
  its own commit `bc034c4a`). SG-2 is `p9-od3-runbook.md`'s ("F.1 governs" at the head of section 0; the dated
  PW-LAPSE-PRINT / FLAGS-ROUTE note under section 4 item 3; commit `cb051169`); RUNBOOK-B6 (ledger :1026): no further
  runbook edit.
- B.0 "Gross figure opt-in (PRE-7a)": the route (the parent's `nav.flags` after `f8d852f3`; the v2 templates' `parent`,
  no `nav.flags` override; `scripts/cycle/phases.py:824`), the references' exe-direct hand argv, the identity outcome
  (HOLDS, `gate-logs/flags-identity-yf0.txt`) and the inheritance as root recorded it (applied, 2b-ii part B3).
- B.0 "exes.": the parent's new SHA-256, the re-pin of record `2078f877` kept, no pin moved, EXE-FREEZE unaffected; the
  bracket names no `3631679c`, so the statement sits beside its `lib-v8ysb-gm.json` mention (applied, 2b-ii part B3).
- B.0 "Void" rule (2): FLAGS-ROUTE-2 (alike by inheritance for every v2 cell against `p9-b0g-yf0-nav` or an accepted
  v2 parent; a hand cell's one-sided `--info-clock-print` against `p9-b0g-y1-nav` disclosed in the cell record, not a
  void) and FLAGS-ORDER (argv token order is no input; the flag set is) (applied, 2b-ii part B3).
- B.1 "Every flag": P9-L's route (its hand v1 template root names both flags in its own `nav.flags` or inherits them;
  the cells root states which in the registration commit; alike with `p9-b0g-yf0-nav` either way) (applied, 2b-ii part
  B3); B.1 "Parent / paired reference": the new SHA-256 beside `3631679c` (applied, 2b-ii part B3).
- B.4 "The flag pair": the route exists since `f8d852f3`; before it the parent's `nav.flags` carried neither flag
  (GATE-REV RQ-1) (applied, 2b-ii part B3).
- B.7 and B.8 (the hand specs from P9-F0h's / P9-X's NAV argv with the stated change only): FLAGS-ROUTE-2 (b) once in
  each (applied, 2b-ii part B3).
- The parent's new SHA-256 beside every other `3631679c` citation: A.4 (the Y-F0 bullet), C.15 (no literal `3631679c`
  there; beside its re-pin sentence, with `verify` and the fixture chain digest after `f8d852f3`), Appendix I (the B2b
  paragraph under the table), Appendix IV item 8, and "Round 2b-ii record" (a line under "The re-pinned specs" table;
  no row changed) (applied, 2b-ii part B3).
- "Round 2b-ii record", last paragraph: the route named beside "which every cell carries alike" (applied, 2b-ii part
  B3).
- Line 3 (status): "RQ-1 closed by `f8d852f3` (FLAGS-ROUTE); pending the scoped re-check and PRE-2BII-RULED" (applied,
  2b-ii part B3).
SG-3 (line citations into `65ce28f0`-era files) is NOT re-cited now: they stay disclosed as such ("Round 2b-ii
record", first paragraph), and the cells root refreshes them at the registration commit, as the review proposes. No
pinned registration, expected hash, standing line, verdict or table row is changed; no other file is edited here.

**Basis read (round 2b-ii part B3).** `r14-prompts/pre-flags-prompt.md`; `resume12-addendum.md`;
`r13-prompts/pre-2bii-b-prompt.md`; `r12-post-gate.md` "PRE-2BII"; `prereg-2bii-review.md` whole; `gate-report-review.md`
S-5; this file's header, A.4, B.0, B.1, B.4, B.7, B.8, C.15, F.1, Appendix I, Appendix IV and "Round 2b-ii record" at
`a40db317`, and again at `5ec208b4` before this part's edits; `p9-od3-runbook.md` whole; in the ledger `progress.md`
PRE-7a (:538), PRE-30 (:570), PERM-1 (:595), PRE-40 (:623), ALCLOCK-1 (:625), PAR-1 (:931), PRE30-MVX (:969),
GATE-MYPY (:977), PRE34-WHO (:987), PW-LAPSE-PRINT (:994), SEAL-REF (:996), REPIN-REV (:1003), FINDING FIELDS-RISK
(:1005), OD3-FIELDS-RISK (:1008) and every line from :1015 to :1029 (FLAGS-ROUTE, the RESUME 14 block, HOST-6a,
FLAGS-ROUTE-2, FLAGS-SEQ, RUNBOOK-B6, FLAGS-ORDER and the hand-backs); the PM's PART 2 message (the spec commit, the new
SHA-256, the identity outcome, root's docs head, the inheritance); `git show --stat f8d852f3` (one file) and
`git show f8d852f3:scripts/specs/v8/lib-v8ysb-gm.json | sha256sum` = the working bytes' `sha256sum` = `a4ef6c04...c06a`
(spec text only). Root's records (`root-gate-report.md`, `handoff-root-gate.md`, `gate-logs/flags-*.txt`) are cited by
name from the PM's message and the ledger, not opened. Root heads seen: `6c608ff3`, `5648284f`, `f8d852f3`,
`1a14a24a`, `510a768a`, `5ec208b4` (`git log --format=%h` only).

**Blind (round 2b-ii part B3).** No 2020-2023 return, IC, Sharpe, turnover or NAV statistic is read or written: this
file, the runbook and the ledger lines were read through a decimal-number mask, except the lines edited here (SHAs,
flags, citations); no data file, NAV dir, `trials.jsonl`, `build-equity/` file or anything dated 2024-01-01 or later
opened; no build, no test, no real-data tool; the only git writes are the pathspec commits of this file and the
runbook (PAR-1, GATE-MYPY).

**Cells registration edits (cells root agent 1, `r14-prompts/cells-prompt-1.md`; PRE-40, PRE-2BII-RULED, SG4-REF,
SG-3).** P9-L's registration (code `0bba6c89` the template, `6ea79e5a` the calibrated run recipe; docs: this commit
with `cells/p9l-label-terminal-plan.md`). PRE-40 form: no sentence is rewritten; each fill or restatement is a bracket
beside the text it answers: B.1 "Shape" (the template's SHA-256, the inherited flags), B.1 "Parent", "Acceptance" and
"Fit mode" (SG-3: the stale line citations re-located at docs head `a32b6f4c` / code head `f8d852f3`), Appendix IV
item 7 (filled), and SG4-REF beside B.0 PRE-7a's and the round-2b-ii record's "the references carry both" (the
two lines that held that clause are re-wrapped around the bracket, every word kept in order). No pinned
registration, expected hash, standing line, verdict, table row or `manifest_note` is changed.

**Basis read (round 2b-i).** `resume4-dispatch.md`, `resume6-addendum.md`, `resume7-addendum.md`, `resume8-addendum.md`;
`r8-dispatch.md` "PRE-2b-i"; this file whole (round 2a: 1,117 lines, sha256
`5943005ebc3bdd40783feb064c12e016a11952f8b376feff54020d9bccc9c7f7`); `p9-prereg-flags.md` "P9 rules" and "Open for the
PM"; `briefs/brief-PRE.md`; in the ledger `progress.md` (655 lines at drafting), by grep, the Ruling lines EXE-FREEZE
(:406), OD5-1 .. OD5-7 (:461-467), PRE-29 / PRE-30 (:569-570), PRE-31 (:573), COV-13 (:587), PRE-32 .. PRE-39
(:597-604), W3-8a (:618), COV-14 (:621), C3-11 (:622), PRE-40 (:623), OD3P-1 (:624), ALCLOCK-1 / -2 (:625-626), SQL3-1
(:627), D3-1 / D3-2 (:628-629), GATE-1 (:630), A4-1 (:631), C3-12 (:633), C3-13 (:636), ABS-1, D3W-EVAL, D3W-PB,
PRE-2B-SPLIT (:643-646), the RESUME 8 block (:637-655, process lines), and the cited public lines :115, :184, :190.
ALCLOCK-3 has no "Ruling" line in the ledger (grep: only the dispatch line :651 names it); its text is taken from
`status-8.md` :156-158 ("Rulings made while writing this file (also appended to the ledger)") and `r8-dispatch.md` :113.
The v8 ledger `platform-v8-20260929/progress.md`: the accepted lines of R-2 (:1584-1585), X-2 (:1998-2004), X-3
(:2005-2012) and X-5 (:2019-2023) (E). Root head seen: `1b64ce45` (`git log --format=%h` only).

**Blind (round 2b-i).** The only 2020-2023 figures copied are the dSR and SE of R-2, X-2, X-3, X-5 from those accepted
v8 lines (PRE-36). Disclosure: printing those line ranges also showed the neighbouring v8 lines (:1582-1583; the rest
of R-2's entry and the start of R-3, :1586-1588; X-4 and X-6, both not accepted, :2013-2018, :2024-2027; "X batch 1
closed", :2028-2030), and a grep for "accept"
showed the first 160 characters of v8 :2028, :2099-2102, :2124, :2184; none of their figures is used here. No data file,
NAV dir, `trials.jsonl` or `build-equity/` path opened; no build, no test; no git command that changes state except the
commit of this file.

**Round 2a (record).** Lane PRE, round 2a (dispatch R6-3, ledger :590), doc agent, 2026-10-04. Round 2a rewrote the
round-1 draft (W3-14, W3-14a) so that sections A-G carry every ruling of the ledger up to its line 595, filled every
placeholder whose value existed then (each with its source line), and renamed the placeholders that wait for a running
lane or a root step `[ROUND 2b: ...]` (now `[ROUND 2b-ii: ...]`, round 2b-i; index: Appendix IV).

**Basis read (round 2a).** `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`; the round-1 draft of this
file (755 lines, sha256 `62d85b10a9a688bad02678c6fe77eb9aa7b4392be9e73feac5a78d4d8d82d2b6`); ledger `progress.md` (595
lines at drafting; `ledger :N` = line N) from "- RESUME 5" (:459) to the end, every "Ruling" line there, and the older
lines cited below; `p9-prereg-flags.md` (ruled by PRE-31; sha256 in Appendix III); `root-wave2-gate-report.md` items 2
(ALD-DIAG), 3 (B2-13 rho), 4 (d) "C4 D-COV", 5 (Q5), 6 (S2R-DIAG); `handoff-root-m2i.md` (store pins);
`handoff-root-m2h.md` (W1-I1 margins, the engine match stage on real exes); the spec files of Appendix I and
`scripts/specs/v8/y-vol-target-y-1.json` (bytes and SHA-256 only); `scripts/wave_readers.py` (`book`). Root head seen:
`327c86b2` (`git log --format=%h` only).

**Round 1 basis (record).** `resume4-dispatch.md`; `briefs/brief-PRE.md`; `wave3-carry.md`; plan
`docs/plans/2026-10-03-p9-sprint-plan.md` (sha256 `4acbde4c...`) sections 0.3, 0.5, 2.4 PRE, 4.4, 5, 6; ledger to :509;
status-4 and status-5; `task-AL-COMB-report.md`, `task-AL-SIG-report.md`, `task-S2-report.md`, `task-B2-report.md`; E2's
v2 manifest shape in pool-17 at `ea7d9dde`; B2's `598d2199`, `8fb17ad6` (pool-14); `scripts/specs/p9/*`;
`cov-design.md` section 5; `briefs/brief-COV-MV.md`; `al-data/atx-db-request.md` A0 (flag line only); `v8y-prereg.md`,
`v8x-prereg.md`; branch `feat/platform-v8-xpre-20261002` (OD3-TOOL).

**Blind.** Round 2a read no 2020-2023 return, IC, Sharpe, turnover or NAV statistic beyond the public lines of
`progress.md` and the status files; the ALD-DIAG, rho, D-COV, Q5 and S2R-DIAG tables copied below are universe-count,
correlation, risk-forecast and exposure diagnostics. Nothing dated 2024-01-01 or later; no data file, NAV dir,
`trials.jsonl`, `wq-explore*`, `build-equity/` dir or PNG opened; store pins are copied from the handoff, not recomputed;
root's history read only as SHAs; no git command that changes state. The v8 ledger (`platform-v8-20260929/progress.md`)
was not opened, so the G-B7 step values that only it holds stay placeholders (section E). Round 1's disclosure stands:
`git log` showed it the body of root commit `1b8c4df2` (two rho values, the COV-7 miss); round 1 used neither, and the
rho lines below come from the ledger and the gate report.

**Conventions.** `[ROUND 2b-ii: <what closes it - lane or root step that delivers it>]` = a placeholder that waits
(renamed from round 2a's `[ROUND 2b: ...]` in round 2b-i). "(ID)" after a sentence = the ledger ruling it carries; where
a draft line and a ruling differed, the ruling's text is in. Round 2a's contradictions between rulings were ruled
(PRE-32 .. PRE-39) and are applied in the text; "Rulings applied in round 2b-i" at the end says where [read: "## Rulings
applied in round 2b-i and 2b-ii" (round 2b-ii part B1)]. "The wave-3
P9-B0", "the wave-3 re-pin" and "the wave-3 gate" name the ONE gate of GATE-1 (ledger :630): one gate for waves 2 and 3
on the head that holds every wave-2 and wave-3 merge, ONE P9-B0 (R-2, Y-F0 and Y-1 re-based; the substitution list
written first) and ONE re-pin (W1-I2 (b), `exes.eval` included); EXE-FREEZE starts at that re-pin. Paths are relative
to `C:/atx-wt/pool-2` unless a pool is named. [read (round 2b-ii part B2b; REPIN-REV, ledger :1003): the re-pin of
record is `2078f877`; a follow-up pins-only commit `3a1a4759` refused at `verify` and was reverted by `f8a85ad4`
(`git diff 2078f877 f8a85ad4 -- scripts/specs/v8/` empty); EXE-FREEZE runs from `2078f877` ("## Round 2b-ii record")]

---

## A. Scope, ceilings, starting counts

### A.1 Ceilings (DEC-18 as amended by COV-7 / COV-8; plan section 5.2 :911)

| quantity | registered ceiling | reachable after W3-15 (P9-S lapsed) |
|---|---|---|
| hypothesis cells | <= 8 (P9-S, P9-H, P9-M, P9-C, P9-MV, P9-W, P9-X, P9-V) | <= 7 run (P9-H registered, PRE-6; P9-C / P9-W / P9-MV may lapse, PRE-14) |
| protocol cells | <= 1 (P9-L) | <= 1 |
| N_c (construction) | <= 62 + 9 = 71 (COV-8, ledger :200) | <= 70; <= 69 when P9-X is not a new trial (PRE-30) |
| K_a (hand-written admission strings) | <= 10 | 0 added (W3-15, OD5-4) |
| mined campaigns / M | 0 campaigns (OD-P9-9, ledger :17); M + 0 | M = 110 unchanged |
| N_tot = sum(trial_counts) + M | <= 252 | <= 241; <= 240 under PRE-30 |
| history reads (E-41, counted apart, not in N) | - | 0 now; OD-3 adds 4 history lines (PRE-18, fourth line PRE-31; F.1) |

### A.2 Starting counts (public lines only)

- n_before (R0-14, ledger :190): N_c 62 (cap 62), K_a 40 hand-written (61 admission lines in all: v8 12, X hand-written
  25, X-4 re-screens 9, Y hand-written 15), M 110, N_tot 233; N_hand = N_tot - M = 123 (R0-12, ledger :184).
- Trial ledger `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` (ledger :190, :459, :582;
  `resume6-addendum.md` :15-16). No trial since R0-11 (ledger :167).
- Public DSR at the v8 adoption print (ledger :184), printed for continuity, gating nothing: DSR_tot .6798 (N_tot 233),
  DSR_hand .7589 (N 123), DSR_v8 .8350 (N_c 62).

### A.3 E[max] arithmetic (v8y section 4 formula, gamma .5772; recomputed in round 1)

E[max] factor = (1 - gamma) Z^-1(1 - 1/N) + gamma Z^-1(1 - 1/(N e)):

| N | 62 | 69 | 70 | 71 | 123 | 131 | 233 | 241 | 252 |
|---|---|---|---|---|---|---|---|---|---|
| factor | 2.3576 | 2.3972 | 2.4025 | 2.4077 | 2.6026 | 2.6242 | 2.8151 | 2.8259 | 2.8402 |

Matches plan section 5.2 :920-923 (2.357, 2.397, 2.4025, 2.4077, 2.815, 2.840). Against the v8 end state: the reachable
maxima raise the factor by +1.90% on N_c (62 -> 70), +0.83% on N_hand (123 -> 131), +0.38% on N_tot (233 -> 241); the
registered ceilings by +2.13% on N_c (62 -> 71) and +0.89% on N_tot (233 -> 252). The plan's "+0.44%" is against its
earlier 69, not against 62. V decides more than N (v8y section 4).

### A.4 Named books

- **R-2** (V8-F): the reference of G-B6 and G-B8. Spec as the v8 OD-3 protocol names it (`scripts/specs/v8/lib-v80.json`,
  v8-od3-protocol section 3.1). Re-based at the wave-3 P9-B0 (PRE-22). [ROUND 2b-ii: `build-equity/p9-b0-r2-nav`, the
  re-based R-2 of record (FIELDS-SUB-A, path A: PRE-22 met by the fields substitution v10 -> v15, FIELDS-SUB's witness
  70 / 70 held, `gate-logs/fields-witness.txt`, commit `e5a2fe68`); substitution list row 1 (`778d34b8`, committed before
  any P9-B0 run): the v8 receipt `mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80-run`'s argv with only
  `--fields build-equity/train-2020-2023-lo3-fields-v15/manifest.json --fields-sha256 26fee5ce...3b09` and `--output`
  changed and `--ann-mean-gross` added, token 0 the Release NAV exe p9-3f `255b58ac...3565` (provisional, re-pin
  pending); completed exit 0, the 84-field manifest accepted; 27 files, listing SHA-256
  `04bb8060aadf692a63e7f1baafd23f297db67c24d9c1f0a43dce6678a755b1bd`, `summary.json`
  `48a52f6adb06c2e966cb366f2c71c9dbef55ba48de243afc2cce01df3085c1b9` (per-file SHA-256 listing:
  `gate-logs/b0-files-p9-b0-r2-nav.txt`). Disclosed beside it (FIELDS-SUB-A): against the v8 NAV of record
  `mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80` (written by the v8-era exe `5497c89d`, before P9's
  NAV change) 13 / 27 files are byte-identical and every differing path is in one class: ALLOWED (8 leaves:
  `/producer/*`, the `ann_mean_gross` opt-in), FIELDS (4: `/financing_fields/manifest_sha256`, v10 `a4a060ae` -> v15
  `26fee5ce`), DERIVED (14 digests of other output files), M1D-C1 (1: `/v7/extras`), C1-COST (9 files: cost columns
  only, 2-8 rows of 1,006); no `events_*`, L0 or `summary.json` statistic leaf moved; the report records PATH B by
  FIELDS-SUB's letter (`root-gate-report.md:671-683`) and FIELDS-SUB-A re-rules it path A; G-B6 and the OD-3 R-2 line
  read this dir and "cross-build reference" is NOT printed - `root-gate-report.md:636-683`; `gate-logs/r2-classify.txt`,
  `r2-cmp-raw.txt`; `handoff-root-gate-3.md:82`; ledger :979, :985]. Round 2b-ii part B2b (REPIN-REV, ledger :1003):
  R-2 is read BY PATH (`p9-b0-r2-nav`): its spec `lib-v80.json` (`306a070b...3a21`) is untouched at the re-pin of record
  `2078f877`, its `fields` pin naming the absent v10 (`a4a060ae`) and its `nav.output` the v8 NAV dir: both stale, stated
  in the substitution list (amendment 4, PRE-40 form); `3a1a4759`'s re-pin of it (REPIN-R2) failed `verify` (the
  70-name `fields.list` against v15's 84 rows) and was reverted by `f8a85ad4`; R-2's TRAIN fields dir is v15
  (`26fee5ce`) and its TRAIN w the v8 w its argv reads, `mega-v8-r1w-train-std-v80-1/train_combined.json`
  (`bbbf6f2b...cd98`, not re-based); G-B6's bundle and any OD-3 era spec for R-2 read `p9-b0-r2-nav` (an OD-3 era spec
  for R-2, if OD-3 runs, is built from its recipe at the OD-3 step, the PM ruling then) - `handoff-root-gate.md:75`,
  `:143-147`; `root-gate-report.md:188`, `:804-832`, `:1087-1119`.
- **Y-F0**: `scripts/specs/v8/lib-v8ysb-gm.json` (sha256 `1e3ec118...`), library `fund_industry_ic_v8ysb.json`
  (`41010b0b...`, 67 candidates), L_P 1.1828 (ledger :147, :190). The first parent of P9; re-based at the wave-3 P9-B0
  (PRE-22). [ROUND 2b-ii: u `build-equity/p9-b0g-yf0-u` (5 files, listing SHA-256
  `1cb087abaa50746392269b3452491be8fa2999b9a39e1212f01049acec2916aa`, `summary.json`
  `acbf61cef24e909bb8975668b1b2206708b5c96cf03194a0728564445fae12b8`; the Release IC exe p9-3f, a cold pass); w
  `build-equity/p9-b0g-yf0-w` (12 files, listing `ec69344005107bf0e2ab0bb859583db083889521a652dcfe134b2304e27c8ae1`,
  `summary.json` `f772b394a6885b5965d0c5ad93e4f023515a74ed7e02064a772814357ee904f2`, `train_combined.json`
  `e13fbc4dab75c00cc47d7a06d8fcfbefd2d1368407565e99001136d19de9da8c`); NAV of record `build-equity/p9-b0g-yf0-nav`
  (27 files, listing `e205d6ecb16e75cba02194b90b313849404c2052b3e1738c4bf020e486fcf797`, `summary.json`
  `826d5ec65329c24c4c0c1b72ee1f57dcdbdb136cf00d4113c6463310e79b8f38`; reads the re-based w, YF0-NAV-ORDER; argv
  `--ann-mean-gross --info-clock-print`); each holds against M1d's reference: u and w every payload byte-identical (u
  3 / 3, w 10 / 10) with every differing `summary.json` / `train_candidates.jsonl` leaf in RELCACHE-ALLOW's classes
  (TIMING, COUNTERS, IDENTITY, M1d's list), 0 outside (the compare tool's first pass, which put M1d's list outside, is
  kept and disclosed: `yf0u-cmp-pass1.txt`); NAV 26 / 27 byte-identical, `summary.json` PRODUCER 2 + OPTIN 75 leaves on
  the new side only, 0 outside; per-file SHA-256 listings `gate-logs/b0-files-p9-b0g-yf0-{u,w,nav}.txt`. Two roles
  (REPIN-REV, ledger :1003): these Release dirs are the references of record for provenance (the exe, the receipts,
  the identity), while the spec's `ic.u_output` / `ic.w_output` stay the v8-era cycle dirs
  `mega-v8-b0b-train-u-v8ysb-1` / `mega-v8xw-train-theme-erc-v8ysb-2`, the cycle's resume roots (completed state),
  byte-identical to the Release references in every payload; no cell re-runs u or w; `3a1a4759`'s re-pin of those
  outputs (REPIN-UW) failed `verify` (the cycle resolved them as pending attempt roots) and was reverted. The Y-F0 fit is
  not re-based (flag-absent identity held, B.0 "Fit mode") - `handoff-root-gate.md:78-80`, `:132-137`;
  `root-gate-report.md:189`, `:834-907`, `:1113-1119`; ledger :986, :988, :1003]. Stated beside (the re-pin of record
  `2078f877`): `lib-v8ysb-gm.json` is now `3631679cd18f1c6a63a6a5dc96cdbe6ee7669b81b8979f529544f1b237d9edea` (pins only:
  the Release NAV / IC exes, `exes.eval` and `exes_sha256` added, `nav.output` -> `p9-b0g-yf0-nav`); `1e3ec118...` above
  is its pre-re-pin SHA-256 (PRE-40 form). [re-stated, FLAGS-ROUTE `f8d852f3`:
  `a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a` (`nav.flags` + `--ann-mean-gross
  --info-clock-print`, nothing else); the re-pin of record stays `2078f877`; no pin moved; EXE-FREEZE unaffected]
- **Y-F**: Y-1 (`scripts/specs/v8/y-vol-target-y-1.json`, `faffee17...`; ledger :167). Its re-based reference at the
  wave-3 P9-B0 reads the Release-built `atx-risk-v1.1` container store `build-equity/p9-m2i-dcov-v11` (manifest
  `397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7`; PRE-29, ledger :569, :582) and is not
  byte-comparable with v8's Y-1, whose store of record a Debug exe built (ULP-level covariance differences; PRE-29).
  [ROUND 2b-ii: `build-equity/p9-b0g-y1-nav` (substitution list row 3, `778d34b8`: M1d's `p9-b0-y1-nav-run` argv with
  only `--risk-model build-equity/p9-m2i-dcov-v11 --risk-model-sha256 397366473b3a...23c7` (PRE-29's store
  substitution) and `--output` changed and `--ann-mean-gross` added, token 0 the Release NAV exe p9-3f (provisional,
  re-pin pending); before it the COV-9 S9 check held and the v1.1 store re-checked equal to its pin); completed exit 0;
  28 files, listing SHA-256 `cac882e19a5baee9d3d60581d93a278f9f4fc6b769ed0e9154dd408b1552b813`, `summary.json`
  `b0d4dcbf53e81aff3863ca1d9c7230da73a5c0a911a60f622c9f78aa3796c028`, `vol_target.csv`
  `8e614ea7bbf47d363a5c357b7442c7b3a7b86df753879715f5d20bd083f767a7` (`gate-logs/b0-files-p9-b0g-y1-nav.txt`); no
  identity claimed (PRE-29): it differs from M1d's `p9-b0-y1-nav` and, PRE-34's byte / JSON-path form, from v8's
  ledgered Y-1 NAV `mega-nav-v8y-vol-target-L2.0` at 26 / 28 files each (`recipe.json`, `capacity/recipe.json`
  byte-identical; `gate-logs/y1-cmp-b0.txt`, `y1-cmp-v8.txt`); PRE-34's printed-precision agreement of the figures is
  the CELLS root's, at the P9-X step, before the adoption print, figures to the cell record only (PRE34-WHO; B.7) -
  `root-gate-report.md:146`, `:646-652`, `:689-698`; `handoff-root-gate-3.md:84`; ledger :984, :987]. Stated beside
  (part B2b; the re-pin of record `2078f877`): `y-vol-target-y-1.json` is now
  `f4186573a41de3b67cdd815976639ed2123e5c6f43353516c61150d92dbb50d6` (`nav.output` -> `p9-b0g-y1-nav`; `--risk-model` /
  `--risk-model-sha256` -> `p9-m2i-dcov-v11` `397366473b3a...23c7`, PRE-29; `locked.reference_cell` ->
  `p9-b0g-x10-nav/summary.json` `deabe71b...0bd`), and `x-leverage-L2.0.json` (X-10)
  `46e9d6c2fdbf42e362518118938bc89af038e3c09d7519d660aaa936554fd0bf` (`nav.output` -> `p9-b0g-x10-nav`,
  `locked.reference_cell` -> `p9-b0g-yf0-nav/summary.json` `826d5ec6...8f38`); `faffee17...` above is Y-1's pre-re-pin
  SHA-256 (PRE-40 form); P9-X's reference spec is the re-pinned `y-vol-target-y-1.json` - `handoff-root-gate.md:52-56`,
  `:131`.
- **Fields inputs (round 2b-ii part B2a; FINDING FIELDS-GONE, ledger :978).** `build-equity/train-2020-2023-lo3-fields-v9/`,
  `-v10/`, `-v13/`, `-v14/` and `-v15a/` are absent from disk and `-v11/` holds only its `manifest.json`; `-v15/` is whole
  (84 payloads); only the `-run` dirs of the others remain; nothing is rebuilt from atx-db (A4-DB) and who deleted them
  is an owner item. So: R-2 is re-based by the fields substitution v10 -> v15 (FIELDS-SUB, FIELDS-SUB-A; the R-2 bullet
  above); X-5's step-3 NAV / u / w flag-absent identity is CUT, "not run at the gate: fields v13 absent (FIELDS-GONE)"
  (FIELDS-X5, ledger :980; X-5 is no P9 cell's parent; the gate's flag-absent identity of record is Y-F0's); Y-F0, Y-1
  and X-10 read v15, every input present (`gate-logs/inputs-check.txt`); the two D-COV stores are pinned bytes,
  unaffected (CRT-2), but their build argv names fields v10 (B.8 (a)) - `root-gate-report.md:463-474`. Re-stated (part
  B2b; FINDING FIELDS-RISK, ledger :1005), against this line's "every input present" for Y-F0: its run argvs' inputs
  are present, but its spec built ALONE resolves a template-chain input `baseline_fields` of fields v13 (absent): `verify`
  prints "REFUSED (the cycle cannot be built): input baseline_fields missing: build-equity/train-2020-2023-lo3-fields-v13/
  manifest.json" for `lib-v8ysb-gm.json` alone (pre-existing at `883be9f2`); as a PARENT it resolves (X-10 and Y-1
  `verify` hold with u / w resolved to the v8-era dirs, done); no P9 cell runs Y-F0 alone; if a cell's preflight
  nevertheless refuses on that input, it is a driver stop before any read (UNDEFINED (0) as B.0 words it, logged) and
  the cells root hands back for the PM to rule; nothing is pre-decided - `gate-logs/repin-verify-3.txt:63`;
  `handoff-root-gate.md:138-139`.
- **P9-F0**: the last accepted P9 cell, leverage excluded (P9-X, P9-V) and P9-W excluded; equals the re-based Y-F0 if no
  P9 cell is accepted. The book of G-B1 and G-B6.
- **P9-F0h** (PRE-10, ledger :522): the last accepted book that does not read `--risk-model`; equals P9-F0 unless P9-MV
  is accepted. The book of G-B8 (PRE-10, PRE-17) and P9-W's parent and paired reference (PRE-39, ledger :604); when it
  differs from P9-F0 the adoption print shows both and labels G-B8 "history-readable book, not the deployable one"
  (PRE-10).
- **Clean-source book** (PRE-31, ledger :573): the last accepted book of the lineage that applies no rule whose
  `source_sample_end` is 2015 or later (theme-cov-hedge-v1 2017, mom-volman-v1 2016, info-clock-rate-v1 2019 (ALCLOCK-2,
  ledger :626), and mv-aim-v1 as its registration says), with the 12 drop-list members absent (what "absent" means:
  OD3P-1, ledger :624; F.1) and TRAIN weights carried (PRE-20); an OD-3 print line only, "clean-source book: no flagged
  member, no flagged rule"; decides nothing.
- **P9-F**: the levered book offered to the owner (G-B2). Rule (PRE-13, ledger :524): P9-V if it is accepted AND passes
  the X-10 rule against P9-F0 [read: P9-F0h (PRE-MVX, MVX-F0H)]; else P9-X if accepted; else none. Round 2b-ii part B1
  (MVX-PRE13, ledger :950), the ruled reading: P9-F = P9-V if accepted AND it passes the X-10 rule against P9-F0h,
  else P9-X if accepted (or, under PRE-30, the re-based Y-1 reference with Y-1's verdict of record), else none; PRE-13
  stands whole with its base read as P9-F0h (MVX-F0H), and PRE-MVX's "P9-F is P9-X's book if P9-X is accepted" (E) is
  this rule's second branch, not a replacement.
- Parent of every cell = the last accepted (plan section 5.2 :915), except P9-W (decides nothing, never a parent), P9-X
  (parent P9-F0 [read: P9-F0h (PRE-MVX, MVX-F0H)]) and P9-V (parent and paired reference P9-X, named as its lineage parent: E2-15, ledger :499). After a
  hand-run cell (P9-L, P9-X, P9-V) the cell of record a child names as parent is the plain recipe at the published
  L-prime, with no calibrate flag; any calibration run uses the run recipe (`-cal`) (PRE-27, ledger :543; C2-6, :358).
  PRE-27's calibration clause (C2-6, run recipe `-cal`) applies to P9-L only: P9-X and P9-V are `gross_match: "none"`
  (PM7-20 reading B), so for them only "the cell of record is the plain recipe, no calibrate flag" applies (PRE-37,
  ledger :602). P9-X when no P9 cell is accepted before it: B.7 (PRE-30, PRE-34). Round 2b-ii (PRE-MVX, ledger
  :889): P9-X's parent is P9-F0h (PRE-10's book), not P9-F0; the two coincide unless P9-MV is accepted; this bullet's
  "P9-X (parent P9-F0)" is stale text (PRE-40 form; B.7, B.8, E).

### A.5 Windows and seal (unchanged)

TRAIN [2020-01-01, 2024-01-01), seal 2024-01-01, research-window-v2 (`atx-impl/strategies/research_window.json`).
Nothing dated 2024-01-01 or later is opened by anyone or by code they run (plan section 0.6; `resume4-dispatch.md`
:13; `resume6-addendum.md` :10). The OD-3 eras end on or before 2020-01-01 (`atx-engine/tools/era_pool.py:40-43` refuses a
window past the seal). `atx-impl/tools/holdout_gate.py` (the OD-1 reader of 2024+, `:1-12`) never runs in P9 (PRE-21;
`resume6-addendum.md` :10). A run's own wall-clock date is not a data date: SEAL-ALLOW-3 (B.0).

### A.6 Trial counting (v8y section 3, carried; P9 rows)

| event | counts in | count |
|---|---|---|
| P9 construction or protocol cell scored (any row of B) | N_c | 1 |
| P9-X when no P9 cell is accepted before it: the re-based Y-1 reference of the wave-3 P9-B0 is the cell of record, verdict of record = Y-1's (PRE-30), provided its figures agree with the ledgered Y-1 figures (PRE-34; else root stops and the PM rules, B.7) | - | 0 |
| P9-S (no string), P9-MV / P9-V / P9-C / P9-W lapsed (B) | - | 0 |
| undefined on its parent (E-45); stopped before any return read | - | 0, logged |
| void after a return read | N_c | 1 (stays) |
| rule-7 tool or data defect: blind re-run / returns re-run | - / N_c | 0 / 1 |
| PM6-6 calibration runs (incl. P9-L's, PRE-3), P9-B0 re-bases (R-2, Y-F0, Y-1), identity runs, builds, ALD-DIAG, Q5, D-COV, D-COV-EIG, S2R-DIAG, rho measurements, ucrtbase records | - | 0 |
| adoption print; OD-3 history lines | - | 0 to N; history reads counted apart (E-41) |
| any 2020-2023 statistic read for a variant not in section B | N_tot | 1 per variant; never enters P9 |

Closing rules (v8y section 3, carried): closed list (section B); a read is a trial; no retry, re-parameterisation or
re-cast of a rejected cell (E-45); voids keep their count; unused budget lapses (plan section 5.2 :918).

### A.7 Order (plan section 5.2 :913-918; never re-ordered after a read)

P9-L -> (P9-S lapsed) -> P9-H -> P9-M -> P9-C -> P9-MV -> P9-W -> P9-X -> P9-V -> adoption print (E) -> OD-3 once (F)
-> freeze (G). A lapsed cell (PRE-14) leaves the order of the remaining cells unchanged; P9-X under PRE-30 is the
re-based Y-1 reference, not a run.

### A.8 Power, stated before any result (plan section 5.2 :927-933)

MDE at 80% power = 2.49 x SE. Combination cells (SE about .2) detect only about .5; single cells are sign-level
evidence; the decision-relevant tests are G-B6 (P9-F0 vs R-2, cumulative) and G-B8 (the history read, on E1+E2: MDE about
.40-.45, PRE-17). P9-V's pair differs only by the store's forecasts (low-variance difference; small expected effect). The
closest pair measured so far is X-10 vs Y-F0, 1 - rho 5.1e-4 (C.11).

---

## B. The cells

### B.0 Common to every cell

- **Registration before read** (plan section 0.6): the cell's template or hand spec, rule file, constants and this block
  are ruled before its first read. ids below are <= 18 characters, so they meet W1-I1 (ledger :314) at either eval mode.
  W1-I1 measured at M2h on 65 committed specs (`handoff-root-m2h.md` row (d)): worst margin 1 char (`v70-lo3`, no P9
  parent), 60 specs at margin 2 (longest candidate id `seasonality_same_month`), both AL-COMB templates among them; none
  refused at pool-2's depth.
- **Manifest.** `atx.research-wave/v2` (K-P9-9) for P9-H, P9-M, P9-C, P9-MV, P9-W. By hand for P9-L (a v1 cycle, PRE-2),
  P9-X and P9-V (`research_cycle.py`, PRE-11). E2 is merged in root (`430c29e8`, ledger :562); kinds and blocks as E2
  defines them (line citations are pool-17 `scripts/wave_kinds.py@ea7d9dde`; later rounds only added refusals, W3-14a):
  rules enter only through `rules: [{name, params}]` (ALCOMB-1); templates carry no `rules` key and no `change.flags`;
  the raw-rule-flag guard runs on the resolved flag lists of every v2 kind (E2-8); `nav.leverage` may be set only by a
  kind `leverage` wave or by the gross-match stage (E2-10); `--calibrate-gross` / `--calibrate-tolerance` are owned flags
  that only the gross-match stage may put on an argv, refused from any other route, kind or spelling (E2-14); the
  reference side is judged like the cell and no cell adds, replaces or edits a `ref` section by any route (E2-15); a kind
  `rule` wave naming `fixed-v1` is checked against the table range [1, 2], and no P9 cell is such a wave (E2-13 (a),
  (b)); `inputs.*` may be set only by the kinds `field`, `role`, `horizon` and only the keys they name, a D3 carry, and
  no cell runs before D3 merges (E2-17, ledger :542). As ruled since: the P9 cells start only after lane D3's two guards
  (the C3-3 alias guard and the E2-17 `inputs.*` guard) are merged and APPROVE (PRE-35, ledger :600, as D3-2, :629,
  reads it on the split of D3 part A into lanes D3 and D3W); no P9 cell is a v2 kind `leverage` wave (PRE-32, B.7).
- **Sign receipt (round 2b-ii; SIGN-SUBST, ledger :803, :815).** [ROUND 2b-ii: SIGN is merged (APPROVE `e2844926`,
  ledger :824; merges `ad913bd5`, `338e6a7c`, `cdc753a8`, `integration-log.md:734`), so the condition "iff SIGN merges"
  holds: a v2 wave's screen stage writes the sign receipt `atx.sign-receipt/v1` (class `ic-output`, SQL2-CLS
  `1dd9b9b1`) as `out/waves/<w>/sign/receipt-<k>.json` with `rows-<k>.json`, and the screen receipt gains the key `sign`
  `{exe, exe_sha256, rows, rows_sha256, receipt, receipt_sha256}`; it is a per-wave file the judge pins, NOT a
  substitution: the P9-B0 references (R-2, Y-F0, Y-1) are hand NAV / fit runs with no screen stage, so no reference dir
  gains a `sign/` file - `root-wave3-merge-report.md:845-849`; `task-SIGN-report.md:151-163`; ledger :803].
- **Plan Appendix A (round 2b-ii; PY-S S-4, ledger :904).** The plan's Appendix A example `research_cycle.py wave run
  scripts/specs/v8/waves/y-s.json --until screen` (`docs/plans/2026-10-03-p9-sprint-plan.md:1040`) is stale text: it
  stops at preflight (exit 3) because the ledgered v1 manifests name the retired `pm6-6` (C2-11;
  `wave-review-python.md:195`). The argv of every P9 cell and reference is this file's (B.0 and the cell blocks; the
  accounting block is E's); the plan is not edited; the freeze report carries S-4's text (PY-S).
- **Gross match.** Every gross-matched P9 template names `gross_match: pm6-6-engine` explicitly (C2-9, C2-11). The engine
  route on real exes (tiny-world, M2h; `handoff-root-m2h.md` rows (b), (c)): the calibration run (`-cal`) at the cell's L
  moved to L-prime with one correction, `matched` true; the cell of record is the plain recipe at L-prime with no
  calibrate flag; verify pins the match record digest; a literal `aim_leverage` other than the cell's L stops at match
  (exit 3). The reference keeps its own pre-calibration leverage on the engine route (E2-16, E2-16a, ledger :541,
  :564); C3b retires the Python `pm6-6` route (C2-11) and adds the test that pins `ref.leverage` unchanged across the
  engine match stage (E2-16a); C3b merges before any cell (E2-16).
- **exes.** Every cell names and pins `exes.ic`, `exes.nav` and `exes.eval` by `lock --exes` (E1-REUSE-a2, ledger :153;
  B2-10 (c), :400). For a v2 cell verify checks only that `exes.eval` is named and pinned; the judge runs `nav_summ` on
  the PINNED verb and refuses to publish "Statistics of record" when the verb on disk or the hash in `nav_summ_run`
  differs from the pin (B2-11, :427); for the hand cells that check is done and logged by hand (PRE-11). All are Release
  exes (M2c-RED-2 (e), :441). Every identity, probe or run of record states the exe's build tag, SHA-256 and source
  commit, and uses an exe only if `git diff <source>..HEAD` over the C++ / CMake files of its target is empty; otherwise
  that target is rebuilt first (EXE-CUR, :567), and after the wave-3 re-pin a rebuild redoes P9-B0 and the re-pin before
  any further cell (EXE-FREEZE, :406). [ROUND 2b-ii: the re-pin of record `2078f877` (pins only, over the gate code
  `06dc9e5a`) pins the Release build tag p9-3f, source commit `06dc9e5a` (receipt Source `5f48a25d`, a docs commit;
  `git diff 5f48a25d..2078f877` outside `.superpowers/` = the three re-pinned specs only, no C++ / CMake file;
  receipt `build-equity/mega-p9-3f-receipt.json`): `exes.nav` `build-equity-rel/bin/atx-equity-strategy-targets.exe`
  `255b58acedf7df82a1caa672ff30a58c317a8f0dccf7dbe7d3b7393b91063565`, `exes.ic`
  `build-equity-rel/bin/atx-equity-strategy-ic.exe` `ccd54db605f67fef1aa480050a6c785ffe179dae2f77e978dd6d01a5c117d705`,
  `exes.eval` `build-equity-rel/bin/atx-research-eval.exe`
  `e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c` (named and pinned, B2-11), in
  `lib-v8ysb-gm.json`'s `exes` / `exes_sha256` (templates inherit them, `scripts/cycle/lock.py:24-28`) [re-stated,
  FLAGS-ROUTE `f8d852f3`: `lib-v8ysb-gm.json` is now `a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a`
  (was `3631679c...edea` at the re-pin; `nav.flags` only, its `exes` / `exes_sha256` unchanged); the re-pin of record
  stays `2078f877`; no pin moved; EXE-FREEZE unaffected]; every other exe
  of the frozen set (composition, admission, store, risk, fields, ...) at step 1's table, all in "## Round 2b-ii
  record"; 27 / 27 Release and 27 / 27 Debug `disk = receipt` at the gate's end; EXE-FREEZE from `2078f877` (NAV, IC,
  eval, composition, admission, store; GATE-EXES); the follow-up `3a1a4759` (REPIN-UW / REPIN-R2) moved no exe pin, was
  refused at `verify` and is reverted by `f8a85ad4` (REPIN-REV); the code head `f8a85ad4` equals `2078f877` in
  `scripts/specs/v8/` - `handoff-root-gate.md:21-22`, `:28-69`; `root-gate-report.md:191`, `:925-969`, `:1087-1112`].
- **Seal allowance (SEAL-ALLOW-3, ledger :563).** `--seal-allow <the run's own UTC date(s)>=SEAL-ALLOW` is part of the
  driver argv of every P9 wave, hand cycle, calibration and identity run. Root passes it only after a per-hit source
  check (`tools/sealsrc.py`) shows every hit of the token is a wall-clock `started_utc`-type stamp written by the run's
  own receipts or consoles; any other token or source stops the run. It is not an input difference under the void rule.
  Re-stated (round 2b-ii part B2b; SEAL-REF, ledger :996), against "and identity run": `--seal-allow` is a DRIVER argv
  flag (`research_cycle.py`'s seal scan of a wave / hand cycle / calibration run); the P9-B0 references and the gate's
  identity runs invoke the exes directly through the bounded runner, which runs no seal scan, so the flag has no object
  there and appears on no reference argv (substitution list: "`--seal-allow` on none"); every cell run through the
  driver carries it after `tools/sealsrc.py`.
- **Gross figure opt-in (PRE-7a, ledger :538).** Every P9 cell and its reference name the opt-in `--ann-mean-gross`
  (NavSpec `options.ann_mean_gross`) alike at the wave-3 P9-B0; print-only, not a rule flag; definition C.22 (C3-9).
  [ROUND 2b-ii: C3-6 HOLDS, cited from M2j: the flag-absent NAV of X-5 (`p9-m2j-x5-argv` vs its reference of record
  `p9-c1-s2-x5-nav`) and of Y-F0 (`p9-m2j-yf0-argv` vs `p9-b0-yf0-nav`) 26 / 27 files byte-identical, `summary.json`
  differing only at `/producer/build_type`, `/producer/engine_git_sha`; `--spec` runs 27 / 27 byte-identical to the argv
  runs; the canaries 15 on both goldens; the opt-in itself changes only `summary.json`, by `/scenarios/0..4/ann_mean_gross`
  (opt-in side only; stripped = the flag-absent bytes); so no gross-alpha line is cut (PRE-7, PRE-7a); at the gate the
  opt-in again adds only those leaves on the R-2, X-10 and Y-1 re-bases - `handoff-root-m2j.md:42-44`,
  `handoff-root-m2j-fin.md:44`; `root-gate-report.md:148`; `gate-logs/x10-cmp.txt`, `r2-classify.txt`].
  [FLAGS-ROUTE, `f8d852f3`: the route: every P9 cell inherits `--ann-mean-gross` (and `--info-clock-print`) from the
  lineage parent's `nav.flags` (`scripts/specs/v8/lib-v8ysb-gm.json`,
  `a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a` after `f8d852f3`; the v2 templates set `parent` to
  it at registration and carry no `nav.flags` override; the driver builds the NAV argv from the resolved spec's
  `nav.flags`, `scripts/cycle/phases.py:824`); the P9-B0 references carry both by hand on their exe-direct argv
  [SG4-REF: of the P9-B0 NAV references only `p9-b0g-yf0-nav` carries `--info-clock-print` (with `--ann-mean-gross`);
  `p9-b0-r2-nav`, `p9-b0g-x10-nav`, `p9-b0g-y1-nav` carry `--ann-mean-gross` only (their receipts); every spec over the
  lineage parent carries both (FLAGS-ROUTE); the one-sided case is FLAGS-ROUTE-2 (b)]; the
  spec route reproduced `p9-b0g-yf0-nav` outside `/producer/*` (HOLDS: the driver-planned spec-route NAV of Y-F0
  (`build-equity/p9-fr-yf0-nav`, Release NAV p9-3f, bounded run exit 0) vs the reference `p9-b0g-yf0-nav`: 27 / 27
  files byte-identical (no `/producer/*` leaf differs: the same Release exe); the 75 opt-in leaves (5 `ann_mean_gross`,
  70 `info_clock`) present and equal on both sides; `gate-logs/flags-identity-yf0.txt`); inheritance as root recorded
  it: `x-leverage-L2.0.json` has no `nav.flags`, `y-vol-target-y-1.json`'s `change.flags.nav` only adds, the four
  templates set outputs only, so every one inherits both flags once (`root-gate-report.md` "FLAGS-ROUTE (RQ-1 close)",
  `handoff-root-gate.md` "FLAGS-ROUTE amendment", root docs head `5ec208b4`)]
- **NavSpec (`atx.nav-spec/v1`, C3 part A; part A merges on its APPROVE, part B is round C3b, reviewed and merged
  before the wave-3 gate, C3-2, ledger :508).** A spec may restate a compiled constant only at its compiled value and may
  narrow `leverage_range` inside the registry's [1, 2] (C3-1, ledger :507); no P9 cell changes the cost law (C3-3);
  `construction_grid` and the no-op options: C.23 (C3-7, C3-8); the decide-path seal deviation: C.24 (C3-10).
- **Eval mode** = the P9-B0 reference's (PATH-1, ledger :416). Expected research-fast: no ruling switched S2's default.
  No P9 cell is registered under audit-exact, so no short cache dir is needed. [ROUND 2b-ii: research-fast (the
  default): the re-based Y-F0 u receipt's argv carries no `--eval-mode audit-exact`, and the Release VM identity is
  `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0`, without the `_audit` suffix; every cell runs in its reference's mode
  (PATH-1) - `gate-logs/receipt-p9-b0g-yf0-u.json`, `yf0u-cmp.txt`; `root-gate-report.md:192`; `handoff-root-gate.md:124`].
- **Fit mode** = the P9-B0 parent reference's (ALCOMB-7, ledger :424): `--engine-fit` only if lane D3W (ALCOMB-7 is
  D3W's, D3-2) delivered the C++ sleeve and the wave-3 P9-B0 re-based the parent under engine-fit, else flag-absent.
  Stated per cell below. Lane fact (round 2b-ii part A): D3W delivered the C++ sleeve (ALCOMB-7; `--engine-fit
  --mom-volman` writes it; D3W merged `08f04e07`, APPROVE `1e6c4336`, ledger :694; `task-D3W-report.md:20`); whether
  the wave-3 P9-B0 re-based the parent under engine-fit is D3-Q1 / D3-Q1a's outcome at the gate (part B). [ROUND 2b-ii:
  flag-absent for every cell: D3-Q1a DIFFERENT at the gate (the engine fit's stripped weights differ from the Python
  fit's at 694 float leaves; admission files identical), so P9-F0h is produced flag-absent (the Python fit) and the
  parent reference is not re-based under `--engine-fit`; the Y-F0 fit is not re-based at P9-B0 (its flag-absent
  identity holds outside D2's code-identity stamps, `gate-logs/fit-identity-yf0.txt`) - `gate-logs/d3q1a.txt`;
  `root-gate-report.md:151`, `:482-489`, `:628-630`; ledger :975 ("Consequence for every v2 cell: fit mode =
  flag-absent"), :981 (LAPSE-TABLE)].
- **Fields producers: the A4-Q2 entry (round 2b-ii; A4-DB and A4-P10-FLIPS, ledger :867, :875-876, :883).**
  [ROUND 2b-ii: both flipped fields are in Y-F0's manifest; identity not run: inputs under atx-db; flips P10. A4 stays
  merged at its pre-flip head `6dab6948` (merge `bba4c06f`: the engine builders, kinds and their tests, flag
  absent); the flips `69141218`, `59d807ea`, the registry rows of `fc3e414e` and the fix-1 tests (`87df9e00`) are P10
  whole; Python stays the producer of record of `ftd_shares_ratio21` and `regsho_threshold_days63`; Y-F0's fields
  manifest `26fee5ce`
  (`build-equity/train-2020-2023-lo3-fields-v15/manifest.json`) is unchanged; no P9 run reads the engine builders -
  ledger :867, :875, :876, :883; `root-wave3-merge-report.md:910-944` (M5c); `integration-log.md:735`, `:746`].
- **Lane suites (round 2b-ii; the PRE 2b-ii list, ledger :699; TEST-1).** The lanes that ran under TEST-1 ran their
  named filters and their own pytest files and recorded the whole suites (whole targets, Release builds and filters,
  tiny-world canaries, the whole `scripts/tests` at seeds 0 / 1, the stage chain, as each lane lists them) as
  "deferred to the gate (TEST-1)" (`task-AL-CLOCK-report.md:488`, `task-COV-MV-report.md:443`,
  `task-D3-report.md:116`, `task-D3W-report.md:133`, `task-D3W-PB-report.md:143`, `task-OD3P-report.md:298`,
  `task-SIGN-report.md:81`); they run once at the gate on the gate head; their counts are the gate report's (part B),
  not a lane's. [ROUND 2b-ii: the gate's step 2 on the gate head `06dc9e5a` (TEST-2, ledger :973: what ran stands;
  every cut-list item had already run and is recorded as run; none "not run"): Release gtests whole (p9-3f) 16 / 16
  targets, 4,098 run, 0 failed, 9 named pre-existing skips; Debug gtests whole (p9-3g) 16 / 16, 4,099 run, 0 failed, 6
  named skips; `ctest -L atx_research` 291 and `-L atx_equity_strategy` 728 on both trees, 0 failed (1 disabled test,
  `CovStageDigest.DumpPerStageOnTheLaneFixture`); `scripts/tests` seed 0 on Release: agent 1's run 1 failed / 702 passed
  / 3 skipped, the one red GATE-TMP's (the tool's TMP root, W1-I1 refusing a 327-character path), re-run on the short
  root 703 passed / 3 skipped / 0 failed, and the GATE-TMP case alone 1 passed; seed 1 703 passed / 3 skipped / 0
  failed (the 3 skips the opt-in live-root cases of `test_research_cycle.py`); `atx-engine/tools/test_stage_chain.py`
  10; `atx-engine/tools` 391; `atx-impl/tools` 770 on the OS TMP (on the short TMP one TMP-length-dependent golden,
  `test_mega_report_v8.py::test_v7_config_renders_the_pre_v8_bytes`, fails: a test-fragility P10 note, GATE-TMP-2,
  ledger :976); `atx-impl/strategies` 14; the fields pytests 4 / 26 / 16 / 37; the mirror guard 7 (G-P5: 0 rows left
  for P9 lanes); golden `0x889874a3b9b29c55` at 1 and 4 workers OK on both builds; canaries 15 / 15 on both builds;
  GATE-COVDIFF three `cov-diff --legacy` runs exit 0 after the COV-9 check; known-red set EMPTY - `root-gate-report.md`
  :240-265, :336-346, :353-375, :381-399, :401-410, :412-459; `gate-logs/gt-exits.txt`, `ctest-exits.txt`,
  `pyt-exits.txt`, `covdiff-runs.txt`].
- **Void** (v8y section 12, carried; P9 additions 5-7): (1) run before this file is ruled, before the wave-3 P9-B0 and
  its re-pin commit (W1-I2 (b)), or before ALD-DIAG is printed (OD5-1 (iii); printed at M2g, C.2); (2) any input differs
  from its block (string, constant, template, hand-spec or registration SHA-256, parent not the last accepted, flag,
  fields dir, L other than PM6-6's correction or the registered 2.0, store pin, exe pin, eval or fit mode; the
  `--seal-allow` flag of SEAL-ALLOW-3 is not an input difference; `--ann-mean-gross` must be named alike on cell and
  reference, and `--info-clock-print` alike on P9-C and its reference, ALCLOCK-1) [FLAGS-ROUTE-2, `f8d852f3`: by
  inheritance both flags are on every spec over the parent, so they are alike for every v2 cell against
  `p9-b0g-yf0-nav` or an accepted v2 parent (and `--ann-mean-gross` on every cell / reference pair: every P9-B0 NAV
  reference carries it); a hand cell written from P9-F0h's argv (P9-X, P9-V if they run) against `p9-b0g-y1-nav` or
  P9-X may carry `--info-clock-print` one-sidedly: print-only, byte-neutral but for `/scenarios/*/info_clock`
  (ALCLOCK-1), disclosed in the cell record, NOT a void. FLAGS-ORDER (ledger, RESUME 14 block): argv token ORDER is
  not an input under this rule (2) (the flag SET is): the driver's planned argv places `nav.flags` (ending
  `--ann-mean-gross --info-clock-print`) before the `--label-role` pair (`scripts/cycle/phases.py:832`) while the
  exe-direct references carried the two flags after it; the two argvs resolve to the same NavSpec
  (`gate-logs/flags-printspec.txt`) and the identity is byte-exact (`gate-logs/flags-identity-yf0.txt`)]; (3) a
  return, IC, Sharpe or NAV statistic of the cell read before its mechanics passed; (4) a session at
  or after 2024-01-01 opened; (5) an exe rebuilt after the re-pin without P9-B0 redone (EXE-FREEZE); (6) `ucrtbase.dll`
  version or SHA-256 differs from the P9-B0 record (M2c-RED-2 (d); a change before a cell redoes P9-B0 and the re-pin
  but does not void the two D-COV stores, CRT-2); (7) a SQLite cache index flag on any argv (SQL-11, ledger :212). Not
  voids: rule-7 defects; mechanics fail (rejected, counted); undefined on its parent (0); a driver stop.
- **Standing lines** of section C apply to every cell.

### B.1 P9-L: survival-conditioned IC labels (protocol cell; DEC-17)

- **id** `p9l-label-terminal`. **Class** protocol (B0c precedent). **Counts** N_c +1 if run; K_a 0.
- **Shape (PRE-2, ledger :517).** By hand as a v1 cycle: `research_cycle.py run` of a template root writes and commits
  before the run, because no v2 kind carries `--label-terminal` (kind `rule` needs a non-empty `rules` list, pool-17
  `wave_kinds.py@ea7d9dde:161-162`). The v2 guards do not run on it, so this block names every flag. [ROUND 2b-ii:
  filled at the cell's registration commit by root - Appendix IV item 7] (root writes
  `scripts/specs/p9/templates/label-terminal.json` from this block and records its SHA-256 here in that commit).
  [filled, P9-L registration commit `0bba6c89` (cells root agent 1): `scripts/specs/p9/templates/label-terminal.json`
  `7483f4b10513d9000cfc89036c3f8f7f49626a9dd9c57999bcce11a258754219` (after `lock --write --exes`; parent and
  nominal_parent `../../v8/lib-v8ysb-gm.json`); the change is `change.flags.ic` as "Change" below; it names no
  `nav.flags`: it INHERITS `--ann-mean-gross` and `--info-clock-print` from the parent's `nav.flags` (FLAGS-ROUTE;
  each once on the planned NAV argv, shown in the commit body); the run recipe `label-terminal-cal.json`
  `7783967471f5b06dc1407d4425605156f7a3e647f28edd2a3ca5f2fb1a989d37` (`6ea79e5a`; `--calibrate-gross` G_parent
  `0.9862134133265011`, `--calibrate-tolerance 0.005`, PRE-3); the limits and the verdict arithmetic in
  `cells/p9l-label-terminal-plan.md`; the cell of record `label-terminal-gm.json` follows the calibration run (PRE-27)]
  [PRE-40 restatement, P9L-WBUDGET, 2026-10-17, `f560f741`: only w's IC admission
  `--max-memory-mib` and runner `--max-rss-mib` become 3584; an admission budget, not a research input.
  Template SHA-256 `1d15513e74851e776a7e40d538f528690fbab04c4a92bb8afdcc240a805e3170`;
  regenerated run recipe SHA-256 `1f00e19ec9dc3092ebe570b9645700910062881835dc9cd391623cff76c00062`.
  G_parent, tolerance, flags, pins and all other settings stay as registered. Resume w attempt 2 after
  free RAM >= 4608 MiB and C: >= 15 GB; completed u / fit / card and failed w run1 are preserved.]
  [PRE-40 restatement, R15-RAM-NOW, owner direction 2026-10-17: the extra RAM gate and RAM waits above
  are superseded. Omit `--admission-wait` and `--host-budget-mib`; launch immediately under the unchanged
  w IC / RSS 3584 MiB caps, 300 s limit and runner launch/runtime free-memory floor 512 MiB.
  C: >= 15 GB, one bounded run at a time, frozen exes and all read checkpoints still apply.
  Any actual bounded resource stop is preserved and reported immediately, without automatic retry or waiting.]
- **Change.** `--label-terminal imputed-v1` with `--train-label-role <label role> --train-label-role-sha256 <pin>` on
  the IC passes (u and w), on the parent; fit, card, w and NAV follow (plan section 2.4 S2 :490-498;
  `task-S2-report.md:23`). Label role: `build-equity/train-2020-2023-lo3-dlret`
  (`95e16cfe3ad0e0c9e7cf94acb6df4004e6f286ae59dff7c0e725ea33aace5069`, the parent spec's `inputs.label_role`,
  `scripts/specs/v8/lib-v8ysb-gm.json:53-57` [read at the re-pin of record `2078f877`: `:59-63`, the same text]). No
  constant.
- **Every flag (PRE-2).** Differing from the parent reference's argv: on the u and w IC argv `--label-terminal
  imputed-v1` and the label-role pair above; on the NAV calibration run (`-cal`) only `--calibrate-gross <G_parent>` and
  `--calibrate-tolerance <the engine stage's tolerance>` (PRE-3, PRE-27); on the NAV cell of record `--aim-leverage
  <published L-prime>` and no calibrate flag (PRE-27). Carried alike on cell and reference (not a difference):
  `--seal-allow` (SEAL-ALLOW-3), `--ann-mean-gross` (PRE-7a), the pinned exes, eval mode, fit mode, the run caps (C.14).
  Never on any P9-L argv: a rule flag, a `--risk-model`, a cache index flag (SQL-11), any other changed input.
  Re-stated (round 2b-ii part B2b; SEAL-REF, ledger :996): `--seal-allow` is "carried on every driver-run argv; absent
  from an exe-direct reference run by construction" (the P9-B0 references were exe-direct bounded runs; B.0 "Seal
  allowance"); it is not an input difference (B.0 "Void"). [FLAGS-ROUTE, `f8d852f3`: the route of `--ann-mean-gross`
  (and `--info-clock-print`) on P9-L: its hand v1 template root is written from this block over the parent; if it names
  `nav.flags` itself it names both flags (B.1 "this block names every flag"), else it inherits them from the parent's
  `nav.flags` (B.0 "Gross figure opt-in"); the cells root states which in the registration commit; either way alike
  with `p9-b0g-yf0-nav`]
- **Parent / paired reference.** Y-F0 as re-based at the wave-3 P9-B0 (PRE-22). [ROUND 2b-ii: = item 2 (A.4): the
  lineage parent spec is the re-pinned `scripts/specs/v8/lib-v8ysb-gm.json`
  `3631679cd18f1c6a63a6a5dc96cdbe6ee7669b81b8979f529544f1b237d9edea` [re-stated, FLAGS-ROUTE `f8d852f3`:
  `a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a`; the re-pin of record stays `2078f877`; no pin
  moved; EXE-FREEZE unaffected] (the re-pin of record `2078f877`; root sets the
  template's `parent` to it at the cell's registration commit and `lock --exes --write`s the cell); the paired parent
  NAV `build-equity/p9-b0g-yf0-nav` (listing `e205d6ecb16e75cba02194b90b313849404c2052b3e1738c4bf020e486fcf797`,
  `summary.json` `826d5ec65329c24c4c0c1b72ee1f57dcdbdb136cf00d4113c6463310e79b8f38`, made by the pinned Release NAV
  exe: W1-I2 (a) holds); the u / w references of record `p9-b0g-yf0-u` / `p9-b0g-yf0-w`, while the parent spec's
  `ic.u_output` / `ic.w_output` stay the v8-era cycle dirs `mega-v8-b0b-train-u-v8ysb-1` /
  `mega-v8xw-train-theme-erc-v8ysb-2`, the cycle's resume roots (REPIN-REV; byte-identical payloads) -
  `handoff-root-gate.md:78-80`, `:127-137`; `root-gate-report.md:193`]. [SG-3, P9-L registration: at docs head
  `a32b6f4c` these read `handoff-root-gate.md:79-81` (the three Y-F0 reference rows), `:133-143` (the lineage parent
  and the two roles of Y-F0's u / w); `root-gate-report.md:193` (item 8, unchanged)]
- **Gross match (PRE-3).** `gross_match: pm6-6-engine`, through the engine calibration: one zero-trial calibration run
  (run recipe `-cal`); the cell of record that a child names as parent is the plain recipe at the published L-prime,
  with no calibrate flag (PRE-27, C2-6). Otherwise the first v2 wave after P9-L is refused at preflight (E2-14 R3a;
  E2 re-review 4 caveat, ledger :540).
- **Acceptance (decides; PRE-5).** Mechanics only (G-B9 limits); adopted as the new parent iff mechanics pass. The
  driver has no "mechanics only" rule (`scripts/wave_rules.py:143`): the mechanics verdict is derived by hand and
  re-derived independently by a second agent before the ledger line is written (PRE-5). [SG-3, P9-L registration:
  `scripts/wave_rules.py:143` reads `:158-161` at code head `f8d852f3` (the `ACCEPTANCE` table: `pm7-34`,
  `v8-prereg-5`; no mechanics-only rule); the hand rule is `v8-mech` (`MECHANICS`, `:92-93`) at the G-B9 limits
  unscaled, fixed in `cells/p9l-label-terminal-plan.md` before the run]
- **Printed only.** Paired dSR vs the parent and both p (the pinned verb through `nav_summ`, `atx-impl/tools/
  nav_summ.py:205-210`); orientation flips (u `orientations.json`, parent vs cell); admission changes (fit
  `admission.json`, parent vs cell); the IC exe's `ic_missing_exit_v1`, `terminal_imputed_pairs`,
  `missing_exit_share` and `guard_excluded_pairs` (`task-S2-report.md:23`, `:195-204`).
- **Predictions.** None registered (protocol cell; plan section 7.2 gives -.02 to +.02 [est], not a prediction).
- **Fit mode** [ROUND 2b-ii: flag-absent (the Python fit): the parent reference's mode, D3-Q1a DIFFERENT at the gate,
  so P9-F0h is produced flag-absent (B.0 "Fit mode") - `gate-logs/d3q1a.txt`; `root-gate-report.md:151`; ledger :975,
  :981]. Eval mode: B.0. [SG-3, P9-L registration: `root-gate-report.md:151` (a `65ce28f0`-era line) reads `:173`
  at docs head `a32b6f4c` (the fit mode row of items 9, 12, 15)]
- **Exposure (PRE-4, ledger :517; S2-FIX1 (3), :325).** Under `--label-terminal` a delisting cause may be classified up
  to 30 days after termination T, so labels whose T falls within 30 days of TRAIN end can embed post-TRAIN information.
  Stated, with no truncation code; bounded by the 2 exits ALD-DIAG prints as "unclassified (within 30 days of the seal)"
  (C.2; ALD-10).
- **Inputs (S2-6, ledger :365; `task-S2-rereview-1.md:62-68`).** Before the run root shows that no `imputed-v1`
  IC-result entry written by a pre-`110fd735` build exists under any cache root the cell reads (M2c found none,
  ledger :412); one found is deleted by name and logged, never reused. Post-fix rule, stated: the terminal step T =
  L+1 is guard-checked on the label role (|log r| > 1.5 or adjusted vs raw drift > .10 excluded and counted in
  `guard_excluded_pairs`; the builder's Shumway set {0, -.30, -.55} passes) (`task-S2-report.md:195-204`).
- **Undefined (0).** The parent's IC passes refuse `--label-terminal` with its flags; no label role pinned. **Lapsed:**
  never (S2 merged).

### B.2 P9-H: `theme-cov-hedge-v1` (DEC-15; first hypothesis cell)

- **id** `p9h-cov-hedge`. **Kind** `rule`, `rules: [{name: theme-cov-hedge-v1, params: {}}]` (`params {}` = every
  registered constant). **Counts** N_c +1 if run; K_a 0.
- **Registration** `scripts/specs/p9/rules/theme-cov-hedge-v1.json` (sha256 `55f25f2f...`); template
  `scripts/specs/p9/templates/theme-cov-hedge.json` (`7c6778bb...`, = the registration's `template_sha256`). Root sets
  `parent` in the template and re-pins (registration `manifest_note`); the allowed diff is `/parent` plus the "ex-ante"
  cut below, checked by JSON diff (R0-9-PIN precedent, ledger :127). [ROUND 2b-ii: filled at the cell's registration
  commit by root - Appendix IV item 10]. **Stale note (PRE-40, ledger :623).** The registration's `manifest_note` "gross_match pm6-6" is
  stale; a pinned registration is not edited (an edit would move a registered SHA-256 for a comment). Every P9 template
  names `pm6-6-engine` (C2-9, C2-11); a manifest that follows the note literally is refused at preflight by C3b's
  retirement of the Python `pm6-6` route; root checks that the template names `pm6-6-engine` before the re-pin.
  [PRE-40, P9-H registration `818e0750`: template SHA-256
  `1f56c19428b7b62de9788e37ec21bf20a78f122e411fb8fb948a0cc0347ec082`; registration SHA-256
  `6b4c3b0d3eebdf5752e41394d316766e75a1b0df22552bc85449740ae36f2516`. Closed JSON diff in the commit body:
  `/parent` = `label-terminal-gm.json`, description removes `ex-ante and `, registration template hash consequence
  only; `manifest_note` unchanged. The actual gross-match key is on the new v2 wave manifest
  `scripts/specs/p9/waves/p9h-cov-hedge.json` (`a27afd5a301f13f7037f3e85c29e0765c24f1121f56913536da1c563becb781a`),
  `pm6-6-engine`; the template has no literal gross_match key. The paired parent is accepted P9-L's
  `mega-nav-p9l-label-terminal-gm`; its plain template SHA is `de44a668999ab8997c7118afb52efd93551ac2e7df93efe9118edbf719aa9a4d`.
  Required NAV print flags each occur once by inheritance; `cells/logs/p9h-resolved-plan.txt` records them.
  No measurement yet: generated-cell lock and internal-commit route remain pending the PM's route ruling.]
  [PRE-40, R15-H-READY / R15-H-RESTART, metadata amendment `708ab6ce`: attempt1 stopped before any data phase
  because the native guard treats all four textual requires as open. PM discharged each on the committed
  `cells/p9h-prerequisite-closure.md` evidence. New `theme-cov-hedge-ready.json` changes only `/requires` to `[]`,
  SHA-256 `5e9eae0aa660e9de3e538a1e659b16234e36597cd4e6dc07eecc956740d7f168`. New
  `scripts/specs/p9/waves/p9h-cov-hedge-attempt2.json`, SHA-256
  `c4d682c33fb216c2c4915a8dee778267d4c6cfe2c9f53f6551b8cc938acd4c81`, changes only `/out_dir`,
  `/rule_cell/template`, `/rule_cell/template_sha256`, `/rule_cell/name`; full closed delta in commit body and
  `cells/p9h-attempt2-registration-commit.txt`. All scientific keys, parent, rule, pins, outputs and caps remain;
  original template/rule/manifest, generated `21c78bfd` spec and eleven stopped files are preserved. R15-SEAL-C
  classifies only the evidenced exact Git exclusion-token source. Updated read plan retains mechanics then rho
  then paired read order. Attempt2 awaits PM handback verification; G-P1 zero-spec-edits-on-retry is UNMET.]
- **Rule and constants.** Per theme t and decision d on ew-theme-std-v1's re-rank R_t: beta of each name on its own
  theme sleeve over the trailing 252 decisions ending d-2 (>= 126 pairs), orthogonalised to R_t; H_t = its centred
  rank; h_t = OLS slope of the sleeve on the hedge leg, re-estimated every 21 decisions from decision 253 (0 before);
  the theme adds W_t (R_t - h_t H_t). Constants lookback 252, lag 2, step 21, min_pairs 126 (registration
  `constants`). h_t unbounded and the hedged re-rank not re-standardised (S-2, ALCOMB-5; registration `fixed_choices`).
- **Parent.** The last accepted; its fit must be `--composition ew-theme-std-v1` or `ic-shrink-v1` (+/- `--theme-erc`,
  `--theme-tsmom`, `--mom-volman`), no theme-resid / two-speed (template `requires`; `atx-impl/tools/
  composition_theme_hedge.py:27-30`). Y-F0's fit is `ew-theme-std-v1 --theme-erc theme-erc-v1`
  (`lib-v8ysb-gm.json:205-213` [read at the re-pin of record `2078f877`: `:211-219`, the same text]): defined.
- **Registration gate: registered (PRE-6, ledger :518).** Rule (the blind round-1 recommendation, adopted unchanged; PM
  note on order, ledger :515): P9-H is registered unless q_lit < .05. Q5 (`scripts/specs/p9/diagnostics/
  al-comb-q5-style-variance.json`, `721889d1...`) ran at M2g on Y-F0, the parent of P9-H (ALCOMB-10, ledger :512: the
  spec's nominal X-5 is a stale default) (`root-wave2-gate-report.md` item 5; ledger :510):
  - **q = +0.0502**: size +0.0334, residual_vol +0.0275, liquidity -0.0107;
  - **q_lit = +0.0671**: q plus beta +0.0091, leverage +0.0078;
  - sessions complete 1,005, incomplete 0; spec arithmetic bound on the SR factor 1 / sqrt(1 - q) = 1.0261,
    1 / sqrt(1 - q_lit) = 1.0353; the Debug cross-check printed every share identically.
  - q_lit = .0671 >= .05: registered.
- **gross_match** `pm6-6-engine` (plan section 5.2 :916-917; C2-9, C2-11). **Acceptance (decides)** `pm7-34`: paired S2
  net dSR > 0 vs the parent AND mechanics (`scripts/wave_rules.py:143`).
  [SG-3, P9-H registration: current `scripts/wave_rules.py:174-181` implements this predicate; `:96-114`
  implements v8-mech. The historic line citation above is retained.]
- **Registered predictions and producers** (ALCOMB-9 (b), ledger :448; PRE-7, PRE-7a, C3-9; PRE-25):
  - realised vol ratio (cell / parent) < 1; producer (PRE-38, ledger :603, correcting ALCOMB-9 (b)'s
    `atx-research-eval`, which prints no vol: its moments are sessions, sr_daily, skew, kurtosis, `atx-engine/src/
    research/eval/eval_report.cpp:41-46`): the NAV exe's C++ scenario `ann_vol` (`atx-impl/src/
    strategy_nav_replay.cpp:2001`) at the S2 primary scenario, one value per NAV dir; the ratio cell / parent is the
    reader's division of two C++-printed numbers, not a Python statistic (G-P5). No line is cut (PRE-38).
  - gross-alpha ratio "about 1", printed against the band [.90, 1.10] (PRE-25): `ann_mean_gross` of the cell over that
    of the parent, read at the S2 primary scenario, "gross of costs and financing" (PRE-7, PRE-7a, C3-9; C.22). No
    Python sum (PRE-7). The line stands only if the opt-in is merged and byte-neutral before this file is ruled; else it
    is cut (PRE-7, PRE-7a; B.0 placeholder).
  - "ex-ante vol ratio" (template text): CUT. Q5 reads one book and printed the ratio as "not produced" (gate report
    item 5); ALCOMB-9 (b) keeps the line only if Q5 prints it. [ROUND 2b-ii: filled at the cell's registration commit
    by root - Appendix IV item 11] (root removes "ex-ante" from the template description in the same commit as the
    re-pin above).
    [PRE-40, P9-H registration `818e0750`: Appendix IV item 11 filled; exact description cut
    `book ex-ante and realised vol ratio` -> `book realised vol ratio`. No prediction or constant otherwise changed.]
- **Printed only.** Hedge ratio per theme and block (IC exe `composition_hedge_trace`, AL-COMB S-3); turnover per unit
  gross (`scripts/wave_readers.py`, `book`); cost per traded dollar (C++ `cost_bps_per_traded_dollar`,
  `atx-impl/src/strategy_nav_v7.cpp:360`); net Sharpe at 4x NAV (`capacity_curve.csv`, `strategy_nav_v7.cpp:357-382`).
- **Fit mode** [ROUND 2b-ii: flag-absent (the Python fit): the parent reference's mode, D3-Q1a DIFFERENT at the gate,
  so `--engine-fit` is on no P9-H argv (B.0 "Fit mode") - `gate-logs/d3q1a.txt`; `root-gate-report.md:151`; ledger
  :975, :981]. Eval mode: B.0.
  [SG-3, P9-H registration: the cited historic fit-mode row now reads `root-gate-report.md:173`.
  Resolved accepted P9-L fit is ew-theme-std-v1 plus theme-erc-v1, flag-absent, without resid/two-speed;
  H retains it and the registered rule supplies the theme hedge.]
- **Undefined (0).** Parent outside the list above; the exe or fitter refuses. **Lapsed:** not applicable (registered,
  PRE-6).
- **Expected effect** +.05 to +.15 [est] (registration `predictions.expected_effect`), printed, deciding nothing. On
  history the hedge is 0 in each era's first 252 decisions (PRE-26; F.1).

### B.3 P9-M: `mom-volman-v1` (DEC-16: distinct from Y-2 and Y-1, ledger :79; Y-2 read at R0-8, ledger :138)

- **id** `p9m-mom-volman`. **Kind** `rule`, `rules: [{name: mom-volman-v1, params: {}}]`. **Counts** N_c +1 if run.
- **Registration** `scripts/specs/p9/rules/mom-volman-v1.json` (`8a7222fa...`); template
  `scripts/specs/p9/templates/mom-volman.json` (`7e9b26c1...`, = registration). In the registration commit, before any
  read, root sets `parent`, corrects the template description (text only, PRE-8) and re-pins `template_sha256`.
  [ROUND 2b-ii: filled at the cell's registration commit by root - Appendix IV item 13] (PRE-8's description; the
  sleeve line is kept, item 14 below, so no sleeve sentence is cut).
  [PRE-40, R15-M-READY / R15-M-LAUNCH, registration `a11bfb9d`: parent is accepted H's
  `theme-cov-hedge-v1-attempt2-gm.json` (SHA `1d8eecaa077844d093b05a861db18787ead3223317154c25094cfc98f029e88f`).
  The description's parent-modifier substring gains exactly `or --theme-hedge theme-cov-hedge-v1`;
  the five evidenced textual prerequisites are discharged to `[]` under the explicit PM ruling.
  Template SHA `a731e7889a2fdccb352ba4cc07aa8dd896c33821db248969533fdca666c2a30a`;
  rule registration SHA `cc1afede247bf031c5674bfa333b0953298453528974825b733e9bca43109138`;
  native manifest `scripts/specs/p9/waves/p9m-mom-volman.json` SHA
  `0bfdd92284afc5209cedc979d4ab317fced7878c3772deeda570749484adf0fc`.
  The sleeve sentence, manifest_note and historical engine-fit note remain unchanged. Fit remains
  flag-absent; the registered sleeve print is in-sample/full-TRAIN as below. Exact prior blobs,
  closure proof and adjudicated parent-read limitation: `cells/p9m-prerequisite-closure.md`.
  Precommitted scalar-only order/paths: `cells/p9m-mom-volman-read-plan.md`; immediate bounded
  launch under R15-RAM-NOW, w3584MiB/300s, floor512MiB, no optional admission/host-budget wait.]
  **Stale note (PRE-40, ledger :623):** as B.2: the registration's `manifest_note` "gross_match pm6-6" is stale and is
  not edited; the template names `pm6-6-engine` (C2-9, C2-11), checked by root before the re-pin; a manifest following
  the note literally is refused at preflight by C3b's retirement of the Python `pm6-6` route.
  **Stale registration text (round 2b-ii; D3W-STALE, ledger :676).** `scripts/specs/p9/rules/mom-volman-v1.json:50`
  says "--engine-fit is refused with --mom-volman (the C++ composition fit records no momentum sleeve; lane D3 adds it,
  Ruling ALCOMB-7 ...)"; since D3W's merge (`08f04e07`) ALCOMB-7 is in root and `--engine-fit --mom-volman` is accepted
  and writes the C++ sleeve (`task-D3W-report.md:20`, `:206-207`); the pinned registration is not edited (PRE-40 form:
  its SHA-256 `8a7222fa...` does not move for a comment; `root-wave3-merge-report.md:294`); a reader of the rule file
  alone is misled until this line.
- **Rule and constants.** The price_momentum theme's mass x target_b / sigma_b per block; sigma_b = sqrt(mean r(d)^2)
  over the 126 decisions ending j_b - 2; target_b = running mean of sigma_0..sigma_b; blocks start at 127 + 21 b;
  lookback 126, lag 2, step 21, no cap; a non-finite scale is refused (registration `constants`, `fixed_choices`).
- **Parent (PRE-8, ledger :520).** The last accepted book, also when that is a P9-H hedge book (P9-M then tests
  mom-volman on top of the hedge, not alone). It must fit `--composition ew-theme-std-v1` or `ic-shrink-v1` (+/-
  `--theme-erc`, +/- `--theme-hedge`), weight price_momentum, and carry no theme-tsmom / theme-resid
  (`atx-impl/tools/composition_mom_volman.py:41`, `:134-136`: the fitter and the exe accept a `--theme-hedge` parent).
- **gross_match** `pm6-6-engine`. **Acceptance** `pm7-34`.
- **Registered prediction** "the momentum sleeve's realised volatility falls and its Sharpe rises after scaling": kept
  only if lane D3W (ALCOMB-6 is D3W's, D3-2) makes the IC exe write the sleeve's daily series before and after scaling
  and `atx-research-eval` prints vol / Sharpe from those files (D3W-EVAL, ledger :644: a listed cross-lane append, a new
  verb or flag, flag absent = bytes unchanged); the eval verb stays this line's producer (PRE-38); no Python derivation;
  else cut from the template before registration and P9-M prints the per-block scale only (ALCOMB-6, ledger :423; cut
  if its C++ producer is not merged and APPROVE when the cells start, PRE-35). [ROUND 2b-ii: KEPT: ALCOMB-6 is merged
  and APPROVE (D3W, APPROVE `1e6c4336` / review `f0726acb`, ledger :694; merge `08f04e07`); the IC exe writes
  `<role>_mom_volman_sleeve_raw.csv` and `<role>_mom_volman_sleeve_scaled.csv` for a mom-volman-v1 run with at least
  one recorded block row in the role, and the line's producer is `atx-research-eval --vol --daily
  raw=<role>_mom_volman_sleeve_raw.csv --daily scaled=<role>_mom_volman_sleeve_scaled.csv` (each file's
  `net_vol.vol_annual`, `net_vol.sharpe_annual`); no cut, so no re-pin for a cut; PRE-38 stands (the realised vol ratio
  of B.2's form is the NAV exe's `ann_vol`) - `task-D3W-report.md:202-205`; `root-wave3-merge-report.md:299-302`].
  Disclosed (round 2b-ii; D3W-S S-6 (1), ledger :695, :699; `task-D3W-review.md:199`): the sleeve files hold only the
  recorded TRAIN sleeve (its sessions are the fit's decisions, its values 0 outside TRAIN; a role with no overlap
  writes no file), so P9-M's sleeve vol / Sharpe line is an in-sample figure. Every mom-volman-v1 IC run now publishes
  the two files beside its byte-identical existing outputs, so a directory digest or file list of such a run differs
  from a pre-D3W run's (`task-D3W-review.md:270-272`; `root-wave3-merge-report.md:290`).
- **Printed only.** sigma_b, target_b, scale_b per block (IC exe `composition_schedule_trace`, AL-COMB S-3); turnover
  per unit gross; net Sharpe at 4x NAV (producers as B.2).
- **Fit mode.** Flag-absent unless D3W (ALCOMB-7, D3-2) adds the sleeve to the C++ fit: `--engine-fit --mom-volman` is
  refused today (`composition_mom_volman.py:53-55`, `:137`; ALCOMB-7). [ROUND 2b-ii: flag-absent (the Python fit): the
  parent reference's mode, D3-Q1a DIFFERENT at the gate (B.0 "Fit mode"); the parent was not re-based under
  `--engine-fit`, so the "undefined unless" clause has no case - `gate-logs/d3q1a.txt`; `root-gate-report.md:151`;
  ledger :975, :981]. Round 2b-ii part A (lane fact, stated beside): "is refused today" is stale since D3W's merge `08f04e07`:
  D3W delivered the C++ sleeve (ALCOMB-7; `task-D3W-report.md:20`), so the "undefined unless" clause does not bind;
  the parent's mode itself is Appendix IV item 15 (part B).
- **Limit (ALCOMB-4, ledger :268).** The schedule is fixed on the fit's TRAIN sleeve: outside those sessions the
  parent's masses hold before the first recorded block and the last block's after it. It binds P9-W and OD-3 (F.1).
- **Undefined (0).** Parent outside the list; price_momentum unweighted. **Expected effect** +.01 to +.03 [est].

### B.4 P9-C: `info-clock-rate-v1` (DEC-16: distinct from Y-5)

[PRE-40, R15-C-READY/R15-C-LAUNCH, registration `0ca14564`: parent is accepted H's
`theme-cov-hedge-v1-attempt2-gm.json`, SHA `1d8eecaa077844d093b05a861db18787ead3223317154c25094cfc98f029e88f`.
Only template parent/requires and registration template pin changed; exact five strings/proof
and original blobs are in `cells/p9c-prerequisite-closure.md` at `afba2530`. New template SHA
`09c6289de5759bb9721c14bbb07e59dfeb20e21f57d76bc62132216282a597e1`, rule SHA
`57d4590fb91cf3f5149d3dfe2b2b82e82edc002f172cb81e3e270c16becdeb9b`, native v2 manifest
`scripts/specs/p9/waves/p9c-info-clock.json` SHA `a2975f0c58692f8432c5bba97f98610be6b33665ddae987266b8496ed53e5f3b`.
Description/manifest_note/source2019 FLAGGED/constants/predictions and capacity statement remain.
The C scalar read plan preserves per-scenario realised exposure max/mean and clock provenance;
fixed-rate-only capacity x1 test limitation/P10 remains. No scientific criterion added.]

- **id** `p9c-info-clock`. **Kind** `rule` naming the trade-rate NAV row; `gross_match: pm6-6-engine`, leverage
  published by the match stage (the shape E2-13 (b) requires to work, ledger :481; E2 re-review 4 ran it to record on
  `pm6-6-engine` with L-prime reaching the cell of record and its argv, real exes not probed, ledger :540). **Counts**
  N_c +1 if run.
- **Fixed now.** theta_f = `two_speed_fast_theta(cadence)` = 1 - 2^(-C/5) (W3-16; `atx-engine/include/atx/engine/book/
  two_speed.hpp:21-23`, `:45-46`), slow rate .05 (`:39`); fast while `ea_days_since` <= 21 (plan section 2.4 :601-605).
  Acceptance `pm7-34`. Printed: fast-rate share of name-days, cost per traded dollar, 4x (plan section 5.2 :906).
  OD-3 prediction "turnover share up, gross up" (plan section 5.3 :946).
- **Producer of the fast-rate share and turnover share (ALCLOCK-1, ledger :625).** The `info_clock` block (name-days,
  fast and missing name-days, fast share, planned-turnover share on fast name-days) is printed by the NAV exe only under
  the print-only opt-in `--info-clock-print` (NavSpec `options.info_clock_print`), which loads `ea_days_since` and prints
  the block at whatever rates the book runs; it is not a rule flag; with it only `summary.json` changes, by that block;
  flag absent = bytes unchanged; `info-clock-rate-v1` prints the block only when the flag is named too. P9-C and its
  reference name the flag alike (as PRE-7a), and so do the two books of the OD-3 turnover-share prediction (F.1); the
  wave-3 P9-B0 names it on the Y-F0 reference iff AL-CLOCK is merged and APPROVE then. If the opt-in cannot be
  byte-neutral, the block is cut from the reference and the prediction is printed "no producer" (ALCLOCK-1).
- **`source_sample_end` (ALCLOCK-2, ledger :626).** `info-clock-rate-v1` registers `source_sample_end` 2019 (Bowles et
  al., the published sample) and is FLAGGED under PRE-19 / PRE-31: P9-C stays registered (PRE-14 unchanged), the
  clean-source book applies no `info-clock-rate-v1` (A.4, F.1), and OD3P's `rules_pending` moves it to `rules`. The
  mapping of the paper's information-release date to `ea_days_since` is stated in the registration as a fixed choice.
- **Capacity curve (ALCLOCK-3; `status-8.md` :156-158, `r8-dispatch.md` :113; no "Ruling" line in the ledger).**
  `--capacity-curve` is accepted with `info-clock-rate-v1` (the rate reads no NAV; P9-C's parent runs the flag); the
  registration states it; the reviewer checks that the loosened refusal touches no other rate (a test shows it).
- **Probe (C3-5, ledger :566; C3-13, :636).** At C3 part A's merge root re-runs the M2h tiny-world probe with a wave naming
  one of C3's trade-rate rows, and at AL-CLOCK's merge with `info-clock-rate-v1` (P9-C's real shape), and reports which
  `--rule` token reaches the NAV argv; a duplicate or conflicting `--rule` is a stop for the PM. C3-13: P9-C's row names
  `--rate` (NAV argv token `--rate info-clock-rate-v1`), not a `--rule` row, so D3's C3-13 fix (a kind `rule` wave
  naming a row whose `x-flag` is `--rule <id>` sets `nav.rule`; other row kinds keep the append route) does not touch
  P9-C's argv; root repeats the probe with `--rate info-clock-rate-v1` at AL-CLOCK's merge.
- **Realised leverage (C3-4).** If the row's realised leverage can exceed its parameter, its registration states its
  cap on realised leverage.
- **Lane AL-CLOCK's block (round 2b-ii part A; Appendix IV item 16).** Merged and APPROVE: [ROUND 2b-ii: AL-CLOCK
  APPROVE `7ebd41ff` (review `e9b3a323`), merge `858a4fca`; fix round 1 (ALCLOCK-S) APPROVE `eed512bd` (re-review 1 of
  `f892eab1`), merge `1d003241` - ledger :744, :860; `integration-log.md:716`, `:759`].
  - **Rule and row.** [ROUND 2b-ii: `info-clock-rate-v1` (18 characters), K-P9-7 kind `trade-rate`, x-flag `--rate
    info-clock-rate-v1`, no parameter (params_schema with no property), incompatible `per-name-v1, two-speed-v1,
    aim-partial-v6, spo-v1, spo-v2, spo-v3`; at each rebalance decision d member i moves at theta_f =
    `two_speed_fast_theta(C)` = 1 - 2^(-C/5) at the rebalance cadence C (W3-16) when `ea_days_since[d, i]` lies in
    [0, 21], else at theta_s = .05 (`--trade-fraction`, required); a NaN clock is slow; row d only; one rate vector per
    decision for every book - `atx-impl/src/book/nav_rules.cpp:75`; `task-AL-CLOCK-report.md:263-268`; registration
    `scripts/specs/p9/rules/info-clock-rate-v1.json:5`, `:42-45`].
  - **Registration.** [ROUND 2b-ii: `scripts/specs/p9/rules/info-clock-rate-v1.json`, SHA-256
    `7801c19880a225a2f4c6ecb8756117f03fccafa71b92f440830bfb6fdcb8a152` (LF bytes as merged at `1d003241`, after fix
    round 1's ALCLOCK-4 sentence; `51b8b754b7e16f49d2185a5e27bf5fa1260b8890f34490cea82d0bc1ffa4e33c` at the first merge
    `858a4fca`); `source_sample_end` 2019, FLAGGED (ALCLOCK-2; `:12-14`); the ALCLOCK-2 mapping as a FIXED CHOICE
    (`fixed_choices[0]`, `:41`): the paper's information-release date = the reaction session of the name's latest
    visible primary earnings announcement as the role field `ea_days_since` records it (session date for pre-market,
    intraday and closed-day releases, the next session for post-market; NaN without a fresh announcement); no other
    release (10-K / 10-Q filing, news) enters the clock; the ALCLOCK-3 statement (`fixed_choices[5]`, `:46`):
    `--capacity-curve` is accepted with this rate (it reads no NAV), per-name-v1 stays refused with the capacity
    curve and aim-partial-v6 refuses every non-fixed rate (test `InfoClockRate.CapacityCurveAcceptsThisRateOnly`);
    `data_class` H - `sha256sum` of the working bytes = `git show HEAD:<path> | sha256sum`;
    `task-AL-CLOCK-report.md:269-276`, `:462`; `root-wave3-merge-report.md:1028`, `:1034-1042`]. Re-pinned with the
    template at the registration commit (CELLS step 1): [ROUND 2b-ii: filled at the cell's registration commit by root
    - Appendix IV item 16].
  - **Template.** [ROUND 2b-ii: `scripts/specs/p9/templates/info-clock.json`, SHA-256
    `95d4104a421c32f91877649f1ec9dfbdb409c9a121d6f6e2377e5e25e19b42d5` (= the registration's
    `manifest_fragment.rule_cell.template_sha256`, `:70`; unchanged by fix round 1); name `p9-info-clock`; `parent`
    null, nominal parent `../../v8/lib-v8ysb-gm.json` (Y-F0); `change.set` only `nav.output`
    (`build-equity/mega-nav-p9-info-clock`) and `monitor.output`; no `rules`, no `change.flags` (ALCOMB-1); root's wave
    manifest: kind `rule`, `rules: [{name: info-clock-rate-v1, params: {}}]`, `gross_match: pm6-6-engine`, wave id
    `p9c-info-clock` - `sha256sum`; `task-AL-CLOCK-report.md:277-282`; registration `:64-82`]; the re-pinned template
    with `parent` set: [ROUND 2b-ii: filled at the cell's registration commit by root - Appendix IV item 16].
  - **NAV argv token (C3-5 / C3-13).** [ROUND 2b-ii: the row puts `--rate info-clock-rate-v1` on the NAV argv on the
    append route (not a `--rule` row): the cell's argv carries the parent's `--rule aim-partial-v5` once (from
    `nav.rule`) and `--rate info-clock-rate-v1` once; a parent `--rate` value is replaced, never duplicated;
    `--info-clock-print` on the parent is kept once; `--rate` is rule-owned (a `rule_cell.constants.flags` or a
    template `change.flags` setting it is refused at preflight, exit 3) - `task-AL-CLOCK-report.md:283-289`]. The probe
    at its merge (root M4, real exes): [ROUND 2b-ii: holds: wave `clk3`, `--until spec`, preflight .. spec done, exit
    0; the cell sets no `nav.rule`; its planned NAV argv carries `--rule aim-partial-v5` ONCE, `--rate
    info-clock-rate-v1` ONCE and `--trade-fraction .05` once; no NAV run of the cell (the tiny world has no
    `ea_days_since`); no duplicate or conflicting `--rule` - `root-wave3-merge-report.md:538`;
    `m4-logs/c3-5-probe.txt`, `m4-logs/probe-wave-clk3.log`].
  - **Parent conditions.** [ROUND 2b-ii: the parent NAV runs `--rule aim-partial-v5 --trade-fraction .05` at the
    fixed rate, without `--two-speed`, aim-partial-v6 or an spo rule (each refused with the rule); its `--fields`
    manifest lists `ea_days_since` point in time (refused otherwise, before any output); a construction grid keeps
    the base cadence; `--capacity-curve` is accepted (ALCLOCK-3); Y-F0 (`lib-v8ysb-gm.json`) meets these at
    registration (cadence 1, .05, `--capacity-curve`, `--warm-start-sessions 60`, `--locate-in-aim`, delta orders) -
    `task-AL-CLOCK-report.md:290-295`; registration `:45`, `:47-48`, `:88-89`].
  - **The flag pair, cell and reference (ALCLOCK-1, PRE-7a).** [ROUND 2b-ii: `--info-clock-print` on BOTH: root names
    it on the Y-F0 reference at the wave-3 P9-B0 and the cell inherits it from its parent's NAV flags [the route exists
    since `f8d852f3` (FLAGS-ROUTE); before it the parent's `nav.flags` carried neither flag (GATE-REV RQ-1)] (it is no
    rule's flag, so the E2-8 guard passes it); `--ann-mean-gross` on both; the rule flag `--rate info-clock-rate-v1` on the
    cell only - `task-AL-CLOCK-report.md:296-298`; registration `:53`]. That the P9-B0 reference names it: part B
    (the gate's P9-B0 argv). [ROUND 2b-ii: ALCLOCK-1 HELD at the gate's step 3: `--info-clock-print` on Y-F0's argv
    (Release NAV p9-3f, provisional, re-pin pending) gives the same 27 files and changes only `summary.json`, by 70
    leaves all under `/scenarios/<i>/info_clock` (5 of 5 scenarios), opt-in side only; without those blocks it is the
    flag-absent document; so the block is not cut, no "no producer" line prints, and the P9-B0 Y-F0 NAV names
    `--info-clock-print` (substitution list row 6, `778d34b8`; that run is behind GATE-IDX: its argv of record is part
    B2b's) - `gate-logs/nav-identity-yf0.txt`, `tools/clock_optin_cmp.py`; `root-gate-report.md:102`, `:481`, `:625`;
    ledger :981 (LAPSE-TABLE: P9-C RUNS)]. Part B2b: the P9-B0 Y-F0 NAV of record `build-equity/p9-b0g-yf0-nav` ran with
    `--ann-mean-gross --info-clock-print` (its receipt; YF0-NAV-ORDER) and holds against M1d's `p9-b0-yf0-nav`: 26 / 27
    files byte-identical, `summary.json` PRODUCER 2 + OPTIN 75 leaves on the new side only (`/scenarios/0..4/
    ann_mean_gross` 5, `/scenarios/0..4/info_clock/*` 70), 0 outside: ALCLOCK-1 holds at the NAV of record -
    `gate-logs/yf0nav-cmp.txt`, `yf0nav-classes.txt`; `root-gate-report.md:886-900` (at `d22246f6`).
  - **Producers of each printed figure** (all the NAV exe `atx-equity-strategy-targets`). [ROUND 2b-ii: fast-rate share
    of name-days: `summary.json` `scenarios[k].info_clock.fast_share` (and `fast_name_days`, `missing_name_days`,
    `name_days`) under `--info-clock-print` (`atx-impl/src/book/nav_write.cpp:289` `info_clock_json`; census
    `nav_decide.cpp:252`); turnover share up (OD-3): `info_clock.turnover_share` = fast planned turnover / planned
    turnover, on the cell and on its parent (same producer, same flag); gross up (OD-3), the producer of the gross
    prediction: realised exposure `scenarios[k].exposure.mean_gross_leverage` (existing) and the daily
    `gross_leverage`, gross return `scenarios[k].ann_mean_gross` under `--ann-mean-gross` (C3-9); on TRAIN the cell is
    gross-matched (pm6-6-engine), so its mean gross exposure is the parent's by construction there; cost per traded
    dollar and 4x: the capacity curve under `--capacity-curve` (`strategy_nav_v7.cpp` `capacity_row`:
    `cost_bps_per_traded_dollar`, `net_sharpe` at multiple 4; ALCLOCK-3 makes it run with the rule) -
    `task-AL-CLOCK-report.md:299-309`].
  - **C3-4 realised leverage (ALCLOCK-4, ledger :702).** [ROUND 2b-ii: the stated bound, verbatim: "planned gross <=
    (theta_f / theta_s) x gross(aim) by contraction outside the dust band, 2.589x at cadence 1; P9-C's mean gross
    matched to its parent by pm6-6-engine"; the realised clause, verbatim: "realised gross leverage is read from the
    cell's own summary (max and mean over the cell) and printed beside the parent's" (`summary.json`
    `scenarios[k].exposure.max_gross_leverage` and `mean_gross_leverage`, the NAV exe's realised gross series; the
    daily realised gross is the daily CSV's `gross_leverage`); the OD5-7 cap test applies to that realised figure, not
    to the planned bound - registration `:51`; `task-AL-CLOCK-report.md:310-322`; `root-wave3-merge-report.md:1028`].
    P9-C's print therefore shows the cell's realised max and mean gross leverage beside the parent's.
  - **Restatement against Y-5 (DEC-16).** [ROUND 2b-ii: Y-5 (two-speed-v1) buckets THEMES into a fast and a slow
    sleeve (own aims and holdings, netted into one trade); this rule keeps one aim and one book and sets a rate per
    NAME-DAY from an event clock (the name's earnings announcement), whatever theme its alpha comes from: another
    decision unit and another object (a rate vector, no sleeve, no netting); R-3 is a per-signal aim gain at one rate;
    cost if wrong: one restated trial (N_c +1) - registration `:52`; `task-AL-CLOCK-report.md:323-326`].
  - **Constants.** [ROUND 2b-ii: no new constant: theta_f = Y-5's function, theta_s = Y-5's slow rate .05, window 21
    (Bowles et al.'s month; 0 and 21 are fast), field `ea_days_since` (`sec-ea-days-since-reaction-v1`) - registration
    `:18-39`; `task-AL-CLOCK-report.md:327-328`].
  - **Disclosed (round 2b-ii; ALCLOCK-S2 S-A, ledger :861-862).** The capacity curve's "x1 is S2 bit for bit"
    sentence (`capacity_x1_equals_primary_bit_for_bit`) is tested on fixed-rate runs only; no test shows it with the
    rule ON; the rule-on test is P10. P9-C's acceptance (dSR > 0 AND mechanics) does not read that sentence; the claim
    stays untested for the rule-on path in P9, disclosed before any read.
  - **Fit mode.** NAV-only cell: the fit is the parent's (`task-AL-CLOCK-report.md:294-295`); [ROUND 2b-ii: flag-absent
    (the Python fit): the parent reference's mode, D3-Q1a DIFFERENT at the gate (B.0 "Fit mode") - `gate-logs/d3q1a.txt`;
    `root-gate-report.md:151`; ledger :975, :981].
- **Lapsed (0; PRE-14, ledger :524)** if AL-CLOCK is not merged in root and APPROVE when the P9 cells start; the order
  of the remaining cells is unchanged. W3-EARLY-2 (ledger :584) lets the lane start on approved heads; it does not
  change this condition.

### B.5 P9-MV: `mv-aim-v1` (COV-7, ledger :199; funded)

- **id** `p9mv-mv-aim`. **Kind** `rule` naming the NAV `target` row `mv-aim-v1`; `gross_match: pm6-6-engine` (plan
  section 5.2 :910); acceptance `pm7-34`. **Counts** N_c +1 if run. The row names `--target-rule`, not a `--rule` row,
  so D3's C3-13 fix does not touch P9-MV's argv (C3-13, ledger :636).
- **Store (W3-10, ledger :388; PRE-29, :569; ledger :582).** `build-equity/p9-m2i-dcov-covv1`, the Release-built
  `atx-cov-v1` container store D-COV wrote (Release `atx-equity-strategy-risk` p9-2o
  `ccc73cd10ddc0c4454de50a51f2624e8f38f440e1ea5b47b984cbab06444d811`, source `c8ddf736`; role `train-2020-2023-lo3`
  `e1c67101...95f4`; fields `train-2020-2023-lo3-fields-v10` `a4a060ae...0809`; `--emit-exposures all --emit-container
  --bias-families extended --recipe atx-cov-v1`): `manifest.json`
  `ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3`, container root
  `20919f7b338dfbd786994cbbcb506bdaabd9c3af3266d4394d4d2e07d90c9f8e`, `model.atxcov`
  `0f90bf7c78203ca8c93d6003d58c8b2894747045485137756fbc13a32e3b642a` (510,678,016 bytes) (`handoff-root-m2i.md` "Store
  pins"). Kept, never rebuilt in P9 (a rebuild is a new store, COV-5; CRT-2). S9 / COV-9: root re-checks
  `universe.delisting.returns_applied == false` on the role before the run (ledger :354, :401; held before every M2i
  COV run).
- **Store pin route (PRE-9, ledger :521).** The `mv-aim-v1` row lists `risk_model` and `risk_model_sha256` as params with
  their `x-flag` (as `strategy_risk_target.cpp:341-346`); the cell names them in `params`: `rules: [{name: mv-aim-v1,
  params: {risk_model: build-equity/p9-m2i-dcov-covv1, risk_model_sha256: ea58691f...f0f3}}]`. E2's guard refuses those
  flags by every other route.
- **Gross rule** as `brief-COV-MV.md:96-99` (W3-17): the priced block rescaled to the desired target's gross over the
  priced names; unpriced names keep their weights.
- **Required carries for COV-MV (rulings).** COV-13 (c), ledger :587: `mv-aim-v1` never inverts the factor covariance F
  by itself; V = X F X' + D is applied through a form defined for a singular F (D > 0), and a synthetic test with an
  exactly rank-deficient F shows the target finite and equal to the dense solve. PRE-24a, :557: the `mv_aim` summary
  reports per run the mean count of priced names and the ratio of the gross-weighted mean specific volatility of the MV
  target to that of the desired target. COV-13 (c): it also prints the share of the MV target's forecast factor variance
  on eigen ranks 00-02. COV-11, :402: COV-MV checks per-row group values itself (S11) and carries S10. COV-14 (ledger
  :621) leaves COV-13 (c) binding COV-MV unchanged.
- **Prior of record (PRE-24a), printed beside P9-MV.** "spo-v1: rejected, implementation defect (gross .65, mechanics
  fail), no research content. spo-v2: rejected as a research result (paired dSR -0.794, Memmel SE .476; cost per traded
  dollar +27%; held names 1,514 vs 1,930; lesson G2: with a binding gross budget the risk term does not discipline the
  book, the vol-scaled alpha a = IC sigma z tilts it to high specific-vol, lower-ADV names)" (`platform-20260928/
  progress.md` :84-105, :260-275, as the PM read them). P9-MV is ruled distinct from spo-v2 and not a re-cast under E-45:
  `mv-aim-v1` is a closed-form `target` step (gross-normalised P V^-1 P alpha, whose diagonal part divides by specific
  variance, so the tilt is toward low specific-vol names), with no optimiser, no cost term and no gross cap inside the
  rule; trading stays aim-partial; the store is the eigen-adjusted `atx-cov-v1` (PRE-24a).
- **Predictions (plan section 5.2 :910):** vol ratio < 1 at gross-alpha ratio about 1 (band [.90, 1.10], PRE-25;
  producers as B.2: the vol ratio from the NAV exe's `ann_vol`, PRE-38). Printed only: turnover per unit gross, capacity
  curve, 4x, the rule's `mv_aim` summary block (`brief-COV-MV.md:107-108`) with the priced-name count, the specific-vol
  ratio (PRE-24a) and the eigen 00-02 factor-variance share (COV-13 (c)), and cost per traded dollar vs the parent
  (PRE-24a).
- **OD-3 consequence (PRE-10, ledger :522).** No per-era risk store is built in P9. If P9-MV is accepted, P9-F0 reads
  `--risk-model`: the adoption print shows P9-F0 and P9-F0h; G-B8 is decided on P9-F0h and labelled "history-readable
  book, not the deployable one".
- **Lane COV-MV's block (round 2b-ii part A; Appendix IV item 17).** Merged in root and APPROVE: [ROUND 2b-ii: yes:
  COV-MV APPROVE `25e11b32` (re-review 1 of fix round 1 `942f499b`; first review `e9e794b3` of `846739c1`), merge
  `5c811b56` (one conflict, the `atx-impl/tests/CMakeLists.txt` tail, kept both, AL-CLOCK's block first) - ledger :813;
  `integration-log.md:717`; `root-wave3-merge-report.md:560-567`], so P9-MV does not lapse under COV-7 / PRE-14 (B.10;
  COMPOSE-1 is the gate's, part B). [Ruled (COMPOSE-PRE, ledger :964; COMPOSE-1, :832, as COMPOSE-1a, :956, refines
  it): P9-MV's parent is the last accepted book; if that parent carries `info-clock-rate-v1` (P9-C accepted), the first
  run of both rules on one argv is P9-MV's own cell (no composition test in P9); the check reads "accept at the gate's
  store-backed parse check (`--rate info-clock-rate-v1 --target-rule mv-aim-v1 --risk-model <a tiny-world store>` to
  the parse / rule-table stage, no NAV run), or (if no tiny-world store can be built) at P9-MV's own preflight; a
  refusal at either lapses P9-MV at 0 before any read", the refusal line the finding, never a fix round after a read;
  which of the two the gate delivered: part B2] [ROUND 2b-ii: the gate delivered the STORE-BACKED parse check:
  COMPOSE-1a ACCEPT: a tiny-world store built by the Release risk exe (p9-3f `0c110534...586b`, provisional, re-pin
  pending; `--recipe atx-cov-v1`, exit 0, manifest `5a4b7b9b...1555`), then the Release NAV exe (p9-3f `255b58ac...3565`)
  with `--rate info-clock-rate-v1 --risk-model <that store> --risk-model-sha256 <its pin> --target-rule mv-aim-v1`
  passes the v7 parse, the store open (role pin checked), the base parse and the run checks and stops at the fields load
  ("fields lack ea_days_since": the tiny world has no clock field), the same stop as the rate alone, i.e. a data
  refusal, not a flag conflict; controls: `mv-aim-v1` alone passes the fields load, two `--risk-model` are refused at
  the v7 parse; no output dir created; so P9-MV does NOT lapse under COMPOSE-1 at the gate; stated limit:
  `validate_nav_config` / `validate_nav_input` (after the fields and blend loads, `book/nav_lockstep.cpp:238-250`) for
  the two rules together are P9-MV's own preflight, where a refusal lapses P9-MV at 0 before any read (COMPOSE-1,
  COMPOSE-PRE) - `gate-logs/compose-1a.txt`, `tools/compose_1a.py`; `root-gate-report.md:101`, `:529-553`; ledger :974,
  :981 (LAPSE-TABLE: P9-MV RUNS)].
  - **Rule and row.** [ROUND 2b-ii: `mv-aim-v1` (9 characters), kind `target`, x-flag `--target-rule mv-aim-v1`;
    params `risk_model` (`--risk-model`) and `risk_model_sha256` (`--risk-model-sha256`, pattern `^[0-9a-f]{64}$`),
    both required, closed schema (PRE-9); incompatible `aim-partial-v6, spo-v1, spo-v2, spo-v3, two-speed-v1,
    adv-hold-v1, hold-band-v1` (fix round 1 S-5 renamed `two-speed-nav-v1` to `two-speed-v1`); the NAV exe also
    refuses a `--rule` other than `aim-partial-v5`, and `--construction-grid`; `nav --list-rules --json` of the merged
    head: 12 -> 13 rows, only `mv-aim-v1` added, the last row and the only `target` row - `task-COV-MV-report.md:207-212`,
    `:411`; `root-wave3-merge-report.md:603`].
  - **Registration.** [ROUND 2b-ii: `scripts/specs/p9/rules/mv-aim-v1.json`, SHA-256
    `4b4af6e238b529d5d7eabccd2699022dc653da52fa45801eb3f61896bd6680b4` (the bytes as merged at `5c811b56`; fix round 1
    left it unchanged); `data_class` H; `predicted_mechanism` "V^{-1} tilt lowers ex-ante and realised vol at equal
    gross alpha" - `sha256sum` (= `git show HEAD:<path> | sha256sum`); `task-COV-MV-report.md:213-214`, `:232-234`;
    `root-wave3-merge-report.md:619`, `:625-626`; registration `:14-15`]. Re-pinned with the template at the
    registration commit: [ROUND 2b-ii: filled at the cell's registration commit by root - Appendix IV item 17].
  - **Template.** [ROUND 2b-ii: `scripts/specs/p9/templates/mv-aim.json`, SHA-256
    `83391f2c526b28008d48e250f0e7d26e7c3f8661786d43a8f3d76439f664d870` (LF bytes; = the registration's
    `manifest_fragment.rule_cell.template_sha256`, `:59`); name `p9-mv-aim`; NAV-only: `change.set` only `nav.output`
    (`build-equity/mega-nav-p9-mv-aim`) and `monitor.output`; `parent` null, nominal parent `../../v8/lib-v8ysb-gm.json`
    (Y-F0), root sets it - `sha256sum`; `task-COV-MV-report.md:215-218`; template `:2-19`]; the re-pinned template with
    `parent` set: [ROUND 2b-ii: filled at the cell's registration commit by root - Appendix IV item 17].
    [PRE-40, R15-MV-READY registration `0afd08b4`: template
    `d4dc2ff7916fdda924590da597e7c8325358cc369a6c567f2c1b9d7df145e5c6`, registration
    `d88e393a49c8efc9bf08af019eaf427af106092feec23a9d47151ce34cdd8712`; original blobs
    `63917df794f55696830021c330e0e5f93b9f12a2` / `31bcf217be335e768658e3e490155f2322e207f6`
    preserved. Exact delta: template parent null -> accepted H
    `theme-cov-hedge-v1-attempt2-gm.json`, five proved template requires -> [] under
    the explicit extension to COVMV-PIN-2, and rule template_sha256 re-pin only.
    All scientific text, historical rule requires, constants and source2010 stay.
    Native `scripts/specs/p9/waves/p9mv-mv-aim.json` SHA
    `95ab9777cbfe0fb099f0008120cd6104606f7d589f286f2c31ae7bc912b070b0`, H/libraryv8ysb,
    v15/covv1 pins, n_before66/cap71, pm7-34/pm6-6-engine, no admissions; completed
    u/fit/card/w reused. Both print flags inherited once, target and risk flags
    derived once from the registered row; H fixed rate, no rejected C rate.
    Source/pin/void/mechanics then bound existing rho then exact registered scalar
    leaves: cells/p9mv-mv-aim-read-plan.md and cells/tools/p9mv_read_filter.py. Eigen null
    encoding is disclosed, never imputed. R15-MV-LAUNCH releases one immediate
    bounded native run after these committed checks; no retry or next-cell work.]
  - **Manifest fragment and the store pin (PRE-9).** [ROUND 2b-ii: `gross_match: "pm6-6-engine"`; `rules: [{name:
    mv-aim-v1, params: {risk_model: build-equity/p9-m2i-dcov-covv1, risk_model_sha256:
    ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3}}]`; E2's `rule_flags` derives exactly
    `--risk-model`, `--risk-model-sha256` and `--target-rule mv-aim-v1` from the built exe's row; E2-8 refuses the three
    by any other route; the store is named by its pin only (the lane never opened it; W3-10, PRE-29); root M4's
    `validate_specs.py` against the merged table: template and registration SHAs as pinned, no `rule_flags` problem -
    `task-COV-MV-report.md:219-225`; registration `:49-73`; `root-wave3-merge-report.md:604`, `:619`;
    `m4-logs/covmv-validate-specs-2y.log`].
  - **`source_sample_end`.** [ROUND 2b-ii: 2010, verified: primary source Menchero, Wang and Orr (2011),
    "Eigen-Adjusted Covariance Matrices" (cov-design section 8 source [2]), read 2026-10-06 in the paper's text: "we
    select the 50 largest US stocks as of July 30, 2010 that have full daily return histories dating back to July 1,
    1993" and "the testing window is 4,103 trading days (from April 15, 1994 to July 30, 2010)"; before 2015, so NOT
    flagged under PRE-19 (reviewer-checked) - registration `:12-13`; `task-COV-MV-report.md:226-231`;
    `root-wave3-merge-report.md:628-629`].
  - **C3-4 realised leverage.** [ROUND 2b-ii: the row cannot exceed a parameter of its own: the rule has no leverage
    parameter and holds the target's gross at the desired target's (W3-17), so the book's realised gross leverage is
    aim-partial-v5's at the parent's L, or the L' of the PM6-6 match; under a leverage rule the cap is that rule's;
    P9-MV names none - registration `:36`; `task-COV-MV-report.md:235-237`].
  - **The carries (evidence).** [ROUND 2b-ii: PRE-9: the closed params schema and the derived flags above. COV-13 (c):
    F is never inverted; V^-1 is applied as u - D^-1 X y with (I + F G) y = F X' u, defined for a singular F (gtest
    `MvTarget.RankDeficientFEqualsDenseSolve`); the `mv_aim` summary prints `eigen_00_02_share {mv, desired}` (the
    share of e' F_d e on F_d's three smallest eigenvalues over the forecast block, e = X' w). PRE-24a: the `mv_aim`
    summary prints `priced_names` / `unpriced_members` {n, mean, min, max} and `specific_vol_ratio {pooled,
    per_decision}` (sum |w| sigma / sum |desired| sigma over the priced names); cost per traded dollar and turnover from
    the existing NAV summary and capacity curve. Producer of every figure: `atx-equity-strategy-targets nav`,
    `summary.json` key `mv_aim`, main pass (`strategy_mv_aim.cpp`); `recipe.json`, the holdings manifest and
    `capacity/summary.json` carry `{rule, declaration, risk_model}` only. Tests at root's merge: `MvTarget.*` 12 / 12,
    `MvAimRule.*` 16 / 16, the lane's named filter 100 / 100 (Debug p9-2y) - registration `:29`, `:37-44`;
    `task-COV-MV-report.md:238-250`; `root-wave3-merge-report.md:599-601`].
  - **Disclosed (round 2b-ii; COVMV-S2 S-b, ledger :814-815).** [ROUND 2b-ii: the eigen share may print null: on any
    print failure (an F_d forecast block not symmetric within 1e-9) the `mv_aim` block carries `"eigen_00_02_share":
    null` plus `"eigen_00_02_share_unavailable": {"decisions", "first_error"}` (the key exists only on a failure); the
    book's targets and every other file are unchanged by a failure (`MvAimRule.PrintFailureLeavesTheBookAndWritesNull`)
    - `task-COV-MV-report.md:407`; `atx-impl/src/strategy_mv_aim.cpp:315-329` as the report cites them]. The
    registration's `printed_only` (`:37-44`) does not say so. COVMV-S2 and NAV-S S-2 (ledger :890; the
    `predicted_mechanism` clause "ex-ante ... vol" that nothing prints, registration `:14`, `:62`): root edits the
    `printed_only` / "ex-ante" sentences in the re-pin commit iff `mv-aim-v1.json` is not yet pinned by a ledger line,
    golden or prereg SHA at the re-pin; else they stay as they are (PRE-40 form) and this line states them. The SHA-256
    above is a record of the merged bytes from the lane's block, not a pin of this file (stated for the PM: whether it
    counts as a "prereg SHA" under COVMV-S2 is the PM's to rule) [Ruled (COVMV-PIN, ledger :948): YES, a prereg pin;
    the `printed_only` / "ex-ante" sentences of `mv-aim-v1.json` (COVMV-S2 S-b; NAV-S S-2 the same) are NOT edited at
    the re-pin; they stand stated here (PRE-40 form), the file's sentences untouched] [Ruled (COVMV-PIN-2, ledger
    :970): COVMV-PIN stands: the registration commit's allowed diff is the template's `/parent` plus the registration's
    `template_sha256` (no stated cut for P9-MV), shown by JSON diff (CELLS step 1); the `printed_only` / "ex-ante"
    sentences are not in that diff; this block records the registration's SHA-256 before (`4b4af6e2`, above) and after
    (Appendix IV item 17, filled at that commit)]. A null eigen share is disclosed in the cell's print before the read
    (COVMV-S2).
  - **Fit mode.** NAV-only cell (the template sets the NAV and monitor outputs only): the fit is the parent's;
    [ROUND 2b-ii: flag-absent (the Python fit): the parent reference's mode, D3-Q1a DIFFERENT at the gate (B.0 "Fit
    mode") - `gate-logs/d3q1a.txt`; `root-gate-report.md:151`; ledger :975, :981].
- **Lapsed (0)** if COV-MV is not merged in root and APPROVE when the P9 cells start (COV-7; PRE-14); W3-EARLY-2 does
  not change this condition (as B.4).

### B.6 P9-W: walk-forward refit (K-P9-8 = `atx.dsl-composition-weights/v3`, P19)

- **id** `p9w-walk-forward`. **Kind** `walk-forward` (schema only until D3: pool-17 `wave_kinds.py@ea7d9dde:28-29`,
  `:63`, `:255-262`; `lag >= 2`). **Counts** N_c +1 if run.
- **Rule.** The accepted book's fitted weights refit walk-forward: expanding, lag 2, step 21 (plan section 5.2 :907).
  Registered constants of walk-forward-v1 (D3-1, ledger :628): window expanding, lag 2, step 21, `min_decisions` 21
  (one step; the smallest window the step itself implies). Fitter flags (D3-2, :629): `--walk-forward expanding
  --walk-forward-lag N --walk-forward-step N [--walk-forward-min-decisions N]`; walk-forward runs through the C++ fit and
  a post-step in `main` / `composition_engine_fit.py`, with no edit inside `fit` / `fit_prior` or their fingerprint
  closure; flag absent = every byte unchanged (D3-1). Before the first row no fit exists, so the book is flat; P9-W's
  printed dSR includes that flat start (biased against the walk-forward book by about one flat month in 48); no second,
  windowed statistic is added (D3-1).
- **Parent / reference (PRE-39, ledger :604).** P9-F0h (A.4): the last accepted book that does not read the risk model,
  which is the book OD-3 reads; it equals P9-F0 unless P9-MV is accepted, and P9-W keeps its place in the order (A.7).
  If P9-MV is accepted, nothing in P9 measures the walk-forward loss of the mv-aim book; that book gets no history read
  either (PRE-10). P9-W decides nothing; never accepted, never a parent.
- **gross_match** `pm6-6-engine` (PRE-23, ledger :528; subject to the prereg review).
- **Printed.** Paired dSR vs the in-sample parent P9-F0h (pinned verb). Limit: if mom-volman is in the book its schedule
  stays TRAIN-fixed (ALCOMB-4); theme-cov-hedge has no such limit (`task-AL-COMB-report.md:323-327`).
- **OD-3 prediction it feeds** (PRE-23 as PRE-39 reads it; formula fixed now; number filled mechanically after P9-W):
  predicted history S2 net Sharpe of P9-F0h = its TRAIN S2 net Sharpe + P9-W's dSR (walk-forward minus in-sample).
  (Round 2b-ii part B2a: P9-W LAPSED (0) at the gate (D3-Q1a DIFFERENT; below; ledger :975), so no P9-W dSR exists: this
  registered prediction, its F.1 line and E's "P9-W's dSR" printed-beside line have no value to fill; how they print
  ("not produced: P9-W lapsed (0)" or cut) is not ruled; stated for the PM.) [Ruled (PW-LAPSE-PRINT, ledger :994):
  F.1's registered prediction "history S2 net Sharpe of P9-F0h = its TRAIN S2 net Sharpe + P9-W's dSR" prints "not
  produced: P9-W lapsed (0), D3-Q1a" with no number (the formula stands as registered; its input does not exist); E's
  "P9-W's dSR" line prints "P9-W lapsed (0), D3-Q1a: no dSR"; nothing else of F.1 changes]
- **Lanes D3 and D3W's blocks (round 2b-ii part A; Appendix IV item 18).** Merged and APPROVE: [ROUND 2b-ii: D3
  APPROVE `699440bb` (review `e197b3b6`), merge `0a2ceb2b`; D3W APPROVE `1e6c4336` (review `f0726acb`), merge
  `08f04e07` - ledger :753, :694; `integration-log.md:714`, `:699`].
  - **Manifest shape (D3, the kind wiring).** [ROUND 2b-ii: `{"schema": "atx.research-wave/v2", "wave":
    "p9w-walk-forward", "kind": "walk-forward", "parent": {"spec": <P9-F0h's cell of record>, "library": <its
    library>}, "fields", "acceptance", "gross_match": "pm6-6-engine", "budget", "ledger", "expect", "out_dir", "exes"
    (B2-10), "walk_forward": {"window": "expanding", "lag": 2, "step": 21, "min_decisions": 21}}`; no `rule_cell`, no
    `rules`, no `candidates`; all four constants named (D3-1), 21 explicit (D3-Q3, ledger :728); the kind is out of
    SCHEMA_ONLY (`scripts/wave_kinds.py:82`, `SCHEMA_ONLY = ()`) and owns the four fitter flags (`:86`
    `WALK_FORWARD_FLAGS`; E2-8) - `task-D3-report.md:247-251`; `root-wave3-merge-report.md:453-455`].
  - **Fit mode (D3, D3W).** [ROUND 2b-ii: the cell's fit = the parent's fit flags plus `--walk-forward expanding
    --walk-forward-lag 2 --walk-forward-step 21 --walk-forward-min-decisions 21`; the parent must fit under
    `--engine-fit` (ALCOMB-7) and without `--theme-tsmom`, `--theme-resid` or `--era` (`WALK_FORWARD_REFUSED`,
    `scripts/wave_kinds.py:90`); preflight refuses otherwise; at root's D3 merge the kind's four flag names,
    `expanding`, `--engine-fit`, the refused set, the bounds (lag >= 2, step >= 1, min_decisions >= 2) and the default
    21 match the fitter (`m4-logs/d3-wf-flags.txt`) - `task-D3-report.md:252-254`; `task-D3W-report.md:192-194`;
    `root-wave3-merge-report.md:447`, `:455-457`]. D3W's block writes the fitter flags without
    `--walk-forward-min-decisions` ("min_decisions defaults to the registered 21", `task-D3W-report.md:192-193`)
    (differs from D3-Q3, which has the manifest name 21 explicitly; stated for the PM) [Ruled (D3Q3-ARGV, ledger
    :960): no conflict: P9-W's manifest names `walk_forward.min_decisions` 21 explicitly (D3's block; D3-Q3), so
    `scripts/wave_kinds.py:311-319` `walk_forward_flags` emits `--walk-forward-min-decisions 21` and the output token
    is `wf2s21m21` (`:322-324`; the generated cell name below carries it); D3W's sentence describes the fitter's
    default when the flag is absent, not P9-W's argv]. The engine-fit parent rule,
    stated before the gate (D3-Q1, ledger :726, as D3-Q1a, :898, reads it): at the gate's P9-B0 root compares each
    `--engine-fit` weights file of the current Y-F0 TRAIN fit with the Python fit's, `provenance.engine_fit` removed
    from both (canonical JSON dump, SHA-256 of the stripped documents), and the two admission files by whole-file
    SHA-256; equal = identity, and the re-based Y-F0 (P9-F0h when no cell is accepted) is produced with `--engine-fit`,
    an engine-fit parent; any other difference = P9-F0h is produced as before (Python fit), P9-W lapses at 0 (PRE-35)
    and the differing paths are a finding in the freeze report (no re-tuning, no second attempt). Outcome:
    [ROUND 2b-ii: DIFFERENT (run at the gate's step 3, before P9-B0; Release exes p9-3f, provisional, re-pin pending:
    `factors` -> `p9-gate-yf0-factors`, then the fit with `--engine-fit --engine-factors <it> --admission-exe
    f53d9277... --composition-exe 04db0d67...` -> `p9-gate-yf0-fit-eng`, against the Python fit `p9-gate-yf0-fit-py`):
    `admission.json` (`2d5abfd7...`) and `admission.csv` (`c3933116...`) byte-identical; `composition_weights.json`
    with `$.provenance.engine_fit` removed (canonical JSON SHA-256): Python `f2454589...b876` vs engine
    `decc2f8c...55a8`, 694 float leaves differ (0 non-float, no key on one side only): `/weights/*` (47), the
    candidates' weights, the themes' and theme-ERC provenance (covariance, ERC shares and risk contributions, sleeve
    vols and correlations, weights before cap), the theme-standardise ERC covariance, the in-sample blend diagnostic
    and the weighted standalone turnover leaves; `provenance.engine_fit.status` "agree", `fit_max_rel_float_diff`
    non-zero (within P12; its value not read). So P9-F0h is produced flag-absent (the Python fit) and P9-W LAPSES at 0
    trials (PRE-35; "Cell P9-W: LAPSED (0)"); N_c unchanged; the order of the remaining cells unchanged (A.7). The
    FINDING for the freeze report: on the real Y-F0 TRAIN fit the C++ theme-ERC fit agrees with numpy within ruling P12
    but not bit for bit (BLAS `np.dot` vs the C++ sums, the risk `wave-review-ic.md` section 4 stated before the run)
    and `--engine-fit` writes the C++ values, so every weight moves in its last bits; no re-tuning, no second attempt
    (D3-Q1) - `gate-logs/d3q1a.txt`, `tools/d3q1a.py`; `root-gate-report.md:100`, `:483-489`; ledger :974, :975, :981].
  - **Per-date weights producer (D3W).** [ROUND 2b-ii: the C++ `atx-research-composition fit --factors D --spec S
    --output O --walk-forward expanding --lag 2 --step 21 --min-decisions 21` (one call), then the fitter's
    `run_post_step` writes `atx.dsl-composition-weights/v3` (the full-TRAIN weights, signs and blocks unchanged);
    `composition_fit.json` gains `walk_forward {rule: "walk-forward-v1", window, lag, step, min_decisions, rows:
    [{decision_index, from_session, fit_decisions, last_fit_index, last_fit_session, weights}]}`; the IC runner holds
    the book flat before the first row (P9-W's dSR includes that stretch, D3-1); the v3 runner's single-row test
    `WeightsV3.SingleRowEqualsV1Bytes` (`atx-impl/tests/strategy_ic_runner_test.cpp:4910`) - `task-D3W-report.md:55`,
    `:195-201`].
  - **Template.** [ROUND 2b-ii: none is committed, by design: the spec stage writes the cell `<parent spec
    dir>/<parent stem>-wf2s21m21-p9w-walk-forward.json` = parent + `change.set` (fit, card, w, NAV and monitor outputs
    renamed `-wf2s21m21`) + `change.flags.fit` (the four flags), generated by `wave_kinds.walk_forward_template` /
    `walk_forward_set` / `walk_forward_flags` (`scripts/wave_kinds.py:361`, `:331`, `:311`); pinned instead: the
    generator blobs `scripts/wave_kinds.py` `d282586a` (SHA-256 of the blob bytes
    `9268e222b4f11420d7307e99eb013e311708153afb543b7fba86827674f73ee8`) and `scripts/wave_stage_kinds.py` `694b619e`
    (`800b40801da4443fb282acf6a69201cbdc779555061c751cb31ccfc7672ca82e`), the reviewed blobs and still root HEAD's, and
    the kind fixture `scripts/specs/p9/kinds/walk-forward.json`
    `d865201a1f7a909c62f471f1c9ad58da87228c3c23e5fd28e45df0921b098d9f`; the spec stage records the cell's
    `spec_digest` (`04-spec.json`) and verify re-checks it - `task-D3-report.md:257-265`;
    `root-wave3-merge-report.md:458-461`; `git rev-parse HEAD:<path>`, `git show HEAD:<path> | sha256sum`] (differs
    from this block's "template and SHA-256": no template file exists to pin; stated for the PM) [Ruled (D3W-TPL,
    ledger :961): ACCEPTED as written: P9-W's registration pins are the generator blobs `scripts/wave_kinds.py` /
    `scripts/wave_stage_kinds.py` and the kind fixture `scripts/specs/p9/kinds/walk-forward.json` by SHA-256 (as
    recorded here), plus, at the cell's registration commit, the generated cell spec's and the manifest's SHA-256
    (Appendix IV item 18); the cells root checks the generator blobs at HEAD equal those pins before P9-W's run (a
    difference = STOP, the cell VOID-before-read)]. The generated cell
    spec's SHA-256 and the manifest's SHA-256, once the parent is fixed: [ROUND 2b-ii: filled at the cell's
    registration commit by root - Appendix IV item 18].
  - **Disclosed (round 2b-ii; D3W-S S-6 (2), ledger :695, :699; ALCOMB-4).** `--walk-forward` with `--mom-volman` is
    accepted, and the volman scale schedule then stays the full-TRAIN fit's while the weights walk forward: ALCOMB-4's
    limit, which ALCOMB-4 asks this block to state (`task-D3W-review.md:199`); theme-cov-hedge has no such limit
    ("Printed" above).
  - **D3WPB-Q2 (ledger :732).** [ROUND 2b-ii: answered: no P9-W parent carries a TRAIN-fitted aim gain: P9-W is
    unaffected, and the disclosure D3WPB-Q2 would require ("rows refit weights; the aim gain is the TRAIN constant; the
    prediction concerns the weights") is not needed - ledger :815; `root-wave3-merge-report.md:787-789`].
  - **Disclosed (round 2b-ii; D3W review item 6, root M3 item 6 "carried to PRE / P9-W").** The v1 / v2-only weight
    readers refuse a v3 file (fail-closed): `atx-impl/src/strategy_marginal_ic.cpp:267`,
    `atx-impl/tools/book_diagnostics.py:743` (`read_book`), `scripts/research_mine.py:68`; "P9-W's cell path must not
    run them on the v3 file, or PRE defers those lines" (`task-D3W-review.md:277-281`; `root-wave3-merge-report.md:292`).
    This block registers one P9-W print, the paired dSR (pinned verb); any other P9-W line that would read the weights
    through one of the three is not produced on the v3 file (stated for the PM) [Ruled (D3W-V3READ, ledger :962): P9-W
    registers ONE print, the paired dSR on the pinned verb; no P9-W line reads the v3 weights through
    `strategy_marginal_ic.cpp`, `book_diagnostics.py` `read_book` or `research_mine.py`; if the driver's wave path itself
    calls one of them on the v3 file and refuses, that is a driver stop before any read (P9-W UNDEFINED (0), logged;
    nothing re-coded in P9)].
- **Lapsed (0; PRE-35, ledger :600, as D3-2 reads it).** The cells start only after lane D3's two guards are merged and
  APPROVE (B.0). P9-W lapses at 0 iff, at that moment, D3W's walk-forward deliverables (the walk-forward C++ fit; the
  v3 weights with `WeightsV3.SingleRowEqualsV1Bytes`) or D3's kind wiring (the `walk-forward` kind out of SCHEMA_ONLY)
  are not merged and APPROVE. Part B (the C++ `ew-theme-aim-v2` fit, the resid diagnostics, the D2-3 deletion) gates no
  cell (PRE-35; D3W-PB, ledger :645). Round 2b-ii part A: both lane conditions hold (D3 merged `0a2ceb2b`, D3W merged
  `08f04e07`, each APPROVE; B.10); D3-Q1 / D3-Q1a's engine-fit identity at the gate's P9-B0 is the remaining lapse
  condition (above; part B). D3W-PB merged too (APPROVE `44a331e6`, ledger :789; `integration-log.md:733`) and gates
  nothing (`task-D3W-PB-report.md:188-191`). Round 2b-ii part B2a: that condition failed: D3-Q1a DIFFERENT (above), so
  **P9-W LAPSED (0)** (PRE-35; ledger :975, LAPSE-TABLE :981); every other line of this block stands as registered and
  is not run; the finding is the "Outcome" above.

### B.7 P9-X: leverage, the Y-1 form (PRE-1, ledger :516; settles OD5-7)

- **id** `p9x-leverage`. **Counts** N_c +1 if run, N_c +0 under PRE-30 (below). **Parent** P9-F0 [read: P9-F0h (PRE-MVX, MVX-F0H)] at L_P. Undefined if
  L_P >= 2.0 (v8y section 6).
- **Parent (round 2b-ii; PRE-MVX, ledger :889; NAV review M-1, `wave-review-nav.md:13`, `:98`).** P9-X's parent is
  P9-F0h (PRE-10's book: the last accepted book that does not read `--risk-model`), not P9-F0; the two coincide unless
  P9-MV is accepted. P9-X's argv stays this block's (one `--risk-model`: the v1.1 store): the NAV exe holds one risk
  store per argv and refuses a second `--risk-model` (`wave-review-nav.md:98`). A two-store NAV (`mv-aim-v1` +
  `vol-target-v1` in one run) is P10 (NAV-2STORE). If P9-MV is accepted, the leverage rule is tested on the book
  without the MV target and the MV book is printed unlevered; the owner's deployment choice is between them, stated
  in the report (PRE-MVX). The line above ("Parent P9-F0 at L_P") is stale text (PRE-40 form). The block's other
  "P9-F0" (the NAV argv the change is made on; u, fit, card, w; the acceptance, degenerate-pair and side-by-side
  lines) stand as written before PRE-MVX; whether each reads P9-X's parent P9-F0h is the PM's (stated for the PM).
  [Ruled (MVX-F0H, ledger :949): YES for every mention that names P9-X's or P9-V's parent, reference, comparison base,
  argv base or side-by-side book (this block's "Parent", "the Y-1 template's on P9-F0's NAV argv", u / fit / card / w,
  the acceptance comparisons, the degenerate pair, the print; B.8 the same and PRE-13's X-10 test); each such "P9-F0"
  in A.4, B.7 and B.8 carries "[read: P9-F0h (PRE-MVX, MVX-F0H)]" beside it (round 2b-ii part B1); the two books
  coincide unless P9-MV is accepted; G-B1 and G-B6 keep P9-F0 (E); if P9-MV is accepted the levered book offered is on
  the h-book, stated (NAV-2STORE)]
- **Form (PRE-1).** `vol-target-v1` at cap 2.0, its registered constants: L_t = clip(2.0 x sigma_ref_t / sigma_hat_t,
  1, 2.0), sigma_hat_t = risk-target-v1's forecast on the pinned store, sigma_ref_t = running mean of the book's own
  estimates (current included), re-estimated every 21 sessions, cap before the first estimate; constants cap 2.0, floor
  1, cadence 21, annualisation 252 (`scripts/specs/v8/y-vol-target.json`, `4e190f54...`, description; v8y section 8).
  `gross_match: "none"` (PM7-20 reading B; the Y-1 template: "NOT gross matched").
- **Shape (PRE-11, ledger :523).** By hand through `research_cycle.py` (the X-10 / Y-1 precedent, ledger :156, :167):
  the spec, the scaled mechanics limits and the verdict arithmetic committed before the run; an independent second
  derivation of the verdict before the ledger line; the B2-11 hash check of the eval verb done and logged by hand. The
  change is the Y-1 template's on P9-F0's [read: P9-F0h (PRE-MVX, MVX-F0H)] NAV argv: `nav.leverage 2.0` and
  `--vol-target vol-target-v1 --risk-model build-equity/p9-m2i-dcov-v11 --risk-model-sha256
  397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7`; NAV-only (u, fit, card, w are P9-F0's [read:
  P9-F0h (PRE-MVX, MVX-F0H)]). [ROUND 2b-ii: filled at the cell's registration commit by root - Appendix IV
  item 19] (root writes and commits the P9-X hand spec and records its SHA-256). **Not a v2 `leverage` wave (PRE-32,
  ledger :597).** PRE-1 and PRE-11 supersede E2-13 (b)'s clause "a leverage change is P9-X, kind `leverage`": P9-X is
  the Y-1 form, run by hand as a v1 cycle; no P9 cell is a v2 kind `leverage` wave; that kind stays proven by its
  fixtures and the E2-10 probes. [FLAGS-ROUTE, `f8d852f3` (FLAGS-ROUTE-2 (b)): this hand spec is written from P9-F0h's
  NAV argv with the stated change only, and that argv carries both opt-ins from the parent's `nav.flags`, so the cell
  side carries `--ann-mean-gross` (alike with `p9-b0g-y1-nav` and with P9-F0h) and `--info-clock-print` (one-sided
  against `p9-b0g-y1-nav`, whose exe-direct argv does not name it; alike against P9-F0h, the book it is written from,
  and P9-V's side alike against P9-X, B.8): print-only, byte-neutral but for `/scenarios/*/info_clock` (ALCLOCK-1);
  disclosed in the cell record, not a void]
- **Store (PRE-29).** `build-equity/p9-m2i-dcov-v11`: the Release-built `atx-risk-v1.1` container store D-COV wrote (the
  same exe, role, fields and argv as B.5's store but `--recipe atx-risk-v1.1`): `manifest.json` `397366473b3a3b09...`
  (above), container root `b39a1b776639e7d0b88c70e8a43bb9f6c5e6dacb92a336a327234dfc5d312e1d`, `model.atxcov`
  `cd48d5749dd7484ca69ae6b0b9df142b73e2beb64c814d3c143982ec59bb2ce3` (510,677,824 bytes) (`handoff-root-m2i.md`; ledger
  :582). The re-based Y-1 reference of the wave-3 P9-B0 reads it too. P9-X is not byte-comparable with v8's Y-1, whose
  store of record (`v8-risk-lo3-v10`) a Debug exe built: ULP-level covariance differences (PRE-29; COV-7 / COVR). S9
  role check before the run (COV-9).
- **No new trial when nothing is accepted before it (PRE-30, ledger :570).** If P9-F0 [read: P9-F0h (PRE-MVX,
  MVX-F0H)] is the re-based Y-F0, the Y-1 form
  of P9-X is the same book as Y-1, which v8 ran, accepted and counted: P9-X is then not a new trial: the re-based Y-1
  reference of the wave-3 P9-B0 is the cell of record, N_c +0, verdict of record = Y-1's (ledgered, accepted, ledger
  :167, re-derived :184), and P9-V pairs against that reference. If any P9 cell is accepted before it, P9-X runs as
  registered (N_c +1). (Round 2b-ii part B1: read with P9-F0h, the two sentences part when P9-MV alone is accepted
  before P9-X: P9-F0h is then still the re-based Y-F0, so P9-X's book is Y-1's, while "any P9 cell is accepted before
  it" holds; which branch applies (N_c +0 with Y-1's verdict, or N_c +1 as registered) is not ruled; stated for the
  PM.) [Ruled (PRE30-MVX, ledger :969): +0: PRE-30's condition reads on P9-X's parent under PRE-MVX: P9-X is not a new
  trial iff P9-F0h is the re-based Y-F0 of the P9-B0 (no accepted cell other than P9-MV before it), and then the
  re-based Y-1 reference (`build-equity/p9-b0g-y1-nav`, A.4) is the cell of record with Y-1's verdict of record after
  PRE-34's agreement check (the cells root's, PRE34-WHO); if P9-F0h is any other book, P9-X runs as registered (N_c
  +1)]
- **What prints when P9-X is not a new trial (PRE-34, ledger :599).** The cell of record is the re-based Y-1 reference
  of the wave-3 P9-B0 (frozen exes, Release v1.1 store); its figures are the ones printed for P9-X and, if it is P9-F
  (PRE-13), for G-B2, with v8's ledgered Y-1 figures beside them. Reading them is not a look (the same book up to a
  ULP-level change of its covariance input; 0 trials) PROVIDED they agree with the ledgered Y-1 figures at the ledger's
  printed precision (one unit in the last printed place of S2 net Sharpe and of net annual return); the verdict of
  record is then Y-1's. If they differ by more, nothing further is read: root stops and the PM rules (something other
  than the store's ULP moved). The frozen build's book is the one offered; the agreement check is made before the
  adoption print.
- **Acceptance (decides).** X-10's rule (PM7-34 (3)): S2 net annual return (1x) above P9-F0's [read: P9-F0h (PRE-MVX,
  MVX-F0H)] AND S2 net Sharpe not lower than P9-F0's [read: P9-F0h (PRE-MVX, MVX-F0H)] by more than .100 AND
  mechanics at the scaled limits, written in the cell's plan before the run: gross
  inside [1.0, 2.0] x G_P / L_P widened by .005, the other rows at X-10's scaled limits r = 2.0 / L_P: |mean net| <= .02
  r, tau mean <= .20 r, p95 <= .30 r (v8y section 8; v8x section 8). The driver has neither rule nor scaling
  (`scripts/wave_rules.py:79`, `:143`): verdict by hand with independent re-derivation (PRE-11).
- **Realised leverage (C3-4, ledger :537).** OD5-7's cap is on the book's realised gross leverage; `vol-target-v1`'s L_t
  is capped at 2.0 by construction.
- **Degenerate pair (B2-13, ledger :503; PRE-33, :598: B2-13's fallback applies to P9-X alone, whose acceptance has no
  paired line).** If 1 - rho < 1e-6 vs P9-F0 [read: P9-F0h (PRE-MVX, MVX-F0H)], the paired SE / t / p print "na"
  (B2-6) and the cell is decided on its
  non-paired acceptance lines only (net higher AND S2 net Sharpe not lower by > .100 AND mechanics). Measured: Y-1 vs
  X-10 rho 0.9947113980219977, X-10 vs Y-F0 0.9994863034956717, neither degenerate (C.11). [ROUND 2b-ii: the P9-X
  pair on the gate's P9-B0 references, re-based Y-1 vs re-based Y-F0 (`p9-b0g-y1-nav` vs `p9-b0g-yf0-nav`, T 1005):
  rho 0.9947035686613187, "1 - rho < 1e-6" FALSE (the paired line is defined on the references; B2-13's fallback does
  not apply there); the Release eval verb p9-3f `e2b48c46...962c` through the bounded runner, only `paired.{sessions,
  rho, one_minus_rho, degenerate}` read; if P9-X runs as registered (PRE30-MVX: N_c +1), the cell's own pair (P9-X vs
  P9-F0h) is measured by the cells root and logged before its read (B2-13) - `gate-logs/rho-pairs-2.txt`;
  `root-gate-report.md:194`, `:909-923` (at `d22246f6`); `handoff-root-gate.md:82-84`].
- **Printed.** P9-F0 [read: P9-F0h (PRE-MVX, MVX-F0H)] / P9-X (and Y-F0) side by side: net annual return, net Sharpe at
  1x and 4x, realised vol, max drawdown (NAV exe scenario summary, `strategy_nav_replay.cpp:1996-2002`; 4x
  `strategy_nav_v7.cpp:357-382`); mean L_t (the NAV's `vol_target.csv`); paired dSR vs P9-F0 [read: P9-F0h (PRE-MVX,
  MVX-F0H)] and both p (pinned verb).
- **Fit / eval mode:** NAV-only cell (u, fit, card, w are P9-F0's [read: P9-F0h (PRE-MVX, MVX-F0H)]).

### B.8 P9-V: vol-target-v1 on `atx-cov-v1` (COV-6, ledger :198; registered by PRE-1)

- **id** `p9v-vol-target-cov`. Registered because P9-X is the Y-1 form (PRE-1; COV-6). **Counts** N_c +1 if run.
- **Rule.** `vol-target-v1` with its registered constants (cap = P9-X's L 2.0, floor 1, cadence 21, annualisation 252)
  on P9-F0 [read: P9-F0h (PRE-MVX, MVX-F0H)], reading `build-equity/p9-m2i-dcov-covv1` (manifest `ea58691f...f0f3`,
  B.5) instead of the v1.1 store (plan
  section 5.2 :909; PRE-29: both stores from the same Release exe in the same step group, so the pair differs by the
  recipe only). `gross_match: "none"`. Lineage parent named = P9-X (E2-15); under PRE-30 that is the re-based Y-1
  reference. Round 2b-ii (PRE-MVX, ledger :889): P9-V's parent and pair stay P9-X, whose parent is P9-F0h (B.7); P9-V
  reads one store (`p9-m2i-dcov-covv1`) like every argv (B.7); this line's "on P9-F0" stands as written before PRE-MVX
  (stated for the PM, as B.7) [Ruled (MVX-F0H, ledger :949): YES, as B.7: this block's "P9-F0" that names P9-V's
  argv base, reference or comparison base (this line's "on P9-F0", PRE-13's X-10 test below) reads P9-F0h and carries
  the marker; the two coincide unless P9-MV is accepted].
- **Shape (PRE-11).** By hand, as B.7: the NAV argv of P9-X with `--risk-model build-equity/p9-m2i-dcov-covv1
  --risk-model-sha256 ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3`. [ROUND 2b-ii: filled at the
  cell's registration commit by root - Appendix IV item 21] (root writes and commits the P9-V hand spec and records its
  SHA-256). [FLAGS-ROUTE, `f8d852f3` (FLAGS-ROUTE-2 (b)): this hand spec is P9-X's NAV argv (written from P9-F0h's)
  with the stated change only, so the cell side carries `--ann-mean-gross` (alike with `p9-b0g-y1-nav` / P9-X) and
  `--info-clock-print` (one-sided against `p9-b0g-y1-nav`, the reference under PRE-30 / PRE30-MVX, whose exe-direct
  argv does not name it; alike against P9-X): print-only, byte-neutral but for `/scenarios/*/info_clock` (ALCLOCK-1);
  disclosed in the cell record, not a void]
- **Acceptance (decides).** Paired dSR > 0 vs P9-X AND mechanics at P9-X's scaled limits; a rejected P9-X still serves
  as the reference (plan section 5.2 :915-917). By hand (PRE-11). **Degenerate pair (PRE-33, ledger :598; one rule per
  cell, fixed before the pair's rho is measured).** P9-V is a dSR-decided cell: if 1 - rho < 1e-6 vs P9-X, PRE-12
  governs it (point dSR > 0 AND mechanics; SE / t / p "na"), and the print adds "pair degenerate: the two stores give
  the same book to within 1e-6 of correlation". PRE-13 still asks P9-V to pass the X-10 rule against P9-F0 [read:
  P9-F0h (PRE-MVX, MVX-F0H)] (MVX-PRE13) before it is P9-F. [ROUND 2b-ii: a CELL-TIME value: the P9-V pair (P9-V vs
  P9-X, or vs the re-based Y-1 reference under PRE-30 / PRE30-MVX) is measured by the cells root and logged before
  P9-V's read (B2-13); no P9-V book exists at the gate; P9-V's references are P9-X's (PRE-MVX), whose gate pair is
  B.7's (re-based Y-1 vs re-based Y-F0, rho 0.9947035686613187, flag false) - `root-gate-report.md:195`, `:922-923`
  (at `d22246f6`)].
- **Registered predictions = D-COV's printed numbers** (plan section 3.4 step 6; COV-5, ledger :197; COV-11, :402;
  COV-13, :587). Source: `root-wave2-gate-report.md` item 4 (d) "C4 D-COV" (M2i, ledger :582): both stores on identical
  observations (every family's observation count equal; per eigen rank the same sessions and finite-z pattern, 51
  ranks); risk-forecast accuracy only. Every row is read in both directions (COV-11): b below 1 - sqrt(2 / T) =
  over-forecast / over-corrected; above 1 + sqrt(2 / T) = under-forecast. Printed beside P9-V's result; they decide
  nothing.
  - **(a)** the book's sigma_hat bias closer to 1 under `atx-cov-v1` than under v1.1, read in both directions (a book b
    pushed below the band is over-correction, not a confirmation). [ROUND 2b-ii: B8A-PROD: the M2g Q5 chain
    (`root-wave2-gate-report.md` item 5 steps 0-2), run by the CELLS root inside P9-V's cell (step 2), after its
    registration commit: step 0 the holdings: P9-X's parent NAV (P9-F0h) re-run with its exact argv plus
    `--emit-holdings <new dir>` on the frozen Release NAV exe, every other output byte-identical to the parent's (the
    step-0 identity form; a difference = (a) "not produced"); step 1 `book-csv`; step 2 the frozen Release
    `atx-equity-strategy-risk risk --book-weights <that csv>` once per recipe, with each pinned store's own build argv
    (`--recipe atx-risk-v1.1` for `p9-m2i-dcov-v11`, `--recipe atx-cov-v1` for `p9-m2i-dcov-covv1`, the same role,
    fields and options as their manifests record), written to NEW output dirs (never into a pinned store dir); every
    step through the bounded runner (COVR-1) after the COV-9 role check; the print is the `book` family's full b mean /
    median and in-band flag per recipe from each `bias_summary.json`, read in both directions (COV-11), beside P9-V's
    result, deciding nothing; any refusal = (a) printed "not produced: <step, refusal line>" - ledger :971;
    `root-gate-report.md:154`]. Stated beside (FIELDS-GONE, ledger :978; `root-gate-report.md:471-474`): the two
    stores' build argv names `--fields .../train-2020-2023-lo3-fields-v10/manifest.json`, absent from disk, so step 2 as
    ruled would be refused at the fields input and (a) would print "not produced"; whether a fields substitution (as
    FIELDS-SUB for R-2's NAV) applies there is not ruled; stated for the PM. [Ruled (B8A-PROD-2, ledger :995): step 2
    (`risk --book-weights`, once per recipe) runs with the fields input substituted as FIELDS-SUB did for R-2:
    `--fields build-equity/train-2020-2023-lo3-fields-v15/manifest.json --fields-sha256 26fee5ce...` (the witness 70 /
    70 of `gate-logs/fields-witness.txt` covering the risk exe's field reads as it covered the NAV's), every other token
    as the pinned store's build receipt, stated in P9-V's record; a refusal, a field the risk step needs that is not
    among the 70 witnessed, or a run outside the bounds = (a) "not produced: <reason>"; a diagnostic printed beside
    P9-V, deciding nothing]
  - **(b)** mean L_t and its changes (the NAV's `vol_target.csv`), printed beside P9-X's.
  - **(c)** D-COV's families, v1.1 | atx-cov-v1 (`bias_summary.json`):

| family | status | series ok | obs | full b mean | full b median | in band | in kurt. band | roll63 share / b mean | roll252 share / b mean | QLIKE | MRAD 63 / 252 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| random | ok / ok | 64 / 64 | 69,696 | 0.9603 / 0.9611 | 0.9618 / 0.9624 | 34 / 34 | 44 / 44 | .7232 / .9431 \| .7234 / .9438 | .6072 / .9369 \| .6105 / .9374 | 2.4080 / 2.4078 | .1321 / .0824 \| .1320 / .0820 |
| factor | ok / ok | 51 / 51 | 51,193 | 1.0016 / 1.0041 | 1.0050 / 1.0181 | 27 / 25 | 36 / 35 | .6361 / .9745 \| .6364 / .9760 | .5764 / .9803 \| .5809 / .9812 | 2.5149 / 2.5144 | .1693 / .0944 \| .1692 / .0949 |
| eigen | ok / ok | 51 / 51 | 54,770 | 434.61 / 379.78 | 1.1629 / 1.1217 | 10 / 9 | 48 / 48 | .4346 / 367.78 \| .4824 / 331.78 | .2568 / 430.53 \| .2849 / 379.74 | 5,621,299.8 / 4,149,159.8 | 361.50 / 421.87 \| 326.03 / 371.99 |
| minvar | ok / ok | 1 / 1 | 1,089 | 1.4995 / 1.4747 | 1.4995 / 1.4747 | 0 / 0 | 0 / 0 | .5063 / 1.3364 \| .5628 / 1.3110 | .1969 / 1.3145 \| .2792 / 1.2908 | 3.1942 / 3.1430 | .3725 / .3178 \| .3577 / .2974 |
| optimized | ok / ok | 32 / 32 | 34,848 | 1.0081 / 1.0078 | 1.0045 / 1.0044 | 23 / 23 | 26 / 27 | .7409 / .9921 \| .7408 / .9918 | .7692 / .9900 \| .7694 / .9898 | 2.4311 / 2.4271 | .1341 / .0602 \| .1341 / .0601 |

  - **(d)** D-COV's eigen family per rank (b = sample SD of z, ddof 1; T = 1,089 for ranks 00-47, 948 / 817 / 733 for
    ranks 48 / 49 / 50; band sqrt(2 / T) = .0429 (.0459 / .0495 / .0522); reduction = |b_v1.1 - 1| - |b_cov - 1|,
    positive = atx-cov-v1 nearer 1):

| rank | b v1.1 | b atx-cov-v1 | v1.1 | atx-cov-v1 | reduction |
|---|---|---|---|---|---|
| 00 | 16074.6219 | 13667.7331 | above | above | +2406.8888 |
| 01 | 4802.5569 | 4565.5452 | above | above | +237.0117 |
| 02 | 1230.4744 | 1079.8917 | above | above | +150.5827 |
| 03 | 1.8387 | 1.6799 | above | above | +0.1588 |
| 04 | 1.6923 | 1.5373 | above | above | +0.1550 |
| 05 | 1.7420 | 1.6582 | above | above | +0.0838 |
| 06 | 1.6976 | 1.5493 | above | above | +0.1483 |
| 07 | 1.6226 | 1.5702 | above | above | +0.0525 |
| 08 | 1.5256 | 1.4314 | above | above | +0.0942 |
| 09 | 1.6104 | 1.4994 | above | above | +0.1110 |
| 10 | 1.5563 | 1.4260 | above | above | +0.1303 |
| 11 | 1.5363 | 1.3884 | above | above | +0.1480 |
| 12 | 1.3838 | 1.3174 | above | above | +0.0664 |
| 13 | 1.3362 | 1.2963 | above | above | +0.0399 |
| 14 | 1.3258 | 1.2274 | above | above | +0.0984 |
| 15 | 1.3030 | 1.2265 | above | above | +0.0765 |
| 16 | 1.3352 | 1.2610 | above | above | +0.0742 |
| 17 | 1.3218 | 1.2462 | above | above | +0.0756 |
| 18 | 1.3773 | 1.2865 | above | above | +0.0908 |
| 19 | 1.3108 | 1.2384 | above | above | +0.0724 |
| 20 | 1.3225 | 1.2681 | above | above | +0.0544 |
| 21 | 1.2136 | 1.1690 | above | above | +0.0446 |
| 22 | 1.2481 | 1.2251 | above | above | +0.0230 |
| 23 | 1.2400 | 1.1790 | above | above | +0.0610 |
| 24 | 1.1761 | 1.1868 | above | above | -0.0107 |
| 25 | 1.1629 | 1.1150 | above | above | +0.0479 |
| 26 | 1.1523 | 1.1217 | above | above | +0.0306 |
| 27 | 1.1432 | 1.1135 | above | above | +0.0297 |
| 28 | 1.1202 | 1.0983 | above | above | +0.0219 |
| 29 | 1.1196 | 1.1035 | above | above | +0.0161 |
| 30 | 1.0858 | 1.0993 | above | above | -0.0136 |
| 31 | 1.0962 | 1.0598 | above | above | +0.0364 |
| 32 | 0.9869 | 0.9337 | in band | below | -0.0532 |
| 33 | 0.8009 | 0.8837 | below | below | +0.0828 |
| 34 | 1.0167 | 1.0515 | in band | above | -0.0349 |
| 35 | 1.0781 | 1.0883 | above | above | -0.0102 |
| 36 | 1.0251 | 1.0255 | in band | in band | -0.0004 |
| 37 | 1.0280 | 1.0220 | in band | in band | +0.0060 |
| 38 | 1.0393 | 1.0761 | in band | above | -0.0368 |
| 39 | 0.9842 | 0.9783 | in band | in band | -0.0060 |
| 40 | 1.0003 | 1.0222 | in band | in band | -0.0220 |
| 41 | 0.9882 | 1.0003 | in band | in band | +0.0115 |
| 42 | 1.0015 | 1.0261 | in band | in band | -0.0245 |
| 43 | 0.9165 | 0.9630 | below | in band | +0.0465 |
| 44 | 0.9863 | 0.9694 | in band | in band | -0.0169 |
| 45 | 0.9437 | 1.0023 | below | in band | +0.0540 |
| 46 | 0.9113 | 0.9402 | below | below | +0.0289 |
| 47 | 0.7836 | 0.8009 | below | below | +0.0172 |
| 48 | 0.7365 | 0.7536 | below | below | +0.0171 |
| 49 | 0.7118 | 0.7521 | below | below | +0.0403 |
| 50 | 0.8135 | 0.8004 | below | below | -0.0131 |

  - Read in both directions (COV-11; gate report wording): ranks in band v1.1 10 of 51, atx-cov-v1 9 of 51; ranks
    reduced 39 of 51 (12 negative); below band = over-forecast / over-corrected in v1.1 at ranks 33, 43, 45-50 (8 ranks)
    and in atx-cov-v1 at ranks 32, 33, 46-50 (7 ranks). Ranks 00-02 have b above 1,000 in both recipes, so every pooled
    and mean eigen figure is dominated by them (root's finding, ledger :582; COV-13).
  - **(e) The three pooled eigen rows (COV-13 (b)):**

| pooled eigen row | N | band | b v1.1 | b atx-cov-v1 | source |
|---|---|---|---|---|---|
| all ranks | 54,770 | .0060 | 2370.9495 | 2036.9591 (both above) | D-COV, gate report item 4 (d) |
| without eigen-00 | 53,681 | .0061 | 705.8525 | 667.9782 (both above) | D-COV, same |
| without structurally null ranks | 54,770 | .0060 | 2370.9495 | 2036.9591 (= the all-ranks row: no rank excluded) | D-COV-EIG outcome as COV-14 (ledger :621) rules it |

  - Reading rule (COV-13 (b), fixed before D-COV-EIG runs): a rank is "structurally null" iff its median
    squared-loading share on (i) factors flagged structural plus (ii) the industry block and the country / market factor
    is at least .99; such a rank is excluded from the third row; a rank that fails the rule stays in every pooled row and
    is printed as a finding. COV-13 had P9-V's registered predictions quote the third row, with the all-ranks row printed
    beside it.
  - **D-COV-EIG's outcome (COV-14, ledger :621).** The reading rule stands as fixed before the look. No rank 00-02 is
    structurally null in either recipe, so no rank is excluded and the third pooled row equals the all-ranks row; the
    three ranks are a FINDING (near-null factor directions under-forecast by a factor above 1,000 in both recipes).
    P9-V's registered predictions quote the all-ranks pooled row and the per-rank table (d). The pooled eigen figure is
    dominated by ranks 00-01; printed beside it: the count of ranks nearer 1 under `atx-cov-v1`, 39 of 51 ((d), column
    "reduction"). No exclusion rule is added and no `atx-cov-v1` parameter moves (COV-5); COV-13 (c) binds COV-MV
    unchanged (B.5). The model defect is a P10 item for the risk model.
- **Fixed.** Every `atx-cov-v1` parameter, as step 5's manifest printed it: `{"amplification": 1.0, "method":
  "menchero-wang-orr simulated (USE4 B7)", "seed": 7, "seed_rule": "per session", "simulated_length": "kish effective
  count of the HL-504 weights", "simulations": 64}`; sessions adjusted 1,279, not run 126, refused 0 (gate report item 4
  (d) C6). A change after D-COV is a new recipe id and a P10 note (COV-5; COV-13 (a): no parameter moves). S9 role check
  before the run (COV-9). Not read on history (plan section 5.3 :955-956).

### B.9 P9-S: lapsed

P9-S lapses at 0 trials; N_c and K_a unchanged (W3-15, ledger :393; OD5-4, ledger :464: no new-signal cell in P9).
Admission list: empty (0 strings; no roster, no DSL or argv SHA-256 to pin). Nothing from WQ-EXPLORE is registered
(WQ-1, ledger :343; OD5-4).

### B.10 Lapse table (round 2b-ii)

Part A fills the lane-decided column from the ledger's "Task <lane>: APPROVE at" lines and `integration-log.md`'s
merge rows; the gate-decided column is `pending the gate` until part B. "runs" = no lapse condition of PRE-14 / COV-7
/ PRE-35 is open; "pending" = a gate-decided condition is open; "lapses (0)" is not used in part A. Every cell still
waits for the gate's PROCEED (GATE-REV), the PM's ruling of this file (PRE-2BII-AGENT) and a defined parent (E-45);
the order of A.7 never changes. Common to every v2 cell, merged and APPROVE: E2 (`80c322ef`, merge `430c29e8`; ledger
:540, `integration-log.md:595`), C3b (`b2517fe4`, merge `b182dfdd`; E2-16; ledger :713, `integration-log.md:698`) and
D3's two guards (`699440bb`, merge `0a2ceb2b`; PRE-35; ledger :753, `integration-log.md:714`).

| cell | lane-decided (merged + APPROVE; merge commit) | gate-decided | status |
|---|---|---|---|
| P9-L | yes: S2, re-review APPROVE `2ed2fc18` (ledger :364), merge `f6444417` (`integration-log.md:513`); never lapses (B.1) | none named | runs |
| P9-H | yes: AL-COMB (`theme-cov-hedge-v1`), APPROVE `efb7ab2e` (ledger :447), merge `9d518317` (`integration-log.md:551`); registered (PRE-6, B.2) | none named | runs |
| P9-M | yes: AL-COMB (`mom-volman-v1`), as P9-H; the sleeve line's producer D3W, APPROVE `1e6c4336` (ledger :694), merge `08f04e07` (`integration-log.md:699`): line kept (B.3) | none named | runs |
| P9-C | yes: AL-CLOCK, APPROVE `7ebd41ff` (ledger :744), merge `858a4fca`; fix round 1 APPROVE `eed512bd` (ledger :860), merge `1d003241` (`integration-log.md:716`, `:759`) (PRE-14) | none named; part B2a: ALCLOCK-1 HELD at the gate (the opt-in byte-neutral but for the `info_clock` block, so the block stays and the P9-B0 Y-F0 NAV names `--info-clock-print`; `gate-logs/nav-identity-yf0.txt`; B.4) | runs |
| P9-MV | yes: COV-MV, APPROVE `25e11b32` (ledger :813), merge `5c811b56` (`integration-log.md:717`) (COV-7, PRE-14) | COMPOSE-1 (ledger :832): pending the gate (the tiny-world parse of `--rate info-clock-rate-v1 --target-rule mv-aim-v1`; a refusal there or at P9-MV's run lapses P9-MV at 0); part B2a: **ACCEPT** at the gate's store-backed parse check (COMPOSE-1a, :956; `gate-logs/compose-1a.txt`; B.5); a refusal at P9-MV's own preflight still lapses it at 0 before any read (COMPOSE-PRE) | runs |
| P9-W | yes: D3 (kind wiring), APPROVE `699440bb` (ledger :753), merge `0a2ceb2b`; D3W (walk-forward C++ fit, v3 weights), APPROVE `1e6c4336` (ledger :694), merge `08f04e07` (`integration-log.md:714`, `:699`) (PRE-35) | D3-Q1a (ledger :898; D3-Q1, :726): pending the gate (engine-fit identity of the re-based Y-F0; a difference lapses P9-W at 0, PRE-35); part B2a: **DIFFERENT** (694 float leaves of the stripped weights; admission identical; `gate-logs/d3q1a.txt`; B.6): "Cell P9-W: LAPSED (0)" (:975) | lapses (0) |
| P9-X | n/a: hand cell, no lane (PRE-1, PRE-11); its store `p9-m2i-dcov-v11` built at M2i (ledger :582) | PRE-30 (ledger :570; PRE-34, :599): pending the gate (the re-based Y-1 reference of the P9-B0; P9-X runs as registered, N_c +1, iff a P9 cell is accepted before it, else that reference is the cell of record, N_c +0); part B2a: the gate delivered the reference (`build-equity/p9-b0g-y1-nav`, A.4); still PENDING PRE-30 as PRE30-MVX (:969) reads it: N_c +0 with that reference as the cell of record iff P9-F0h is the re-based Y-F0 (no accepted cell other than P9-MV before P9-X), after PRE-34's agreement check by the cells root (PRE34-WHO); else P9-X runs, N_c +1 | pending |
| P9-V | n/a: hand cell, no lane (PRE-1, PRE-11); its store `p9-m2i-dcov-covv1` built at M2i (ledger :582) | none named (its pair follows P9-X's PRE-30 outcome, B.8) | runs |

Gate-decided column: part B2. [Ruled (COMPOSE-PRE, ledger :964): the P9-MV gate-decided cell reads COMPOSE-1 as
COMPOSE-1a (:956) refines it: "accept at the gate's store-backed parse check, or (if no tiny-world store can be
built) at P9-MV's own preflight; a refusal at either lapses P9-MV at 0 before any read"; part B2 fills which of the
two the gate delivered; the row stands as part A wrote it] [Part B2a: the gate delivered the store-backed check
(ACCEPT; the P9-MV row and B.5)]

Round 2b-ii part B2a (LAPSE-TABLE, ledger :981; CELLS-4's table of record, stated before any cell): the gate-decided
column is filled for P9-C, P9-MV and P9-W from the gate records at `65ce28f0`, and the status column follows; P9-X stays
pending PRE-30 as PRE30-MVX reads it (decided at the P9-X step, not by the gate). The table now reads as LAPSE-TABLE
states it: P9-L RUNS; P9-H RUNS; P9-M RUNS (sleeve line kept, B.3); P9-C RUNS (ALCLOCK-1 held); P9-MV RUNS (COMPOSE-1a
ACCEPT, store-backed); P9-W LAPSED (0) (D3-Q1a DIFFERENT); P9-X PENDING PRE-30 as PRE30-MVX reads it; P9-V RUNS (pair =
P9-X or its reference). Fit mode of every v2 cell: flag-absent (B.0). Order unchanged (A.7); the table never changes
after a read (CELLS-4). Every running cell still waits for the gate's PROCEED (GATE-REV) and the PM's ruling of this
file.

---

## C. Standing lines (every cell, every print, the adoption print, the OD-3 print, the freeze report)

1. **Identity-bridge flag** (OD5-1 (i), ledger :461; text per ALD-9, :473): "universe `linked-operating-v3` and the
   fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in
   `al-data/atx-db-request.md` A0); size unmeasured". G-B1, G-B2 and G-B8 print with it. Absolute Sharpe / return
   figures are stated as possibly overstated by survivorship; the evidence of record is the paired, relative tests
   (OD5-1 (ii)). `al-data/atx-db-request.md` is handed to the owner with the freeze report; a later non-zero diff
   annotates the freeze verdict, nothing is re-run (OD5-1 (iv)). Round 2b-ii (A4-P10-FLIPS, ledger :867, :876; B.0
   "Fields producers"): the A4 engine builders of `ftd_shares_ratio21` and `regsho_threshold_days63` are merged but
   unproven on real inputs in P9 (their TRAIN inputs sit under `C:/atx/atx-db/`, A4-DB); Python stays the producer of
   record of both fields; Y-F0's fields manifest `26fee5ce` and every run of record are unchanged; the flag text above
   is unchanged.
2. **ALD-DIAG** (OD5-1 (iii), ALD-10, ledger :474; run at M2g, ledger :510; `root-wave2-gate-report.md` item 2): role
   `train-2020-2023-lo3` (pin `e1c67101...95f4`; 1,405 sessions 2018-06-01 .. 2023-12-29; seal check passed); no return
   statistic; gates nothing.

| year | names in the role universe | universe exits through a delisting, last session <= 2023-12-01 | share of the year's names | by cause (mna / performance / non_common / unknown / unknown_unlinked) | unclassified (last session > 2023-12-01) |
|---|---|---|---|---|---|
| 2020 | 2,420 | 83 | .034 | 82 / 1 / 0 / 0 / 0 | 0 |
| 2021 | 2,540 | 103 | .041 | 97 / 1 / 0 / 5 / 0 | 0 |
| 2022 | 2,325 | 106 | .046 | 102 / 1 / 0 / 3 / 0 | 0 |
| 2023 | 2,186 | 95 | .043 | 88 / 2 / 0 / 5 / 0 | 2 (within 30 days of the seal) |

   2020-2023: 3,256 distinct universe names; 387 classified delisting exits + 2 unclassified; a lower bound (ALD-10).
   Root's reading (gate report): the role keeps its pre-2024 delistings (about 3.4-4.6% of each year's names), almost
   all M&A; performance delistings of universe members are 1-2 a year (5 in 2020-2023), so the universe holds few
   failure exits.
3. **G-B2 hurdle** 8.0%, labelled "plan placeholder, not owner-stated" in every print (OD5-6, ledger :466).
4. **Leverage** L <= 2.0 in every P9 cell (OD5-7; E2-10, ledger :438; C3-1, ledger :507). OD5-7's cap is on the book's
   realised gross leverage in P9 cells; P9-X and P9-V are `vol-target-v1`, whose L_t is capped at 2.0 by construction;
   `risk-target-v1` is registered for no P9 cell; a later cell that names a rule whose realised leverage can exceed its
   parameter states its cap on realised leverage in its registration (C3-4, ledger :537). Production leverage stays the
   owner's decision after the freeze (OD5-7).
5. **Timings off** for every pinned run (W1-MIN M-3, ledger :316).
   [PRE-40, R15-WAVE-READ: this forbids engine timing flags. Native driver.timings process/receipt telemetry and
   scoreboard --timings are allowed for G-P1; they change no engine or research input.]
6. **CRT record** (M2c-RED-2 (d), ledger :441; CRT-2, :588): `ucrtbase.dll` file version and SHA-256 recorded at each
   P9-B0 and before each cell; a change means P9-B0 and the re-pin are redone before any further cell. The two
   Release-built D-COV stores (`p9-m2i-dcov-v11`, `p9-m2i-dcov-covv1`) are recorded as built under `ucrtbase.dll`
   10.0.26100.7623 `2c08e77f...65ef` (`ucrtbased.dll` 10.0.22000.832 `19e79e91...f48d` recorded beside;
   `handoff-root-m2i.md`); they are pinned bytes, never rebuilt in P9 (a rebuild is a new store, COV-5); a CRT change
   before a cell does not void them, their read path calling no CRT transcendental (M2i C5) (CRT-2). [ROUND 2b-ii:
   `ucrtbase.dll` (C:\Windows\System32) file version 10.0.26100.7623, SHA-256
   `2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF`, recorded at the gate's step 0 (agents 1-4), at the
   cache-free P9-B0 runs (2026-10-16T14:22:31Z) and at the Y-F0 P9-B0 runs (15:39:47Z): unchanged, = the CRT-2 record of
   the two D-COV stores; `ucrtbased.dll` 10.0.22000.832 `19E79E91...F48D` unchanged; each cell records it again before
   its run (a change redoes P9-B0 and the re-pin) - `gate-logs/ucrt-b0-3.txt`, `ucrt-b0-4.txt`;
   `handoff-root-gate.md:95-99`; `root-gate-report.md:196` (at `d22246f6`)].
7. **Release** exes for every run of record (M2c-RED-2 (e)); the Release lit-golden test is a named skip
   (M2c-RED-2 (b)-(c); ledger :502); no golden digest is re-pinned in P9 and the deterministic `pow` / `tanh` / `exp2`
   is a P10 owner item (OD5-2, ledger :462; M2I-HK, :589). D-COV's producer of record is Release; Debug = Release of the
   container is replaced by Release = Release (cause: the CRT's `exp2`, COVR), shown on TRAIN at M2i for both recipes
   (COV-12, ledger :511; ledger :582).
8. **EXE-FREEZE** (ledger :406): after the wave-3 P9-B0 and its re-pin commit (the single P9-B0 and re-pin of the one
   gate for waves 2 and 3, GATE-1, ledger :630) the NAV, IC and eval exes are not rebuilt until the freeze gate; an
   unavoidable rebuild redoes P9-B0 and the re-pin first, no cell in between. EXE-CUR (ledger :567): B.0. Round 2b-ii
   part B2b: EXE-FREEZE is in force from the re-pin of record `2078f877` and covers the NAV, IC, eval, composition,
   admission and store exes (GATE-EXES); no exe was rebuilt after it (27 / 27 Release and 27 / 27 Debug `disk =
   receipt` at the gate's end; next free build tag `p9-3h`); the reverted follow-up `3a1a4759` / `f8a85ad4` moved no
   exe pin (B.0 "exes"; `handoff-root-gate.md:21-22`).
9. **S2R list** (S2R-1, :468; S2R-2, :478): `ts_zscore` is used by 0 of the 67 book candidates and 0 P9 strings
   (ledger :477); `smax` and `smax5` carry the line "AuditExact NaN may be ResearchFast 0 on a flat window"; every
   cell runs in its reference's eval mode (PATH-1). **S2R-DIAG** (run at M2g on the P9-B0 u; gate report item 6; ledger
   :510): `smax` 0 / 0 and `smax5` 0 / 0 (name, day) cells NaN under AuditExact and finite under ResearchFast (all
   sessions / score window; 8,320,410 / 5,957,532 cells each); the two NaN patterns are identical in both modes on
   TRAIN; gates nothing. The streaming-path fix is a P10 note (S2R-1, S2R-2, S2R-3).
10. **P9-L exposure and inputs**: the 30-day line of B.1, bounded by the 2 unclassified exits of C.2 (PRE-4), and the
    S2-6 check (ledger :325, :365).
11. **Paired-test fallback** (B2-R1 (c), :310; B2-6, :347; B2-7, :348; B2-13, :503; PRE-12, :524): a pair with
    1 - rho < 1e-6 prints SE / t / p "na"; root logs rho before each paired read; a printed value on a rounding
    boundary is listed and reported to the PM, never auto-accepted. Measured (Release verb `a57a2090`; T = 1,005 common
    sessions; scenario `modeled-1bn-stale5-v1+swap-fin-v1`; ledger :502, :510; gate report item 3):
    [PRE-40, R15-WAVE-READ: for v2 cells the read boundary is the agent's registered read, distinct from the
    native judge's automatic computation/trial write. Preserve raw console unread; seal/void and mechanics first,
    then existing pinned-eval bundle's rho-only keys logged before any paired figure. No new statistic is computed.
    `cells/p9h-cov-hedge-read-plan.md` fixes H's exact filter/extraction scope. C.11's threshold/fallback is unchanged;
    hand cells retain their ordered route.]

| pair | rho | 1 - rho (stable form) | B2-R1 tolerance | class |
|---|---|---|---|---|
| X-5 vs X-3 | 0.92027 | - | - | defined (ledger :502) |
| Y-F0 vs X-5 | 0.95560 | - | - | defined (ledger :502) |
| X-10 vs Y-F0 (P9-B0 refs; as ledgered: same) | 0.9994863034956717 | 5.13696504329025e-4 | 1.947e-9 | defined |
| Y-1 vs X-10 (P9-B0 refs; as ledgered: same) | 0.9947113980219977 | 5.288601978002812e-3 | 1.891e-10 | defined |

    Round 2b-ii part B2a: the X-10 reference of these pairs is re-based at the gate as `build-equity/p9-b0g-x10-nav`
    (substitution list row 2, `778d34b8`: M1d's `p9-b0-x10-nav-run` argv with `--output` changed and `--ann-mean-gross`
    added, Release NAV p9-3f, provisional, re-pin pending): HOLDS: 27 / 27 files, 26 byte-identical to M1d's
    `p9-b0-x10-nav`, `summary.json` differing only at `/producer/build_type`, `/producer/engine_git_sha` and the opt-in
    `/scenarios/0..4/ann_mean_gross`; listing SHA-256 `6c87c8d516d7782c2484d6cc397323359f9f9b61cc242a5f894885240a74f31d`,
    `summary.json` `deabe71b5e2dedfb0b224fb46fdcd0e7194a6950ad8f5ecabe9601cd786bd0bd` (`gate-logs/x10-cmp.txt`,
    `b0-files-p9-b0g-x10-nav.txt`; `root-gate-report.md:646`, `:685-687`; `handoff-root-gate-3.md:83`); the Y-1 reference
    is `p9-b0g-y1-nav` (A.4); the rho values on the gate's references are item 26 (part B2b).
    [ROUND 2b-ii: on the gate's P9-B0 references, through the Release eval verb p9-3f `e2b48c46...962c` (only
    `paired.{sessions, rho, one_minus_rho, degenerate}` read), T 1005 each: the P9-X pair (re-based Y-1 vs re-based
    Y-F0) 0.9947035686613187, "1 - rho < 1e-6" FALSE; X-10 vs Y-F0 (`p9-b0g-x10-nav` vs `p9-b0g-yf0-nav`)
    0.9994863034956717, FALSE; Y-1 vs X-10 (`p9-b0g-y1-nav` vs `p9-b0g-x10-nav`) 0.9947113980219978, FALSE; each cell's
    own pair is a CELL-TIME value, measured by the cells root and logged before that cell's read (B2-13) -
    `gate-logs/rho-pairs-1.txt`, `rho-pairs-2.txt`; `root-gate-report.md:197`, `:745-757`, `:909-923` (at `d22246f6`)]
    (stated beside, not a ruling: the gate's Y-1 vs X-10 differs from the table row above in the 16th significant digit,
    ...978 vs ...977; the gate's Y-1 reference is on the Release v1.1 store, PRE-29; both defined).
    Fallback per cell (one rule per cell, fixed before the pair's rho is
    measured, PRE-33, ledger :598): P9-X B2-13 (non-paired acceptance lines only; P9-X alone, its acceptance having no
    paired line); P9-L's print and P9-H / P9-M / P9-C / P9-MV PRE-12 (point dSR > 0 AND mechanics; SE / t / p "na";
    G-B7 skips the cell); P9-V PRE-12, with the printed line "pair degenerate: the two stores give the same book to
    within 1e-6 of correlation" (PRE-33; B.8).
12. **Not queued**: `hf_crowd` (ALSIG-4, :258), `rank(russell_recon)` (dropped: ALSIG-5 :264; non-PIT: ALSIG-7 :307,
    ALSIG-8 :308; status `dropped`, final), `rank(gia_13f)` (not registered: ALSIG-2), `lazy_prices` (blocked_on_data;
    fails the bar, ALD-4), nothing from WQ-EXPLORE (WQ-1; OD5-4). DSL SHA-256 recomputed: `rank(russell_recon)`
    `2aad0b86...`, `rank(hf_crowd)` `1881f8ce...`, `rank(lazy_prices)` `bcbe2653...` (each = its registration).
13. **No SQLite cache index** in any cell; no `--candidate-cache-index` / `--pair-cache-index` on any argv (SQL-11,
    :212); SQL4's identity runs join the wave-3 set only if SQL4 merges before the cells (SQL-12, :213). [ROUND 2b-ii:
    SQL4-CUTOFF-2's outcome: SQL4's checks green at M6B-FINISH, SQL4 in the gate head `06dc9e5a` (the M6b merge of
    `44a76937`; a red in any of its 24 files attributes to `06dc9e5a`); not BLOCKED, not P10; SQL4's opt-in cache
    identity of record = M6b's items 5 (1a / 1b / 2) and 6 (`handoff-root-m6b.md`, run on the same source), cited; the
    gate's step-7 X-5 opt-in cache identity is CUT (FIELDS-X5: inputs absent, and a `cache init` on the real cache would
    re-create the GATE-IDX file); no cache-index flag on any gate argv and no `store` block in any P9-B0 receipt
    (SQL3-2, SQL-11) - `root-gate-report.md:11-12`, `:51`, `:158`; `gate-logs/b0-runs.txt`; ledger :924, :940, :980].
    [ROUND 2b-ii: the candidate cache's listings (GATE-IDX, GATE-RELCACHE; `gate-logs/tools/dir_listing.py` form):
    before the Release pass, with the GATE-IDX file `index.sqlite`, 352 files `fb444f7d316a1a4c`; after the owner
    deleted it, `build-equity/mega-candidate-cache-v8-lo3` 351 files, 7,799,539,072 bytes, `155872de50833fca` (the 351
    originals); after the Release pass (cause "Release identity subtree added at step 4 (p9-b0g-yf0-u)") 552 files,
    12,265,945,104 bytes, `c9ee9291f5ddc0c2` = the 351 originals unchanged (`155872de50833fca` restricted) + 201 files /
    4,466,406,032 bytes under `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0/`; the warm w added nothing; no `index.sqlite`
    at the end; never re-create it (`cache init`) - `gate-logs/cache-lo3-listing-agent4-idx.txt`,
    `cache-lo3-listing-after-p9-b0g-yf0-{u,w}.txt`, `cache-lo3-listing-agent4-final.txt`; `handoff-root-gate.md:101-108`,
    `:148-149`].
14. **Run caps** `--max-rss-mib 2048` unless the phase is known to need more, then root reads free memory first and
    states the figure (RUN-1, ledger :451); the parent spec's registered phase caps (u 2,560, w 3,072, card 2,560 MiB;
    `lib-v8ysb-gm.json:10-28`) are such known needs. Bounded real-data runs state `--seconds` <= 600 (the runner refuses
    more); longer work is split into stages (RUN-2, :513). COV-reading runs go through `scripts/run_bounded_research.py`
    (COVR-1, :559).
15. **Lineage pins** (E1-REUSE-a2, :153; W1-I2 (a)-(b), :315; B2-10 (b), :400; GATE-1, :630): parents re-pinned ONCE,
    to the references of the single P9-B0 of the one gate for waves 2 and 3 (GATE-1 replaced the re-pin after wave 2
    and again after wave 3), `exes.eval` included, before the first cell; verify refuses a rule cell whose parent NAV
    came from another NAV exe. Re-stated (round 2b-ii part B2b; REPIN-REV, ledger :1003), against "to the references
    of the single P9-B0": the re-pin of record `2078f877` points Y-F0's `exes` / `exes_sha256` (incl. `exes.eval`) and
    `nav.output` (`p9-b0g-yf0-nav`), X-10's and Y-1's NAV outputs (`p9-b0g-x10-nav`, `p9-b0g-y1-nav`) and Y-1's store
    at the P9-B0 references; Y-F0's `ic.u_output` / `ic.w_output` stay the v8-era resume roots (byte-identical payloads
    to `p9-b0g-yf0-u` / `-w`, the references of record for provenance) and R-2 (`lib-v80.json`) is not re-pinned (read
    by path, `p9-b0-r2-nav`); the follow-up that pinned both (`3a1a4759`) failed `verify` and is reverted (`f8a85ad4`);
    `verify` on the pinned lineage holds (X-10, Y-1; the parent NAVs made by the pinned Release NAV exe: W1-I2 (a)) -
    `gate-logs/repin-verify.txt`, `repin-verify-3.txt`; `handoff-root-gate.md:50-69`. [re-stated, FLAGS-ROUTE
    `f8d852f3`: after the re-pin the parent spec `lib-v8ysb-gm.json` gained the two print-only opt-ins at the end of
    its `nav.flags` and nothing else (a spec edit after the re-pin, disclosed): now
    `a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a` (was `3631679c...edea`); the re-pin of record
    stays `2078f877`; no pin moved; EXE-FREEZE unaffected; `verify` on the pinned lineage holds (X-10, Y-1; the
    FIELDS-RISK standalone REFUSED line unchanged) and the fixture chain digest is unchanged -
    `gate-logs/flags-verify.txt`, `fixture-chain-7.txt`]
16. **G-P11 / COV-6**: role `train-2020-2023-lo3` has `universe.delisting.returns_applied: false` (ledger :292); root
    re-checks it before every run that reads `--risk-model` (COV-9, :354; ledger :401); it held before each M2i COV run
    (0 of 990 events true).
17. **COV-5**: every `atx-cov-v1` parameter fixed; **COV-11**: D-COV read in both directions; **COV-12**: Release is the
    producer of record; **COV-13**: no parameter moves, D-COV-EIG and its reading rule (B.8 (e)), the singular-F carry
    (B.5); **COV-14**: no rank excluded (the third pooled row = the all-ranks row), P9-V's predictions quote the
    all-ranks row and the per-rank table, ranks 00-02 printed as a finding (B.8 (e)).
18. **DEC-16** (ledger :79): info-clock vs Y-5 and mom-volman vs Y-2 / Y-1 ruled distinct; AL-CLOCK's restatement
    argument recorded in B.4.
19. **M1c-RED / C2-5** (:244, :291): Release `norm-score-v1` books are evidence after C2's fix (cleared, ledger :488);
    `rel` / `rel-avx2` are not identity surfaces.
20. **Trusted-caller flags** (`task-B2-report.md:400`, B2 review S4): `--trial` / `--history` of `atx-research-eval` are
    set only by `nav_summ`; E[max] / DSR arithmetic uses the N `nav_summ` derives; house DSR uses the bisection
    `norm_ppf` (B2-9, :350).
21. **E1-STALE** (:141): never merge under a running wave.
22. **Gross figure** (PRE-7, :519; PRE-7a, :538; C3-9, :580): `ann_mean_gross` (flag `--ann-mean-gross`, NavSpec
    `options.ann_mean_gross`; print-only, opt-in) is the arithmetic daily mean, times 252, of the book's return gross of
    trade costs, short borrow AND long financing (write-offs stay in), on each book's own previous pre-trade NAV, one
    value per scenario; it is the mean of the series `gross_sharpe` annualises. It is the figure "gross-alpha ratio"
    (P9-H, P9-MV) and G-B3 mean, read at the S2 primary scenario; it replaces the wave reader's Python sum. A "gross of
    trade costs only" figure does not exist in P9; the wording is "gross of costs and financing" (C3-9). The opt-in is
    named alike on every P9 cell and its reference (PRE-7a); if it is not merged byte-neutrally before this file is
    ruled, every gross-alpha line is cut (PRE-7, PRE-7a; B.0 placeholder).
23. **NavSpec no-op options and grids** (C3-7, :578; C3-8, :579): no P9 cell names `construction_grid`; `holdings_format`
    without `emit_holdings`, `monthly_budget` under aim-partial-v5 and `spo_alpha implied-aim` stay accepted no-ops in
    `atx.nav-spec/v1`; no P9 template or cell names any of the three except as inherited unchanged from its lineage
    parent, and a cell and its reference carry the same value (it moves `recipe_sha256`).
24. **Seal on the decide path** (C3-10, :581): a manifest without `seal.exclusive_end` is accepted there without the P13
    log line (a recorded deviation); every P9 role manifest carries the seal, so no cell reaches that branch.
25. **Cost model** (OD5-5, :465): no data purchase in P9; the fee-aware cost model stays out; modelled net Sharpe stays
    optimistic by the plan's about .06-.10.

---

## D. P9-X form (OD5-7): ruled - the Y-1 form (PRE-1)

**Ruling PRE-1 (ledger :516; settles OD5-7):** the Y-1 form (`vol-target-v1`, cap 2.0, its registered constants); P9-V
is therefore registered -- the lane argued from public lines: it leads X-10 on 1x and 4x Sharpe and drawdown, it is
already Y-F, the gap to X-10 is not significant (p .298), and only this form lets P9-V test the new store -- the
deployable levered book reads `--risk-model` and gets no history read (plan 5.3); one more trial.

The round-1 argument the ruling adopted (public lines only):
- Both forms passed X-10's rule on Y-F0 in v8: X-10 accepted (net 9.306% > 5.654%; S2 net Sharpe 1.8083 >= 1.7495;
  ledger :156), Y-1 accepted (paired dSR vs X-10 +.0215, Memmel SE .0518, p(1s) .2980; net .087990; Sharpe 1.829770;
  ledger :167); both re-derived independently (ledger :184).
- Side by side (R0-14, ledger :190): Y-F0 5.654% / 1.8495 / 1.6994 / 3.057% / 2.580%; X-10 9.306% / 1.8083 / 1.5829 /
  5.146% / 4.326%; Y-1 8.799% / 1.8298 / 1.6281 / 4.809% / 3.602% (net / 1x SR / 4x SR / vol / max DD).
- On the owner's order (Sharpe, gross return, capacity, significance; plan section 0.2) Y-1 ranks first on Sharpe
  (+.0215 at 1x), capacity (+.045 at 4x) and drawdown; X-10 first on net return (+.51%/yr). Both clear the 8.0%
  placeholder hurdle (OD5-6).
- Honest limit: Y-1 vs X-10 is not significant (p(1s) .298, ledger :167); the choice follows the owner's order on point
  estimates, not a test.

Consequences as ruled:
- **P9-V** registered and run after P9-X (B.8; COV-6); N_c reachable 70 (A.1).
- **Store** (PRE-29): P9-X and the re-based Y-1 read the Release-built v1.1 store `build-equity/p9-m2i-dcov-v11`; P9-V
  reads `build-equity/p9-m2i-dcov-covv1` (B.7, B.8); P9-X is not byte-comparable with v8's Y-1.
- **No new trial** when no P9 cell is accepted before P9-X (PRE-30; B.7).
- **OD-3**: G-B8 reads the unlevered P9-F0h (PRE-10); the levered P9-F reads `--risk-model` and gets no history read
  (plan section 5.3 :954-957); P9-V is not read either (:955-956).
- **Mechanics and verdict by hand** (PRE-11; B.7).
- **Shape note (PRE-32, ledger :597)**: E2-13 (b) (ledger :481) says "a leverage change is P9-X, kind `leverage`"; it
  was written before the form was ruled. PRE-1 and PRE-11 supersede that clause: P9-X is the Y-1 form
  (`vol-target-v1`), run by hand as a v1 cycle; no P9 cell is a v2 kind `leverage` wave; that kind stays proven by its
  fixtures and the E2-10 probes (B.7).
- **What prints under PRE-30** (PRE-34, ledger :599): the re-based Y-1 reference's figures, with v8's ledgered Y-1
  figures beside them, subject to the agreement check of B.7.

Not taken (record): the X-10 form (fixed L 2.0, kind `leverage`, `leverage: {value: "2.0"}`, `gross_match: "none"`;
pool-17 `wave_kinds.py@ea7d9dde:180-189`; value range-checked against fixed-v1 and <= `LEVERAGE_MAX` 2.0, `:93`,
`:498-505`; E2-10).

---

## E. The adoption print (once, after the last defined cell; plan section 5.2 :925, section 4.3 phase 3)

Rule (PM7-34 (2), as v8y section 7): P9-F0 is the deployable unlevered book iff its S2 net Sharpe >= 1.0 AND its
mechanics pass. Every line below prints; only that rule decides. Run with the pinned verb (`nav_summ ... --engine-stats
<exes.eval>`; its SHA-256 printed from `nav_summ_run`; B2-11). Every line carries the identity-bridge flag where C.1
says so. If P9-MV is accepted, P9-F0 and P9-F0h print side by side (PRE-10). Round 2b-ii (PRE-MVX, ledger :889): P9-F,
the deployable levered book of G-B2, is P9-X's book if P9-X is accepted, P9-X's parent being P9-F0h (B.7); a two-store
NAV (`mv-aim-v1` + `vol-target-v1` in one run) is P10 (NAV-2STORE); if P9-MV is accepted, the leverage rule is tested
on the book without the MV target, the MV book prints unlevered, and the owner's deployment choice between them is
stated in the report. (Differs from PRE-13, ledger :524, which makes P9-F P9-V if it is accepted AND passes the X-10
rule against P9-F0, else P9-X if accepted, else none; PRE-MVX names P9-X's book only; stated for the PM.) [Ruled
(MVX-PRE13, ledger :950): no replacement: PRE-13 stands whole with its base read as P9-F0h (MVX-F0H): P9-F = P9-V if
accepted AND it passes the X-10 rule against P9-F0h, else P9-X if accepted (or, under PRE-30, the re-based Y-1
reference with Y-1's verdict of record), else none; PRE-MVX's "P9-F is P9-X's book if P9-X is accepted" is that
rule's second branch.] Round 2b-ii part B1 (MVX-F0H, ledger :949): G-B1 and G-B6 keep P9-F0, the deployable unlevered
book; only the leverage cells' "P9-F0" (A.4, B.7, B.8) read P9-F0h.

| line | content | producer | flag / label |
|---|---|---|---|
| G-B1 | S2 net Sharpe of P9-F0 >= 1.0 (target 1.85); P9-F0h beside if it differs (PRE-10) | the summ of record (`net_moments` of the pinned verb; the NAV exe's `net_sharpe`, `strategy_nav_replay.cpp:1996`) | identity-bridge flag; "possibly overstated by survivorship" |
| G-B2 | S2 net annual return of P9-F (PRE-13) >= 8.0% | NAV exe scenario `ann_mean` (`:2000`); when P9-X is P9-F under PRE-30, the re-based Y-1 reference's figure with v8's ledgered Y-1 figure beside it, after the agreement check of B.7 (PRE-34) | "plan placeholder, not owner-stated"; identity-bridge flag |
| G-B3 | annual return gross of costs and financing, `ann_mean_gross` (C3-9); per unit of all-rows gross (floor X-5's 6.54%; target levered >= 10%) | NAV exe `ann_mean_gross` at the S2 primary scenario (opt-in, PRE-7a; C.22); all-rows gross as G-B9's reader | printed; cut with the other gross lines if the opt-in is not merged byte-neutrally (PRE-7) |
| G-B4 | net Sharpe at 4x NAV >= 1.50 and not lower than the parent's by > .10 (target 1.70) | `capacity_curve.csv` 4x row (`strategy_nav_v7.cpp:357-382`) | - |
| G-B5 | DSR_tot (N_tot), DSR_hand (N_tot - M; "n/a" rule of YP-2), DSR_v8 (N_c); gates nothing | pinned verb: DSR_tot = `dsr.tot.tot`, DSR_v8 = `dsr.house_v1` (`eval_report.cpp:128-175`, `:564-577`); DSR_hand = the verb row at N = N_tot - M on the ledger V from B2's post-identity redo (PRE-15; the lane names it `dsr.tot.tot_minus_m`, lane's account, unmerged, ledger :591); if that redo is not merged before this print, the prefix-rule DSR_hand (`eval_report.cpp:579-604`) labelled "prefix rule" (PRE-15) | target >= .80, printed only |
| G-B6 | cumulative paired dSR P9-F0 vs R-2, block 21, seed 20260929, 4,999 resamples; one-sided p < .05 (target .01); both p | `nav_summ --protocol v8 --bundle <R-2 nav> <P9-F0 nav>` on the pinned verb (`paired_of(..., one_sided=True)`, `nav_summ.py:205-210`) | identity bridge shared on both sides |
| G-B7 | winner's-curse-adjusted cumulative gain (AKM conditional, threshold 0, alpha .05, SE basis Memmel) beside the raw sum; steps (PRE-16 as PRE-36, ledger :601, reads it): an accepted cell that changed the unlevered book and was accepted ON its paired dSR: the v8 steps R-2, X-2, X-3, X-5, Y-S, and of P9 only P9-H, P9-M, P9-C and P9-MV when accepted, each with its ledgered dSR and Memmel SE; not steps: P9-L (protocol, accepted on mechanics; its dSR is printed beside and enters no adjusted sum), P9-X and P9-V (leverage; as X-10 and Y-1 in v8), P9-W (decides nothing); a cell decided in a degenerate pair is skipped (PRE-12) | the verb's `conditional` block, `--step LABEL=DSR,SE` rows (`eval_cli.cpp:22-23`; `eval_report.cpp:495-534`); `nav_summ` does not print it (`task-B2-report.md:211-212`), so root calls the verb directly | printed; step values below |
| G-B8 | the OD-3 read on P9-F0h (PRE-10, PRE-17; section F) | section F | identity-bridge flag; "history-readable book, not the deployable one" when P9-F0h differs from P9-F0 (PRE-10) |
| G-B9 | mechanics: all-rows gross [.90, 1.05] (x L / L_P for leverage cells), abs mean net <= .02, tau mean <= .20, p95 <= .30 | driver verify `v8-mech` (`scripts/wave_rules.py:79-101`); scaled by hand for P9-X / P9-V (PRE-11) | - |

**G-B7 step values** (each from its accepted ledger line; nothing computed here; R-2, X-2, X-3, X-5 copied in round
2b-i from the public accepted lines of the v8 ledger `platform-v8-20260929/progress.md`, "v8 :N", as PRE-36 directs):

| step | dSR | SE | source |
|---|---|---|---|
| R-2 | +.0529 | .0945 | v8 :1584-1585 (cells batch 2d, ACCEPTED, N 42; the line labels it "SE") |
| X-2 | +.0109 | .0603 | v8 :1998-2002 (X batch 1, ACCEPTED, N 51; the line labels it "SE") |
| X-3 | +.1536 | .1200 | v8 :2005-2010 (X batch 1, ACCEPTED, N 52; the line labels it "SE") |
| X-5 | +.3490 | .2011 | v8 :2019-2021 (X batch 1, ACCEPTED, N 54; the line labels it "SE") |
| Y-S | +.0800 | .1502 | ledger :115 (R0-6, ACCEPTED; the line labels it "SE"; the same driver's Y-2 / Y-5 lines, :138 / :147, label theirs "Memmel SE") |
| accepted P9-H / P9-M / P9-C / P9-MV (PRE-36) | from each cell's ledger line, after it | (same) | filled mechanically; a cell decided in a degenerate pair is skipped (PRE-12); P9-L, P9-X, P9-V, P9-W are not steps (PRE-36) |

The four v8 lines print "SE" without naming its basis, as Y-S's does; G-B7's SE basis is Memmel. [ROUND 2b-ii: the SE
printed on v8 :1585, :2002, :2010, :2021 is the Memmel (2003) SE. The v8 driver of record is `nav_summ.py`'s paired
test ("as coded in nav_summ.py. Memmel SE and Ledoit-Wolf p are reported beside it", `platform-v8-20260929/
v8-prereg.md:10-11`; `platform-v8-20260929/task-CELLS-brief.md:34-36`; carried for X by `platform-v8-20260929/
v8x-prereg.md:104-106`), run by the cycle's summ step, whose verdict file maps `paired.se` to nav_summ's `memmel_se`
at every commit of `scripts/cycle_verdict.py` in the v8 window (`fa61c67f:scripts/cycle_verdict.py:57`,
`fc802825:...:65`, `337ed421:...:70`, `0aa411a9:...:71`, and P9's `e5c219d6:...:76`: `"se": p.get("memmel_se")`);
nav_summ's console prints that value labelled "Memmel SE" (per-cell line `44a27d5d:atx-impl/tools/nav_summ.py:844`,
bundle line `:669-670`; `0b855971` `:834`, `:659`); `memmel_se` (`44a27d5d:...nav_summ.py:329-333`, Var = [2 - 2 rho
+ (SR_a^2 + SR_b^2 - 2 SR_a SR_b rho^2) / 2] / T on per-session SRs, x sqrt(252), `:29-30`) has the same body at every
nav_summ commit from `425d16db` to `44a27d5d` (the last before B2's `e27d3fda`); the other SE in nav_summ's output,
the Ledoit-Wolf delta-method SE (`lw.se`, `:404`), is in its JSON only, printed on no console line and copied into no
verdict - `git show <commit>:<path>`. Read from the driver source only: the four result lines were not opened
(blind), so that each copies `paired.se` / the "Memmel SE" print is inferred from the driver, not seen]. [ROUND 2b-ii:
the P9 ledger line round 2a's placeholder asked for, for the PM to copy (no figure): "G-B7 v8 steps of record
(PRE-36): R-2 = `platform-v8-20260929/progress.md` :1584-1585, X-2 = :1998-2002, X-3 = :2005-2010, X-5 = :2019-2021,
each with the dSR and SE its accepted line prints (`p9-prereg.md` E, G-B7 step values); the SE on those lines is the
Memmel (2003) SE of the v8 driver of record (`scripts/cycle_verdict.py` `paired.se` = `nav_summ.py` `memmel_se`, the
value nav_summ prints as "Memmel SE"); Y-S is P9 ledger :115" - this section; Appendix IV items 27-30].

Printed beside, deciding nothing (v8y section 7, carried): PBO (both readings labelled, ledger :186 precedent); year
table; net annual return; turnover; Y-F0 / P9-F0 / P9-F side by side (plan section 5.2 :908); bundles Y-F0 vs P9-F0
(the P9 increment) and R-2 vs Y-F0; each cell's verdict; P9-W's dSR; the spo-v2 prior line beside P9-MV's verdict
(PRE-24a); the power line (A.8); ALD-DIAG (C.2); the cost-model line (C.25). Round 2b-ii part B2b (PW-LAPSE-PRINT,
ledger :994): "P9-W's dSR" prints "P9-W lapsed (0), D3-Q1a: no dSR" (P9-W LAPSED (0), B.6, B.10); the bundle "R-2 vs
Y-F0" and G-B6 read R-2 by path, `build-equity/p9-b0-r2-nav` (FIELDS-SUB-A, REPIN-REV; A.4).

**Appendix A accounting block** (on every cell result and on the print). Machine part from `nav_summ.py --ledger-n
build-equity/trials.jsonl --protocol v8` (`atx-impl/tools/nav_summ.py:1196-1201`; `atx-impl/tools/
backtest_integrity.py:1255-1289` `appendix_a`, `:1292-1312` `appendix_a_v8`); split filled from the ledger:

`TRAIN construction cells <N_c>; admission trials this sprint <k> (v8 12, X hand-written 25, X-4 re-screens 9,
Y hand-written 15, P9 hand-written 0 [P9-S lapsed, W3-15, OD5-4]); mined campaigns in P9 0 (v9-mine-c1 from v8: M 110);
N_tot <n>; N_hand <n - 110>; window research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation
reads before v8: 2 (2023-2024); history reads <r>; 2025+ never read; identity-bridge flag (C.1).` P9-X under PRE-30 adds
0 and the block says so.

---

## F. The OD-3 history read (plan section 5.3; DEC-3; OD-P9-2) and OD3-TOOL

### F.1 Runbook (registered; adapted from `v8-od3-protocol.md` @ `a7671d1a`, sha256 `640cd91e...`, section 5)

- **Once.** One read event after the adoption print, on the frozen P9-F0h with Y-F0 beside it (R0-13 read nothing,
  ledger :188; plan section 5.3 :940-942; PRE-10). Nothing in any book changes afterwards. No era u, w or NAV pass runs
  before the last P9 cell is ledgered (PM7-29 choice 11, carried).
- **Not via `holdout_gate.py`.** That tool reads 2024+ (OD-1; `atx-impl/tools/holdout_gate.py:1-12`); it is never run in
  P9 and the runbook says so (PRE-21). Plan section 5.3 :941's citation of it is an error (F.2).
- **Window (PRE-17).** Eras E1 = [2015-01-01, 2017-01-01) (504 sessions) and E2 = [2017-01-01, 2020-01-01) (754
  sessions), warm-up from 2013-06 / 2015-06, score_begin 399 (v8-od3-protocol sections 3.2, 4 choice 2: 2013 unscorable,
  2014 dropped by source dates). E3 = `train-2020-2023-lo3` as built. G-B8 is decided on E1+E2 pooled (2015-2019); the
  2015-2023 pool is printed only (lower power: MDE about .40-.45 vs about .30).
- **Books and history lines** (PRE-16 .. PRE-20, PRE-31; E-41: each adds 0 to N, 1 to history reads): P9-F0h (the book
  of G-B8; PRE-10), Y-F0 (beside), R-2 (G-B8's reference), and the clean-source book (PRE-31): the last accepted book
  of the lineage that applies no rule whose `source_sample_end` is 2015 or later (theme-cov-hedge-v1, mom-volman-v1, and
  info-clock-rate-v1 / mv-aim-v1 as their registrations say), with the 12 drop-list members of Appendix III absent and
  TRAIN weights carried, no re-fit (PRE-20), printed as "clean-source book: no flagged member, no flagged rule"; it
  decides nothing (G-B8 is decided on P9-F0h). Four lines. The clean-source line differs from P9-F0h by rules as well
  as members, so the split does not isolate the members' share (PRE-31).
  Round 2b-ii part B1 (ALCLOCK-2, ledger :626): "as their registrations say" reads: `info-clock-rate-v1` registers
  2019 and is FLAGGED, so the clean-source book applies no `info-clock-rate-v1`; `mv-aim-v1` registers 2010, clean
  ("Contamination flags" below). OD3P-1 (ledger :624) and ABS-1 (:643): "absent" reads: a dropped member is treated on
  every date exactly as the engine treats a member with no data on that date: it adds nothing to the composite and its
  mass stays in its theme ("missing-or-unoriented-mass-stays-in-theme"); a theme whose members are all dropped is
  treated as the engine treats a theme with no present member; the pinned TRAIN weights, signs, theme blocks and the
  runner's verify are unchanged (theme-erc-v1 and ic-shrink bases accepted), so the carry tool zeroes no weight and
  the clean-source book keeps each theme's mass spread over fewer members: it is not "the same book minus twelve
  signals", and the print says so. Route: the IC exe's opt-in `--absent-members <file> --absent-members-sha256 <hex>`
  (capability `absent-members-v1`; flag absent = bytes unchanged), which the cycle passes on history roles only
  through `fit.carry.drop`, refusing at plan time when the IC exe lacks the capability (contract
  `od3p-absent-contract.md`). Lane facts: ABS (the C++; APPROVE `5dae4f69`, ledger :738; merge `898bf120`,
  `integration-log.md:700`) and OD3P (`fit.carry.drop`; APPROVE `97a8157f`, ledger :690; merge `7f6f1c75`,
  `integration-log.md:715`: on the real IC exe p9-2w the plan carries the two flags on E1's u / w only, and a fake
  without the capability is refused at plan time) are merged and APPROVE in the gate head `06dc9e5a`, so the
  clean-source line has its producer and ABS-1's fallback "not read: no producer" does not bind on that ground; the
  IC exe's flag-absent identity is the gate's (`integration-log.md:700`, "IC flag-absent passes -> gate"; part B2).
  Round 2b-ii part B2a (FINDING FIELDS-GONE, ledger :978; FIELDS-SUB-A, :985): the R-2 line's TRAIN reference is
  `build-equity/p9-b0-r2-nav`, re-based by the fields substitution v10 -> v15 (A.4 item 1); "not readable: its TRAIN NAV
  cannot be reproduced on the frozen build" (FIELDS-SUB's path B) does not apply; fields v15 is whole on disk (84
  payloads), so the B7 era fields argv below (Y-F0's v15 receipt with the reuse options removed) names no absent dir;
  the v9 / v10 / v13 / v14 / v15a dirs are gone and nothing is rebuilt from atx-db (A4-DB, OD3-DB).
  Round 2b-ii part B2b: the IC exe's flag-absent identity at the gate HELD: the Release IC exe p9-3f (which carries
  ABS's `absent-members-v1`) reproduced Y-F0's u and w with every payload byte-identical to M1d's references, flag
  absent (A.4 item 2; RELCACHE-ALLOW). REPIN-REV (ledger :1003): R-2's TRAIN fields dir is v15 (`26fee5ce`) and its
  TRAIN w `mega-v8-r1w-train-std-v80-1` (`train_combined.json` `bbbf6f2b...cd98`, not re-based): the runbook's R-2
  `[UNKNOWN]` slots (`handoff-root-gate.md:143-145`); `lib-v80.json` is untouched and stale (A.4), so an OD-3 era spec
  for R-2, if OD-3 runs, is built from `p9-b0-r2-nav`'s recipe at the OD-3 step, the PM ruling then. FIELDS-RISK
  (ledger :1005; A.4): Y-F0's spec built alone resolves fields v13; whether Y-F0's history line (built from its TRAIN
  spec) meets that refusal is not ruled; stated for the PM. [Ruled: OD3-FIELDS-RISK, ledger :1008: a refusal at F.1
  step B6 on the template chain's `baseline_fields` v13 is an F.1 stop condition (hand back before any era read; the
  PM rules then; nothing pre-decided); moot if OD3-DB stays silent ("not read in P9")]
- **Weights.** TRAIN admission, signs and weights carried back unchanged; no re-admission, no re-fit (PM7-29 choice 1,
  carried). Rules estimated at apply time re-estimate on each era's own returns (theme-cov-hedge: the hedge is 0 in
  each era's first 252 decisions, h_t being 0 before decision 253, stated in the OD-3 print, PRE-26); mom-volman's
  schedule is TRAIN-fixed, so on 2015-2019 the parent's masses hold and the rule is inert (ALCOMB-4); info-clock reads
  each era's `ea_days_since`. Each book's L carried; no gross calibration on history; gross printed (choice 6).
- **Predictions, registered here before the read** (plan section 5.3 :945-947; lit section 5.3 caveat 2), each printed
  against its outcome:
  - P9-F0h vs Y-F0: the mechanism sign of every accepted P9 cell that is active on history (P9-H: vol ratio < 1; P9-C:
    turnover share up, gross up; P9-M: inert, no effect predicted; P9-L: labels on history come from each era's own
    label role, whose delisting stage is empty before 2018, v8-od3-protocol section 2). Round 2b-ii part B1
    (ALCLOCK-1, ledger :625): the two books of the turnover-share prediction (P9-F0h and Y-F0 on each era) name
    `--info-clock-print` alike, as P9-C and its reference do; its producer is the NAV exe's `summary.json`
    `info_clock.turnover_share` (B.4); if the opt-in cannot be byte-neutral the block is cut from the reference and the
    prediction is printed "no producer". [Part B2a: the opt-in HELD at the gate (B.4: only the `info_clock` leaves
    added; `gate-logs/nav-identity-yf0.txt`), so the block stays and the prediction has its producer.]
  - P9-F0's history S2 net Sharpe = its TRAIN value + P9-W's dSR (B.6, PRE-23; the read is on P9-F0h, which equals
    P9-F0 unless P9-MV is accepted: "Open for the PM (round 2a)" item 8 [read: decided by PRE-39, ledger :604,
    restated below; "Rulings applied in round 2b-i and 2b-ii" item 8]); "lower than on 2020-2023" (PM7-28's
    registered expectation, carried). Round 2b-ii (PRE-39, ledger :604; root M4's OD3P item 7,
    `root-wave3-merge-report.md:489`, and :736): the registered prediction reads "history S2 net Sharpe of P9-F0h = its
    TRAIN S2 net Sharpe + P9-W's dSR"; this line's "P9-F0's" is stale text (PRE-40 form; B.6 states PRE-39). Part B2a:
    P9-W LAPSED (0) at the gate (D3-Q1a DIFFERENT; B.6, B.10), so this prediction has no P9-W dSR to fill; how it
    prints is not ruled (stated for the PM in B.6). [Ruled (PW-LAPSE-PRINT, ledger :994): this line prints "not
    produced: P9-W lapsed (0), D3-Q1a" with no number; the formula stands as registered; the OD-3 prediction table has
    one "not produced" row; nothing else of F.1 changes]
  - ERC (review-x5 via lit section 5.3): vol ratio .80-.87 and return ratio .90-1.0: printed "not read" (PRE-18; it
    would need X-5's pre-ERC parent as a fifth history line).
  - Producers: NAV exe `ann_vol`, `ann_mean`, `net_sharpe` per era (`strategy_nav_replay.cpp:1996-2002`); pooled rows
    `nav_summ --pool` (`nav_summ.py:1331-1337`); turnover as B.2.
- **Contamination flags** (lit section 5.2 (b); plan section 5.3 :948-951; PRE-19, PRE-28, PRE-31): every member of the
  book and every P9 rule carries `source_sample_end` (Appendix III, the ruled list). P9 rules: theme-cov-hedge-v1 2017
  and mom-volman-v1 2016 (both registrations; confirmed by PRE-FLAGS; mom-volman-v1 keeps its registered 2016, PRE-31
  item 10); info-clock-rate-v1 and mv-aim-v1 [ROUND 2b-ii: info-clock-rate-v1 2019 (Bowles, Reed, Ringgenberg and
  Thornock 2024, JF 79(5), sample 1990-2019): FLAGGED under PRE-19 / PRE-31 (ALCLOCK-2), already in the `rules` of
  `scripts/specs/p9/od3/flagged_members.json` (sha256 `9aa6fb596a4edd0b2a8028a337678161efdfa50535d61dbe9c18fdff9f56f5ff`,
  `:34`, `:39`); mv-aim-v1 2010, verified (Menchero, Wang and Orr 2011, testing window to July 30, 2010): clean, not
  flagged; it is still in that file's `rules_pending` (`:41-42`) and root removes it and re-pins the file at the
  runbook's P0 step, before the read (MV-FLAG-P0, ledger :833) - `scripts/specs/p9/rules/info-clock-rate-v1.json:12-14`;
  `scripts/specs/p9/rules/mv-aim-v1.json:12-13`; `task-AL-CLOCK-report.md:272-273`; `task-COV-MV-report.md:226-231`;
  `root-wave3-merge-report.md:485`, `:543`, `:628-631`, `:1044`].
- **Coverage by year** (lit section 5.3 caveat 1): `atx-impl/tools/era_data_audit.py` (in root; coverage by field and
  year, delisted share, mask exits, first valid date per candidate; opens no return). 2015-2017 tests a different book
  (FINRA short interest, FTD, Russell proxy from 2018; 13F from 2013q2; options from 2012).
- **Not readable**: any NAV that reads `--risk-model` (Y-1, P9-X, P9-V, P9-MV, risk-target). No per-era risk store is
  built in P9 (PRE-10; a P10 note): if P9-MV is accepted the read is on P9-F0h.
- **Steps** (v8-od3-protocol section 5, adapted; tooling of F.2): B1 projection; B2-B5 era base, scan / repair, lo3 and
  dlret label roles (same argv, era dates); B7 era fields with P9-F0h's field list (fields v15, 84 fields, the
  engine-kind rows included; not v10) [ROUND 2b-ii: the era fields argv is the `command` list of P9-F0h's TRAIN
  fields receipt `start.json` (for fields v15, Y-F0's: `build-equity/train-2020-2023-lo3-fields-v15-run/start.json`,
  the run of manifest `26fee5ce`) with exactly three values replaced, `--role <era lo3 dir>`, `--role-sha256 <its
  manifest SHA-256>`, `--output <era fields dir>`, and the reuse options `--reuse X`, `--reuse-sha256 X`,
  `--reuse-hardlink` removed; everything else byte-identical (the `-c` program text, `--fields` with its 84 names in
  the receipt's order, every source and pin, `--mgr13f-stage`, `--max-rss-mib`, `--max-seconds`); the runner wrapper
  keeps the receipt's `limits` and `bindings` except the role binding (the era's), the reuse manifest binding (removed)
  and its run dir; self-check first with the TRAIN values (equal to the receipt's list minus the reuse options, else
  STOP); any other difference from the receipt is a STOP (F.1 "fields argv beyond the allowed keys"); a run refused at
  the 600 s cap is a STOP before any return and the PM rules the staging (the TRAIN v15 dir was built with `--reuse`
  of `-v15a`); P9-F0h's receipt stays UNKNOWN until the last accepted cell (the runbook's marker; v15 unless a cell
  added a field) - `task-OD3P-report.md:106-115`; `p9-od3-runbook.md:99-116`] (differs from OD3-PAR, ledger :935,
  which fixes the era build's field list now as Y-F0's fields manifest `26fee5ce`, fields v15's 84 names, Python the
  producer of the two A4-P10-FLIPS fields; stated for the PM) [Ruled (F1-FIELDS, ledger :963): no conflict in value:
  the era fields list is P9-F0h's TRAIN fields receipt's; no registered P9 cell adds or changes a field (P9-L changes
  labels, P9-H / P9-M / P9-C / P9-MV rules, P9-W the fit, P9-X / P9-V leverage), so P9-F0h's receipt is Y-F0's fields
  v15 (`build-equity/train-2020-2023-lo3-fields-v15-run/start.json`, manifest `26fee5ce`, 84 names; Python the
  producer of the two A4-P10-FLIPS fields) whatever the cells decide; OD3-PAR's "fixed now" stands and the runbook's
  UNKNOWN marker is resolved to v15; the cells root re-checks at the OD-3 step that P9-F0h's spec names that fields
  dir (a difference = STOP before any era read)]. Beside it (OD3-DB, ledger :939, an owner decision): the
  era steps read stage paths under `C:/atx/atx-db/`, which the owner's rule closes; until the owner answers, OD3-PREP
  is not dispatched, and with no answer by the OD-3 step the read is recorded "not read in P9: era inputs under atx-db
  (A4-DB, OD3-DB)" and G-B8 "not evaluated" (stated; nothing here decides it); B6 the roles specs with `fit.carry` (the
  clean-source book's spec as a carry base, PRE-31); lock and plan; P1-P7 pre-read checks (pins equal TRAIN lo3's;
  P9-F0h / Y-F0 / R-2 / the clean-source book reproduce their TRAIN NAVs byte for byte on the frozen build; score_begin 399; eras end <= 2020-01-01; >= 1,000 lo3 members per scored
  E1 session; audit printed; K1 plan-only in caps; the plan lists exactly E1, E2, E3 with the carry); R0 protocol line
  (`research_cycle.py ledger-protocol`; adds 0); R1 era runs to NAV; R2 mechanics only, E3 daily SHA-256 = the TRAIN
  series; R3 summ (pooled history lines); R4 the G-B8 statistics.
- **Stop conditions** (each before any return is opened; the PM rules): v8-od3-protocol's list (pin difference,
  identity failure, score_begin != 399, era past 2020-01-01, unreviewed factor breaks, an E1 session under 1,000
  members, fields argv beyond the allowed keys, a plan with other eras or a carry refusal, K1 over caps, a book reading
  `--risk-model`, the last cell not ledgered, an R2 refusal or E3 SHA mismatch) plus: an exe or `ucrtbase.dll` record
  differing from the freeze build (C.6, C.8); a print without the identity-bridge flag; OD3P not merged and reviewed
  (PRE-21). Round 2b-ii (OD3P-S, ledger :691, the T2 follow-up's two guards; the PRE 2b-ii list, :699): the cycle
  refuses at plan time (exit 2, before any step) (S-a) a carry output recorded for other eras, another admission or
  another anchor role (`scripts/research_roles.py:402-418`; test `test_research_cycle_od3.py:617`) and (S-d) a stale
  `<fit.output>.pending` carry output, dir or file (`scripts/research_roles.py:384-395`; `carry_weights.py:77`
  `PENDING_SUFFIX`; test `:650`); each refusal is a stop (`p9-od3-runbook.md:141-146` B6, `:155-162` P7, `:232-242`
  the stop list). T2 (`nav_summ --pool-one-sided`, the pooled pair's one-sided p from the engine verb's `--one-sided`;
  OD3P follow-up APPROVE `4a7372e2`, ledger :822; merge `a2cf7e78`, `integration-log.md:732`) prints its verdict
  labelled "bundle rule, not G-B8"; G-B8 is decided by this section's pass rule (OD3P-T2V, ledger :765;
  `p9-od3-runbook.md:209-213`) - `task-OD3P-report.md:271-284`.
- **Pass rule (G-B8)**: S2 net Sharpe of P9-F0h on history (E1+E2) > 0 AND paired dSR P9-F0h vs R-2 on history > 0;
  both p printed (one-sided from the verb's `--one-sided`, `eval_cli.cpp:110-111`). A fail is reported, never re-run;
  the freeze wording is per OD5-1 (v).

### F.2 OD3-TOOL: ruled - port lane OD3P (PRE-21, ledger :527)

1. **What "holdout_gate.py under v8x section 10's runbook" refers to (record).** v8x section 10 (`v8x-prereg.md:202-211`)
   has two bullets: `holdout_gate.py` is the OD-1 hidden-block gate (2024+); the OD-3 read is a separate "2013-2019 era
   shards, H-1" read. `holdout_gate.py` exists in root as `atx-impl/tools/holdout_gate.py` (not under `scripts/`), prints
   `pass_2024` / `pass_2025_onward` (`:6`) and must never run in P9 (PRE-21). The OD-3 runbook is `v8-od3-protocol.md`
   section 5 (ruled PM7-29), on the xpre branch only.
2. **What the read needs, and where it is (record, round 1).**

| need | in root (file) | only on `feat/platform-v8-xpre-20261002` |
|---|---|---|
| era roles, label roles, fields builds | `atx-engine/tools/prepare_recent_research.py`, `atx-impl/tools/repair_role_factor_breaks.py`, `atx-engine/tools/prepare_research_fields.py` | - |
| era window checks | `atx-engine/tools/era_pool.py:40-43` | - |
| roles cycle (per-era u / w / NAV, fields_dir) | `scripts/research_roles.py` (H-1) | T4 `roles[].label_dir`; T1 `fit.carry` (+109 lines in `research_roles.py`) |
| carried weights | - | T1 `atx-impl/tools/carry_weights.py` (new, sha256 `5aebb3a7...`) |
| coverage audit | `atx-impl/tools/era_data_audit.py` | - |
| pooled rows and pooled paired dSR | `nav_summ.py --pool / --pool-ids / --pool-reference` (`:1331-1337`) | - |
| pooled one-sided p + verdict | the verb has `--one-sided` (`eval_cli.cpp:110-111`); `nav_summ` uses it only in the bundle path (`nav_summ.py:205-210`) | T2 `nav_summ --pool-one-sided` (Python `paired_stats(..., one_sided=True)`) |
| pooled DSR_tot (history adds no trial) | refused today: `nav_summ.py:1367-1368`; the verb's `--history` reaches `house_v1` only, not `tot` (`eval_report.cpp:566-575`) | T3 `--dsr-total` with `--pool` |
| history ledger lines (E-41) | `atx-impl/tools/backtest_integrity.py:684` `history_read_lines`; `research_cycle.py ledger-protocol` | T5 test |
| tests | - | `atx-impl/tools/test_od3_tooling.py`, `scripts/tests/test_research_cycle_od3.py` |
| per-era risk store | none; not built in P9 (PRE-10) | none |
| the clean-source book with the 12-id drop list | none (`carry_weights.py` carries W unchanged; no drop list) | none: OD3P adds it (PRE-20, PRE-31) |

   Round 2b-ii part B1 (OD3P-1, ledger :624; ABS-1, :643): the table stands as round 1 wrote it; its last row's need
   is met in root by OD3P's `fit.carry.drop` (the carry tool stops zeroing weights; merge `7f6f1c75`) and by lane ABS's
   IC exe capability `absent-members-v1` (`--absent-members <file> --absent-members-sha256 <hex>`; merge `898bf120`),
   the meaning of "absent" being OD3P-1's (F.1 "Books and history lines").

3. **Why not a merge (record).** `git merge-tree` at round 1's root head: CONFLICT in `atx-impl/tools/nav_summ.py`; E2
   turned `scripts/research_cycle.py` into a shim (`make_cycle` in `scripts/cycle/cli.py`), and B2's deletion removes
   T3's targets and `nav_summ`'s Python `paired_stats` (T2's call); `research_roles.py` and `carry_weights.py` merge
   cleanly.
4. **Ruled route (PRE-21).** A small port lane OD3P (Python, one pool, after E2 and the B2 redo are in root; POOL-MAP,
   ledger :586: pool-21, `feat/p9-od3p-20261004` at `5c57c1ef`, run id `p9-od3p-20261004`): cherry-pick
   `carry_weights.py`, the T1 / T4 `research_roles.py` hunks and their tests; the T1 refusal into
   `scripts/cycle/cli.py`; T2 re-implemented on the engine `eval` verb (`--one-sided`), no Python statistics; T3 dropped;
   the flagged-member drop list (PRE-20) with the 12 ids and the clean-source book's spec accepted as the carry base
   (PRE-31, required carry); `v8-od3-protocol.md` and the branch copy of `task-XPRE-report.md` committed as records;
   `atx-impl/tools/holdout_gate.py` is never run in P9 and the runbook says so. The stale `research_cycle.py` text on
   fitter abbreviations is corrected by whoever next edits that file (C3b or OD3P; D2R-2 (a), ledger :547). Reviewed
   and merged before the OD-3 read; not on the path of the cells; if it slips, OD-3 waits for it. P10 note: carried
   weights belong in the C++ composition CLI. Round 2b-ii part B1 (OD3P-1, ABS-1): "the flagged-member drop list" is
   OD3P-1's absence (F.1), with OD3P writing the list-file contract `od3p-absent-contract.md` and the C++ in its own
   lane ABS (ABS-1), which had to be merged and APPROVE at the gate, else the clean-source line prints "not read: no
   producer"; lane facts: OD3P APPROVE `97a8157f` (ledger :690), merge `7f6f1c75` (`integration-log.md:715`), and its
   follow-up APPROVE `4a7372e2` (ledger :822), merge `a2cf7e78` (`integration-log.md:732`); ABS APPROVE `5dae4f69`
   (ledger :738), merge `898bf120` (`integration-log.md:700`); all in the gate head `06dc9e5a`, so neither "if it
   slips" clause binds.

---

## G. Freeze gate checklist (plan section 4.4, as amended by G-P11 and SQL-8; the PM ticks it)

- [ ] 1. Book: G-B1 floor met by P9-F0 (P9-F0h beside if it differs, PRE-10); G-B2 by P9-F (PRE-13) against 8.0% (label
      "plan placeholder, not owner-stated"); G-B4 floor; G-B6 one-sided p < .05; G-B9 mechanics; G-B5 and G-B7
      printed; G-B1 / G-B2 / G-B8 carry the identity-bridge flag (OD5-1 (i), ALD-9).
- [ ] 2. History: OD-3 read once under F.1 on P9-F0h (PRE-10); predictions printed against outcomes; the clean-source
      line printed (PRE-31); G-B8 met on E1+E2 (PRE-17). If G-B8 fails: not production, nothing re-run, the owner
      decides deployment.
- [ ] 3. Platform: G-P1..G-P9 and G-P11 ticked, each with its evidence row in the integration log (G-P11, ledger :224).
      Round 2b-ii part B1 (C3-11, ledger :622): G-P5's named exception, the Python `sign_pm7_35`, stayed through C3b
      and was to be retired by lane SIGN, or to stay in the freeze report (SIGN P10) if SIGN was not merged and APPROVE
      at the wave-3 gate; SIGN is merged and APPROVE (`e2844926`, ledger :824): root's identity on the ledgered wave
      results preceded the deletion commit `50a43599`, merged at `338e6a7c` (`integration-log.md:734`), after which
      `sign_pm7_35` and `screen_decision` are gone from `scripts/wave_rules.py` and the freeze state holds 0 G-P5 rows
      for P9 lanes (`FROZEN_PAIRS` still names `sign_pm7_35`, so a reintroduction is detected;
      `root-wave3-merge-report.md:834`); so no surviving copy of that rule is named in the freeze report on C3-11's
      ground; the G-P5 tick itself is the PM's at the freeze. Part B2b (GP-TICKS, ledger :1001; the gate's G-P table):
      G-P5 stays PARTIAL in the freeze print ("four pairs unlisted, P10 retirement", IC-S; the guard green, 0 rows
      left for P9 lanes); G-P1 (the v2 wave route end to end on a real cell) is "measured at P9-H" and ticked by the
      cells root at the first P9 v2 wave with its evidence row; G-P2, G-P3, G-P4, G-P6, G-P7, G-P8, G-P9 and G-P11
      ticked at the gate, each with its evidence row (`root-gate-report.md:997-1011` at `d22246f6`;
      `integration-log.md:902-909`).
- [ ] 4. Accounting: the Appendix A block of every P9 result complete; N_c <= 71, K_a, M, N_tot <= 252 (A.1); every
      void, undefined, lapsed and not-a-new-trial (PRE-30) cell logged with its count (0 or 1).
- [ ] 5. Housekeeping: every P9 pool released; lane branches merged by SHA; no dirty path in the research pathspec.
- [ ] 6. G-P10 printed with its evidence (a)-(e), landed / partial / P10; it does not gate (SQL-8). [ROUND 2b-ii: at
      the gate (step 7, after the re-pin; the Release store exe p9-3f `0eeac21b...3818`; every catalog DB outside the
      root, `build-equity/research-store/` absent): the real-tree catalog digest of the gate head
      `adbcd348b5d82a1fd4f357274a34bbf387391cf82154a961e701bad39a0dd474` (bounded root run `p9-b0g-cat-2-run`; files_seen
      33,699, verified 4,190, declared 84, skipped 29,468), reproduced by a second from-scratch catalog; the fixture
      chain digest `a50fdcf9c9c83bdac0d41ff368666da31578ebb4407bbc4498175a63014f100c` reproduced (also after
      `3a1a4759` and after `f8a85ad4`); `verify --pins`: ok 1,620, declared 169, stale 220, missing 630, unresolved
      1,512; render identity 435 checked, 0 mismatches; `classes.json`
      `5bc27ba489f24a0d1c8c42c8c29927c8d1bde6bf63cd5993f285a5b084086557`;
      G-P10 (a), (b), (c), (e) landed, (d) PARTIAL as M6 states; the X-5 cache identities are M6b's items 5 / 6
      (FIELDS-X5) - `gate-logs/realcat-gate.txt`, `realcat-gate-verify.txt`, `fixture-chain-{4,5,6}.txt`;
      `root-gate-report.md:971-1013` (at `d22246f6`); `handoff-root-gate.md:110-118`].
- [ ] 7. EXE-FREEZE held from the wave-3 re-pin to the freeze (or each break re-based first); `ucrtbase.dll` records
      equal across every cell, and the two D-COV stores' record unchanged (C.6, C.8; CRT-2). Round 2b-ii part B1
      (GATE-1, ledger :630): "the wave-3 re-pin" is the ONE re-pin (W1-I2 (b), `exes.eval` included) of the one gate
      for waves 2 and 3, made after its ONE P9-B0 (R-2, Y-F0 and Y-1 re-based; the substitution list written first);
      EXE-FREEZE starts at that re-pin; the re-pin's exe table (tag, SHA-256, source commit) is part B2's ("Round 2b-ii
      record"). Part B2b: the re-pin of record is `2078f877`, EXE-FREEZE from it (C.8); the exe table is in "## Round
      2b-ii record".
- [ ] 8. Verdict wording for a pass: "passed with the identity-bridge flag"; `al-data/atx-db-request.md` handed to the
      owner with the freeze report (OD5-1 (iv)-(v)).
- [ ] 9. Owner items listed, not acted on: OD5-2 (deterministic `pow` / `tanh` / `exp2` and its one-time re-pin; M2I-HK),
      OD5-3 (main / origin main), OD5-5 (data purchase), production leverage (OD5-7), the WQ theme (WQ-1, OD5-4).

---

## H. Rulings applied (round-1 item -> the ruling that decided it)

- H1 P9-X form -> PRE-1 (Y-1 form; settles OD5-7); stores PRE-29; no new trial if nothing accepted before it PRE-30.
- H2 P9-L shape -> PRE-2 (by hand, v1 cycle of a template root writes; every flag named in B.1).
- H3 P9-L gross match -> PRE-3 (`pm6-6-engine`, one zero-trial calibration run); the child's parent after it PRE-27.
- H4 P9-L 30-day exposure -> PRE-4 (stated, bounded by ALD-DIAG's 2 unclassified exits; no truncation code).
- H5 P9-L verdict -> PRE-5 (by hand, re-derived by a second agent before the ledger line).
- H6 P9-H registration -> PRE-6 (registered unless q_lit < .05; q_lit .0671: registered; PM note :515); Q5 on Y-F0
  ALCOMB-10.
- H7 gross-alpha producer -> PRE-7, PRE-7a (`ann_mean_gross`, opt-in, byte-neutral, alike on cell and reference), C3-9
  (definition), C3-6 (neutrality shown at root's merge).
- H8 P9-M parent -> PRE-8 (the last accepted, also a P9-H hedge book; template description corrected and re-pinned).
- H9 P9-MV store pin -> PRE-9 (`risk_model` / `risk_model_sha256` params with `x-flag`); store W3-10 / PRE-29.
- H10 P9-MV and OD-3 -> PRE-10 (no per-era risk store; P9-F0h).
- H11 P9-X and P9-V shape -> PRE-11 (by hand, second derivation, B2-11 check by hand).
- H12 degenerate pair, dSR-decided cells -> PRE-12 (point dSR > 0 AND mechanics; G-B7 skips); P9-V see Open item 2.
  [read (round 2b-ii part B1): Open item 2 -> PRE-33 (B2-13's fallback for P9-X alone; P9-V under PRE-12 with the
  line "pair degenerate: ..."); H33]
- H13 P9-F -> PRE-13.
- H14 lapses -> PRE-14 (P9-C, P9-W as COV-7 for P9-MV; order unchanged).
- H15 DSR_hand -> PRE-15 (verb row in B2's redo; else prefix rule labelled).
- H16 G-B7 steps -> PRE-16.
- H17 G-B8 sample -> PRE-17 (E1+E2 decides; 2015-2023 printed).
- H18 OD-3 books -> PRE-18 (four lines; ERC "not read"); the fourth line as PRE-31 defines it (clean-source book).
- H19 contamination rule -> PRE-19 (primary source; unverified = flagged); verification PRE-28; list PRE-31.
- H20 unflagged book -> PRE-20 (TRAIN weights carried, no re-fit); 12-id drop list and flagged rules PRE-31.
- H21 OD3-TOOL -> PRE-21 (port lane OD3P; POOL-MAP pool-21).
- H22 re-bases -> PRE-22 (R-2 and Y-F0 at the wave-3 P9-B0); the re-based Y-1 on the v1.1 Release store PRE-29.
- H23 P9-W -> PRE-23 (`pm6-6-engine`; B.6 formula, subject to the prereg review).
- H24 spo-v1 / spo-v2 read -> PRE-24, PRE-24a (prior of record; print-only carries for COV-MV).
- H25 "about 1" band -> PRE-25 ([.90, 1.10]).
- H26 P9-H on history -> PRE-26 (hedge 0 in each era's first 252 decisions, stated).

Round 2b-ii part B1 (PRE-2BI-REST, ledger :951): one line per ruling applied after H26 that decided a round-1 or
round-2a item ("Open item N" = "Open for the PM (round 2a)" item N), or a round-2b-ii part-A question:
- H27 the parent after a hand-run cell -> PRE-27 (C2-6: calibration run `-cal`; the child names the plain recipe at
  L-prime, no calibrate flag; B.1, A.4).
- H28 flag verification early -> PRE-28 (doc lane PRE-FLAGS, `p9-prereg-flags.md`; Appendix III).
- H29 stores of P9-X and P9-V -> PRE-29 (Release `atx-risk-v1.1` store for P9-X and the re-based Y-1, Release
  `atx-cov-v1` for P9-V, same exe and step group; not byte-comparable with v8's Y-1; A.4, B.7, B.8).
- H30 P9-X when no P9 cell is accepted before it -> PRE-30 (the re-based Y-1 reference is the cell of record, N_c +0;
  A.6, B.7).
- H31 the flag list and the clean-source book -> PRE-31 (FINAL FLAG LIST adopted; 12-id drop list; a flagged rule is
  treated like a flagged member; Appendix III, F.1).
- H32 E2-13 (b) vs PRE-1 / PRE-11 (Open item 1) -> PRE-32 (no P9 cell is a v2 kind `leverage` wave; B.0, B.7, D).
- H33 B2-13 vs PRE-12 (Open item 2) -> PRE-33 (B2-13's fallback for P9-X alone; P9-V under PRE-12 with the "pair
  degenerate" line; B.7, B.8, C.11).
- H34 PRE-29 vs PRE-30 (Open item 3) -> PRE-34 (the re-based Y-1 reference's figures print, v8's beside, after the
  agreement check; B.7, D, E G-B2).
- H35 PRE-14 vs E2-17 / C3-3 (Open item 4) -> PRE-35 as D3-2 reads it (the cells wait for D3's two guards; P9-W
  lapses iff D3W's walk-forward deliverables or D3's kind wiring are not merged and APPROVE then; B.0, B.6).
- H36 G-B7 steps (Open item 5) -> PRE-36 (of P9 only P9-H, P9-M, P9-C, P9-MV when accepted; v8 steps R-2, X-2, X-3,
  X-5, Y-S; E).
- H37 PRE-27 on P9-X / P9-V (Open item 6) -> PRE-37 (the calibration clause binds P9-L only; A.4).
- H38 realised-vol producer (Open item 7; Appendix II item 2) -> PRE-38 (the NAV exe's C++ `ann_vol`; no line cut;
  B.2, B.3, B.5).
- H39 the book P9-W refits (Open item 8) -> PRE-39 (P9-F0h; the prediction on P9-F0h; A.4, B.6, F.1).
- H40 stale `manifest_note` in pinned registrations (C3b's finding) -> PRE-40 (not edited; stated beside; B.2, B.3;
  the form of every later restatement).
- H41 P9-X's parent (NAV review M-1) -> PRE-MVX (P9-F0h; one store per argv; NAV-2STORE P10; A.4, B.7, B.8, E).
- H42 the other "P9-F0" of B.7 / B.8 (part A question 2) -> MVX-F0H (read as P9-F0h where they name P9-X's or P9-V's
  parent, reference, comparison, argv base or side-by-side book; G-B1, G-B6 keep P9-F0; A.4, B.7, B.8, E).
- H43 P9-F after PRE-MVX (part A question 3) -> MVX-PRE13 (PRE-13 whole on the P9-F0h base; A.4, E).
- H44 B.5's registration SHA-256 as a prereg pin (part A question 1) -> COVMV-PIN (YES; the `printed_only` / "ex-ante"
  sentences are not edited; B.5).
- H45 the form of the engine-fit identity (D3-Q1) -> D3-Q1a (`provenance.engine_fit` removed, canonical JSON
  SHA-256; admission files whole-file; B.6; the outcome is part B2's).
- H46 `mv-aim-v1` with `info-clock-rate-v1` (root M4 question 1) -> COMPOSE-1, COMPOSE-1a (no composition test; the
  gate's store-backed parse check, else P9-MV's own preflight; a refusal lapses P9-MV at 0 before any read; wording
  COMPOSE-PRE; B.5, B.10).

---

## Appendix I. SHA-256 recomputed (round 2a, at root `327c86b2`: every value equal to round 1's)

| file | sha256 | note |
|---|---|---|
| `scripts/specs/p9/rules/theme-cov-hedge-v1.json` | `55f25f2fd1976fab2138b1130da5c6968addbd4e333b75fbf9c78330cf2b8ed0` | - |
| `scripts/specs/p9/rules/mom-volman-v1.json` | `8a7222fa7d99b853abb102f87538fadf2ea4a17b8d22b698a913b64c58e2b493` | - |
| `scripts/specs/p9/templates/theme-cov-hedge.json` | `7c6778bbd6c5759e3c431bae3cf301700240e672ea8406926b549927688dfb15` | = registration `template_sha256`; re-pin pending (B.2) |
| `scripts/specs/p9/templates/mom-volman.json` | `7e9b26c1bc4a6453447ccbf9fee2612ecb50a293330f239cfc93d035e45db6fb` | = registration `template_sha256`; re-pin pending (B.3) |
| `scripts/specs/p9/diagnostics/al-comb-q5-style-variance.json` | `721889d19ad5fe19cb09546530e30c04fdc7d4f2012f42c63bbfe72005a8ba74` | - |
| `scripts/specs/p9/candidates/russell_recon.json` | `509e1834af4e8f9b184ab7bb37d15c9058d37a4085c97897ccaf48dca9f088ae` | status dropped |
| `scripts/specs/p9/blocked/hf_crowd.json` | `3cbd5169e5495f70e88a5160f03a62924966cc79bffb9934a3de54d4b5fec4de` | since `30b9c9d2` (ALD-12 (c)); was `9b62a71a...` |
| `scripts/specs/p9/blocked/lazy_prices.json` | `49b2292ab84847516138d09bc769cf6dd6245973d1530fd630bbc4b945b285b0` | since `30b9c9d2`; was `97fe4f73...` |
| `atx-impl/strategies/fund_industry_ic_v8ysb.json` | `41010b0bbc662922e6e873057e1fc39d046d6f01a0a5124dd8a601c25a1308f5` | = Y-F0 spec pin |
| `scripts/specs/v8/lib-v8ysb-gm.json` | `1e3ec118273cb52d7248568011e7dc24a211e7204c25b1116f5a5bd7deaa8626` | Y-F0 |
| `scripts/specs/v8/y-vol-target.json` | `4e190f5434dfc13aced1459721cde476faf3560ef99aa41e05bfe492e2f141b4` | = v8y section 14 pin |
| `scripts/specs/v8/y-vol-target-y-1.json` | `faffee1795ba500fbebaa5a52e2449acaf02c92f7ee972011a267a52ba265f77` | Y-1 |
| `scripts/specs/v8/x-leverage-L2.0.json` | `ebe68e6e4de3ad5b0dc5b86e38d8f749cf77d3bd637c55d5453e9c1e4a674975` | = ledger :156 `ebe68e6e` |
| `v8-od3-protocol.md` @ `a7671d1a` | `640cd91ecfc0dd8e6b0b3a3da13e87e52b6e361c42fc2347d099b9da6976c9bb` | xpre branch only (round 1) |
| `atx-impl/tools/carry_weights.py` @ `f68058e2` | `5aebb3a7ce8ff3f845d3151bf44d5b1648a14705be4421b3fa0768a3db33227c` | xpre branch only (round 1) |
| DSL `rank(russell_recon)` / `rank(hf_crowd)` / `rank(lazy_prices)` | `2aad0b86...` / `1881f8ce...` / `bcbe2653...` | = registrations (round 1) |
| `.superpowers/sdd/platform-p9-20261003/p9-prereg-flags.md` | `7a089e5233cb69a329bd09f42f2dfff48f638101a421751f03b2291aa2d94bbe` | the list PRE-31 adopted (Appendix III) |
| `build-equity/p9-m2i-dcov-v11/manifest.json` | `397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7` | copied from `handoff-root-m2i.md`, not recomputed (blind) |
| `build-equity/p9-m2i-dcov-covv1/manifest.json` | `ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3` | copied from `handoff-root-m2i.md`, not recomputed (blind) |

Round 2b-ii part B2b (the re-pin of record `2078f877`, pins only; PRE-40 form: the rows above stand as round 2a
recorded them): `scripts/specs/v8/lib-v8ysb-gm.json` is now
`3631679cd18f1c6a63a6a5dc96cdbe6ee7669b81b8979f529544f1b237d9edea` [re-stated, FLAGS-ROUTE `f8d852f3`:
`a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a`; the re-pin of record stays `2078f877`; no pin
moved; EXE-FREEZE unaffected; X-10 and Y-1 bytes unchanged, they inherit the two flags],
`scripts/specs/v8/x-leverage-L2.0.json` `46e9d6c2fdbf42e362518118938bc89af038e3c09d7519d660aaa936554fd0bf` and
`scripts/specs/v8/y-vol-target-y-1.json` `f4186573a41de3b67cdd815976639ed2123e5c6f43353516c61150d92dbb50d6` (each the
bytes at `2078f877` = at the code head `f8a85ad4`); `lib-v80.json` (R-2) stays `306a070b...3a21`, untouched
(REPIN-REV); `mv-aim-v1.json` stays `4b4af6e2...80b4` (COVMV-PIN); `info-clock-rate-v1.json` read at the re-pin =
`7801c198...a152` (W1-I2) - `handoff-root-gate.md:50-62`; `root-gate-report.md:925-939` (at `d22246f6`).

## Appendix II. Inconsistencies found across the lane files and the plan (status after round 2a)

1. Plan section 5.3 :941 routes OD-3 "through `holdout_gate.py`"; that tool is the OD-1 2024+ reader. RESOLVED by PRE-21
   (never run in P9; the runbook says so).
2. ALCOMB-9 (b) names `atx-research-eval` as the producer of P9-H's realised vol and gross-alpha ratios; the verb prints
   neither. Gross-alpha half RESOLVED by PRE-7 / PRE-7a / C3-9 (`ann_mean_gross`). Realised-vol half STILL OPEN: no
   ruling names a producer the verb lacks ("Open for the PM (round 2a)" item 7). [Round 2b-ii part B1: realised-vol
   half RESOLVED by PRE-38 (ledger :603): the producer is the NAV exe's C++ scenario `ann_vol` at the S2 primary
   scenario, one value per NAV dir; the ratio cell / parent is the reader's division of two C++-printed numbers; no
   line is cut (B.2, B.5); P9-M's sleeve line keeps the eval verb (B.3)]
3. The theme-cov-hedge template promises an "ex-ante" vol ratio that only Q5 could print. RESOLVED by ALCOMB-9 (b) with
   Q5's result (not produced; ALCOMB-10): cut before registration (B.2).
4. ALCOMB-9 (a) "the P9 parent is neither" holds only if P9-H is rejected. RESOLVED by PRE-8.
5. `brief-COV-MV.md` (closed `params_schema`, `params: {}`) leaves no route for `--risk-model` past E2's guard. RESOLVED
   by PRE-9.
6. Carry PRE item "an imputed terminal return is never guard-checked" predates S2-FIX1 (1). RESOLVED (S2-FIX1 (1),
   ledger :325; text only, B.1).
7. Plan section 5.2 :916-917's PM6-6 list omits P9-L and P9-W. RESOLVED by PRE-3 (P9-L) and PRE-23 (P9-W).
8. The v2 driver has no PM7-34 (3) rule and no scaled mechanics; the v8 Y-1 template's raw `--vol-target` flag would be
   refused under v2. RESOLVED by PRE-11 (P9-X, P9-V by hand). New tension with E2-13 (b): "Open for the PM (round 2a)"
   item 1. [Round 2b-ii part B1: the tension RESOLVED by PRE-32 (ledger :597): PRE-1 and PRE-11 supersede E2-13 (b)'s
   clause "a leverage change is P9-X, kind `leverage`"; no P9 cell is a v2 kind `leverage` wave (B.0, B.7, D)]
9. The verb's `--history` reaches `house_v1` but not `tot`. RESOLVED by PRE-21 (T3 dropped; DSR is not a G-B8 leg).
10. P9-MV accepted would make P9-F0 unreadable on history. RESOLVED by PRE-10 (P9-F0h).
11. Plan section 5.2's N_c / N_tot ceilings assume P9-S's cell and 10 strings; the full N_c ceiling is +2.13% on E[max]
    against v8's 62, above lit section 5.2 (c)'s "about 2%". STILL OPEN as a note: no ruling addresses it; nothing gates
    on it (A.1, A.3). [Round 2b-ii part B1: status unchanged: still no ruling addresses it (ledger to :965); a note
    only, gating nothing; the reachable N_c (<= 70, +1.90% on the factor, A.3) is under the "about 2%", the registered
    ceiling 71 (+2.13%) above it]
12. v8y's DSR_hand used `dsr_total.dsr_at`, deleted by B2 `598d2199`. RESOLVED by PRE-15.
13. The v8 OD-3 runbook builds era fields v10; P9-F0 reads fields v15 with engine-kind rows. STILL OPEN until OD3P's
    runbook gives the era fields argv (F.1 placeholder; PRE-21). [Round 2b-ii: RESOLVED: OD3P's runbook gives the era
    fields argv (part A, F.1 "Steps" B7; `p9-od3-runbook.md:99-116`), and F1-FIELDS (ledger :963) fixes the list as
    P9-F0h's TRAIN fields receipt = Y-F0's fields v15 (manifest `26fee5ce`, 84 names), the runbook's UNKNOWN marker
    resolved to v15; the cells root re-checks at the OD-3 step that P9-F0h's spec names that fields dir (part B1)]
14. The blocked registrations' bytes changed during round 1 (`30b9c9d2`). RESOLVED by ALD-12 (c) (Appendix I holds the new
    SHA-256).

## Appendix III. Contamination flags: the ruled list (PRE-31, ledger :573)

Source: `p9-prereg-flags.md` (doc lane PRE-FLAGS, PRE-28; 182 lines; sha256
`7a089e5233cb69a329bd09f42f2dfff48f638101a421751f03b2291aa2d94bbe`), whose FINAL FLAG LIST PRE-31 adopts as written. Rule
(PRE-19, binding): a member is flagged when the main sample of its PRIMARY source (the paper whose construction it
implements) ends in 2015 or later; unverified = flagged. Evidence per member (URL and quote): that file, Tables A and B.

- **Flagged (10):** bac, smax, smax5, sv_flow, qmj_safety, q5_eg_f49g, ear_mom_12m, op_rd, day_rev_freq, value_composite.
- **Flagged-unverified (2):** k8_intensity, ftd_fail.
- **Drop list of the clean-source book (12) = the 10 + the 2** (PRE-20, PRE-31).
- **Clean (55):** 36 by publication year (ep, cfp, fcfp, ebit_ev_f49, net_payout, sp, rd_me, roa, accruals_f49, fscore,
  asset_growth_f49, noa_f49, issuance_xbrl, issuance_vendor, ear, ind_mom_12_1, within_ind_mom, high_52w, si_ratio,
  seasonality_same_month, iv_rv_spread, opex_at_f49, ins_opp_buy, pct_accruals, fip_id, si_low_io, inst_persist,
  div_season, vol_beta, so_wang_rev, mom_turn, ea_uvol, dato, fscore_hbm, stio_trade, deal_target) + 19 of the 31
  verified (opbe, cbop_f49, roe_q, dtc, si_change, ea_overdue, dtc_slow, nt_late, iv_vol_of_vol, bm, gpa_f49,
  res_mom_12_1, mom_12_1, ind_adj_rev_5, nincr, res_mom_ind, inst_best_ideas, earn_surprise_comp, season_y2_5).
  Count: 55 + 10 + 2 = 67; every library id in exactly one list.
- **PRE-31 on the lane's open items:** `value_composite`, `op_rd` (the plan itself names Novy-Marx-Medhat), `smax` stay
  flagged; `ftd_fail` and `k8_intensity` stay flagged-unverified; `si_change`, `res_mom_12_1`, `res_mom_ind`,
  `inst_best_ideas`, `earn_surprise_comp` stay clean (the earlier paper defines the construction; a later paper that
  extends the sample is not the primary source under PRE-19). Change from round 1's expectation: `value_composite` is
  flagged (the sector-neutral rank composite is Israel-Laursen-Richardson 2021's construction, sample to 2020).
- **P9 rules:** theme-cov-hedge-v1 2017 (DMRS, "1963/07 - 2017/06"; flagged), mom-volman-v1 2016 (Cederburg et al.,
  samples "end in December 2016"; flagged; keeps its registered 2016, a registration is not edited after the fact,
  PRE-31 item 10); vol-target-v1 not read on history; info-clock-rate-v1 and mv-aim-v1 [ROUND 2b-ii:
  info-clock-rate-v1 2019 (Bowles, Reed, Ringgenberg and Thornock 2024, JF 79(5), "Our sample period begins January
  1990 and goes through 2019"; flagged, ALCLOCK-2); mv-aim-v1 2010 (Menchero, Wang and Orr 2011, "the testing window
  is 4,103 trading days (from April 15, 1994 to July 30, 2010)"; clean, verified; removed from `flagged_members.json`'s
  `rules_pending` at P0, MV-FLAG-P0, ledger :833) - `scripts/specs/p9/rules/info-clock-rate-v1.json:12-14`;
  `scripts/specs/p9/rules/mv-aim-v1.json:12-13`; `task-COV-MV-report.md:226-231`; F.1 "Contamination flags"]. A flagged RULE is treated like a flagged member: the clean-source book
  applies none (PRE-31; F.1).

## Appendix IV. Placeholders (index)

**Filled in round 2a** (round-1 tag -> value and source): Q5 numbers (B.2; gate report item 5, ledger :510); ALD-DIAG
(C.2; item 2, :510); S2R-DIAG (C.9; item 6, :510); the 31 members' verification and the final list (Appendix III;
`p9-prereg-flags.md`, PRE-31). Partly filled, the rest kept as ROUND 2b: P9-MV block (B.5: store pin from
`handoff-root-m2i.md`, :582; the spo read PRE-24a); P9-X store pin (B.7: same source); P9-V predictions (B.8: D-COV
tables, item 4 (d), :582; two of three pooled rows); CRT record (C.6: the two stores, CRT-2, `handoff-root-m2i.md`);
leverage-pair rho (C.11: item 3, :510); G-B7 step values (E: Y-S, ledger :115); P9 rules' `source_sample_end` (F.1 and
Appendix III: the two registered rules, PRE-31).

**ROUND 2b, in order of appearance** (what closes it - who delivers it). Round 2b-ii part A (PRE-2BII-PAR): "(filled,
part A)" = filled from a merged lane's block; "(part A: registration commit)" = re-bracketed `[ROUND 2b-ii: filled at
the cell's registration commit by root - Appendix IV item N]`; an item with no tag is part B's (the gate). Round 2b-ii
part B2a (PRE-2BII-B2A): "(filled, part B2a)" = filled from the gate records at `65ce28f0`; "(part B2b)" = left for
the gate's final hand-back:
1. A.4 R-2 re-based NAV reference dir and SHA-256 - root, wave-3 P9-B0 (PRE-22). (filled, part B2a: `p9-b0-r2-nav`,
   FIELDS-SUB-A)
2. A.4 Y-F0 re-based u, w, NAV reference dirs and SHA-256 - root, wave-3 P9-B0 (PRE-22). (part B2b: GATE-IDX)
   (filled, part B2b: `p9-b0g-yf0-{u,w,nav}`; REPIN-REV's two roles)
3. A.4 re-based Y-1 reference dir and SHA-256 (on `p9-m2i-dcov-v11`) - root, wave-3 P9-B0 (PRE-29, PRE-30). (filled,
   part B2a: `p9-b0g-y1-nav`; the X-10 re-base `p9-b0g-x10-nav` beside C.11)
4. B.0 exe paths, SHA-256, build tags, source commits - root, wave-3 P9-B0 re-pin commit (W1-I2 (b), B2-10 (b), EXE-CUR).
   (part B2b: the re-pin; the step-1 table is provisional) (filled, part B2b: the re-pin of record `2078f877`)
5. B.0 C3-6 byte neutrality of `--ann-mean-gross` - root M2j, C3 part-A merge (C3-6; a miss cuts the gross lines).
   (filled, part B2a: holds, cited from M2j)
6. B.0 eval mode - root, wave-3 P9-B0 u receipt (PATH-1). (part B2b: the u receipt is behind GATE-IDX) (filled, part
   B2b: research-fast)
7. B.1 P9-L template `label-terminal.json` and SHA-256 - root writes it from B.1 (PRE-2). (part A: registration commit)
   (filled, P9-L registration `0bba6c89`: `scripts/specs/p9/templates/label-terminal.json`
   `7483f4b10513d9000cfc89036c3f8f7f49626a9dd9c57999bcce11a258754219`, inheriting the parent's `nav.flags`; the run
   recipe `label-terminal-cal.json` `7783967471f5b06dc1407d4425605156f7a3e647f28edd2a3ca5f2fb1a989d37` (`6ea79e5a`);
   B.1 "Shape")
   [PRE-40 restatement, P9L-WBUDGET, `f560f741`: template SHA-256
   `1d15513e74851e776a7e40d538f528690fbab04c4a92bb8afdcc240a805e3170`; run recipe SHA-256
   `1f00e19ec9dc3092ebe570b9645700910062881835dc9cd391623cff76c00062`; only w's two memory
   admission budgets become 3584 MiB; B.1 and the plan amendment govern the resume.]
8. B.1 P9-L parent / paired reference dirs and SHA-256 - root, wave-3 P9-B0 (= item 2). (part B2b: = item 2) (filled,
   part B2b: = item 2; the re-pinned parent spec `3631679c`) [re-stated, FLAGS-ROUTE `f8d852f3`:
   `a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a`; the re-pin of record stays `2078f877`; no pin
   moved; EXE-FREEZE unaffected]
9. B.1 P9-L fit mode - root, wave-3 P9-B0 (ALCOMB-7; D3's C++ sleeve). (filled, part B2a: flag-absent)
10. B.2 P9-H re-pinned template SHA-256 - root's registration commit. (part A: registration commit)
    [PRE-40, filled `818e0750`: template `1f56c19428b7b62de9788e37ec21bf20a78f122e411fb8fb948a0cc0347ec082`,
    registration `6b4c3b0d3eebdf5752e41394d316766e75a1b0df22552bc85449740ae36f2516`; B.2 bracket governs.]
    [PRE-40, R15-H-READY / R15-H-RESTART, `708ab6ce`: new ready-template SHA-256
    `5e9eae0aa660e9de3e538a1e659b16234e36597cd4e6dc07eecc956740d7f168`; only requires closure in this clone.
    Original registration remains byte-for-byte; B.2's adjacent amendment gives attempt2 binding/delta.]
11. B.2 P9-H "ex-ante" cut from the template description - root, same commit (ALCOMB-9 (b)). (part A: registration
    commit)
    [PRE-40, filled `818e0750`: removed exactly `ex-ante and `; B.2 records the before/after text.]
    [PRE-40, R15-H-RESTART, `708ab6ce`: the ready clone preserves that exact description; no further prediction edit.]
12. B.2 P9-H fit mode - root, wave-3 P9-B0 (ALCOMB-7). (filled, part B2a: flag-absent)
13. B.3 P9-M re-pinned template SHA-256 (parent, PRE-8 description) - root's registration commit. (part A:
    registration commit)
    [PRE-40, filled `a11bfb9d`, R15-M-READY: B.3 above records the full new template/rule/manifest
    hashes, exact parent/text/prerequisite delta and original-blob preservation. Items14-15 stay
    KEPT and flag-absent; sleeve remains in-sample/full-TRAIN.]
14. B.3 P9-M sleeve line keep / cut - lane D3 (ALCOMB-6); root re-pins if cut. (filled, part A: KEPT; D3W's block)
15. B.3 P9-M parent fit mode - root, wave-3 P9-B0 (ALCOMB-7; D3). (filled, part B2a: flag-absent)
16. B.4 P9-C block (rule, row, registration and template SHA-256, parent conditions, producers, C3-4, DEC-16 argument,
    `source_sample_end`, C3-5 probe, fit mode) - lane AL-CLOCK, its review and root's merge; fit mode at the wave-3 P9-B0.
    (filled, part A; open: the parent's fit mode, part B; the re-pinned template / registration SHA-256, registration
    commit) (fit mode filled, part B2a: flag-absent; ALCLOCK-1 held, B.4)
    [PRE-40, filled `0ca14564`, R15-C-READY: B.4 above records the exact parent/prerequisite/pin
    closure and new template/rule/manifest hashes; original registration remains preserved at
    `afba2530`. Fit stays flag-absent; both print flags inherited once, NAV-only native route.]
17. B.5 P9-MV block (merged and APPROVE, registration and template SHA-256, `source_sample_end`, C3-4, PRE-9 / COV-13 (c) /
    PRE-24a carries, fit mode) - lane COV-MV, its review and root's merge; fit mode at the wave-3 P9-B0. (filled, part A;
    open: the parent's fit mode and COMPOSE-1, part B; the re-pinned template / registration SHA-256, registration
    commit) (fit mode and COMPOSE-1 filled, part B2a: flag-absent; COMPOSE-1a ACCEPT, store-backed)
    [PRE-40, filled `0afd08b4`, R15-MV-READY: B.5 records the exact closed parent/
    prerequisite/pin delta and complete new template/rule/manifest hashes. The
    parent is fixed-rate accepted H, compatible flag-absent fit, completed phases
    reused; both print flags inherited once. Historical X-5 identity CUT stands.
    R15-MV-LAUNCH read/source/runtime contract is committed before measurement.]
18. B.6 P9-W block (manifest shape, fit mode, per-date weights producer, template and SHA-256) - lane D3. (filled, part
    A, from lanes D3 and D3W; open: D3-Q1a's outcome, part B; the generated cell spec's and the manifest's SHA-256,
    registration commit) (D3-Q1a's outcome filled, part B2a: DIFFERENT, P9-W LAPSED (0))
19. B.7 P9-X hand spec and SHA-256 - root writes and commits it (PRE-11). (part A: registration commit)
20. B.7 rho of the P9-X pair - root, wave-3 references, before the read (B2-13). (part B2b: needs the re-based Y-F0)
    (filled, part B2b: on the references; the cell's own pair, if P9-X runs, is the cells root's before its read)
21. B.8 P9-V hand spec and SHA-256 - root writes and commits it (PRE-11). (part A: registration commit)
22. B.8 rho of the P9-V pair - root, before the read (B2-13). (part B2b) (filled, part B2b: a cell-time value, the cells
    root's before P9-V's read)
23. B.8 (a) producer of the book's sigma_hat bias on the two stores - root, before P9-V's read. (filled, part B2a:
    B8A-PROD, the cells root; the fields-v10 consequence stated for the PM)
24. B.8 (e) third pooled eigen row "without structurally null ranks" - root M2j, D-COV-EIG (COV-13 (b)).
25. C.6 `ucrtbase.dll` record at the wave-3 P9-B0 - root. (part B2b: re-recorded at the Y-F0 runs) (filled, part B2b)
26. C.11 rho of the P9 pairs' references at the wave-3 P9-B0 and of each cell's pair - root (B2-13). (part B2b)
    (filled, part B2b: the three reference pairs; each cell's own pair is a cell-time value)
27. E G-B7 step R-2 - the PM or root, from the v8 ledger's accepted line, into a P9 ledger line. (values copied in
    round 2b-i, PRE-36; part A: the SE basis answered from the v8 driver source and the P9 ledger-citation line written
    in E for the PM to copy)
28. E G-B7 step X-2 - as 27. (as 27)
29. E G-B7 step X-3 - as 27. (as 27)
30. E G-B7 step X-5 - as 27. (as 27)
31. F.1 info-clock-rate-v1 / mv-aim-v1 `source_sample_end` - lanes AL-CLOCK and COV-MV. (filled, part A)
32. F.1 era fields argv from P9-F0h's fields manifest - lane OD3P's runbook (PRE-21). (filled, part A; P9-F0h's
    receipt stays UNKNOWN until the last accepted cell, as the runbook marks it) [read (part B1): the UNKNOWN marker
    is resolved to Y-F0's fields v15 (`26fee5ce`) by F1-FIELDS, ledger :963; F.1]
33. Appendix III info-clock-rate-v1 / mv-aim-v1 `source_sample_end` - as 31. (filled, part A)

---

## Open for the PM (round 2a)

(superseded by the section below; kept as round 2a wrote it)

Contradictions between rulings found while applying them, and objections; none is resolved in the text above (each is
pointed to where it bites). One line each.

1. E2-13 (b) (ledger :481: "a leverage change is P9-X, kind `leverage`") vs PRE-1 + PRE-11 (P9-X is the Y-1 form,
   `vol-target-v1`, run by hand through `research_cycle.py`): P9-X is no v2 kind `leverage` wave (B.7, D).
2. B2-13 (:503: a leverage pair with 1 - rho < 1e-6 is decided on its non-paired acceptance lines only; it names the
   P9-X / P9-V pairs) vs PRE-12 (:524: a dSR-decided cell in a degenerate pair is decided on point dSR > 0 AND
   mechanics): P9-V is both, and the two give different verdict rules (B.8, C.11).
3. PRE-29 (:569: the re-based Y-1 on the Release v1.1 store is not byte-comparable with v8's Y-1) vs PRE-30 (:570: that
   re-based reference is P9-X's cell of record with Y-1's ledgered verdict): which figures print for P9-X and, if it is
   P9-F, for G-B2 (v8's ledgered Y-1 or the re-based reference's), and whether reading the re-based reference's
   statistics is a look, is unstated (B.7, E).
4. PRE-14 (:524: P9-W lapses if D3 is not merged and APPROVE when the cells start) vs E2-17 (:542) and C3-3 (:536: no
   cell runs before D3 merges): cells cannot start before D3 merges, so P9-W can lapse only if D3 merges without the
   walk-forward kind; which D3 deliverable decides the lapse is unstated (B.6).
5. PRE-16 (:526: G-B7 steps = R-2, X-2, X-3, X-5, Y-S "plus the accepted P9 cells") vs its own v8 list, which leaves out
   the accepted leverage cells X-10 and Y-1 and the protocol cell B0c: whether an accepted P9-L (protocol), P9-X or
   P9-V (leverage) is a G-B7 step is unstated (E).
6. PRE-27 (:543) names P9-X and P9-V among the hand-run cells that follow C2-6's calibration rule, but both are
   registered `gross_match: "none"` (PM7-20 reading B, carried by PRE-1; Y-1 template "NOT gross matched"), so its
   calibration clause has no object there; only its "plain recipe, no calibrate flag" part applies (A.4).
7. Objection to ALCOMB-9 (b) (:448), applied as written in B.2 / B.5: it names `atx-research-eval` as the producer of the
   realised vol ratio, but the verb prints no vol (`eval_report.cpp:41-46`); by ALCOMB-9's own principle ("a
   registration may not promise a figure nothing produces") the line is cut unless the producer is changed to the NAV
   exe's C++ `ann_vol` (`strategy_nav_replay.cpp:2001`); PRE-7 / C3-9 moved only the gross-alpha half.
8. PRE-23 (:528: the OD-3 prediction "history S2 net Sharpe of P9-F0 = its TRAIN value + P9-W's dSR", P9-W's parent
   being the in-sample P9-F0 after P9-MV) vs PRE-10 (:522: the history read is on P9-F0h): if P9-MV is accepted the
   prediction names a book OD-3 does not read (B.6, F.1).

## Rulings applied in round 2b-i and 2b-ii

Round 2b-ii part B1 (PRE-2BI-REST, ledger :951; PRE-2BII-B1, :959). The ruling that decided each item of "Open for the
PM (round 2a)" above, and where it is applied. The other rulings of rounds 2b-i and 2b-ii are listed with their places
in the header (the "ROUND 2b-i PARTIAL" line and the part A and part B1 paragraphs) and in H27-H46.

1. E2-13 (b) vs PRE-1 + PRE-11 -> PRE-32 (ledger :597): PRE-1 and PRE-11 supersede E2-13 (b)'s clause "a leverage
   change is P9-X, kind `leverage`"; P9-X is the Y-1 form, run by hand as a v1 cycle; no P9 cell is a v2 kind
   `leverage` wave; that kind stays proven by its fixtures and the E2-10 probes. Applied: B.0, B.7, D (round 2b-i);
   Appendix II item 8 (part B1).
2. B2-13 vs PRE-12 -> PRE-33 (ledger :598): B2-13's fallback (the non-paired acceptance lines only) applies to P9-X
   alone, whose acceptance has no paired line; P9-V is a dSR-decided cell: in a degenerate pair PRE-12 governs it, and
   the print adds "pair degenerate: the two stores give the same book to within 1e-6 of correlation"; one rule per
   cell, fixed before the pair's rho is measured. Applied: B.7, B.8, C.11 (round 2b-i); H12, H33 (part B1).
3. PRE-29 vs PRE-30 -> PRE-34 (ledger :599): the cell of record is the re-based Y-1 reference of the wave-3 P9-B0; its
   figures are printed for P9-X and, if it is P9-F, for G-B2, with v8's ledgered Y-1 figures beside them; reading them
   is not a look PROVIDED they agree with the ledgered Y-1 figures at the ledger's printed precision (one unit in the
   last printed place of S2 net Sharpe and of net annual return), else nothing further is read, root stops and the PM
   rules; the check is made before the adoption print. Applied: A.6, B.7, D, E G-B2 (round 2b-i).
4. PRE-14 vs E2-17 / C3-3 -> PRE-35 as D3-2 reads it (ledger :600, :629): the P9 cells start only after lane D3's two
   guards (the C3-3 alias guard, the E2-17 `inputs.*` guard) are merged and APPROVE; P9-W lapses at 0 iff, at that
   moment, D3W's walk-forward deliverables (the walk-forward C++ fit; the v3 weights with
   `WeightsV3.SingleRowEqualsV1Bytes`) or D3's kind wiring (the `walk-forward` kind out of SCHEMA_ONLY) are not merged
   and APPROVE; part B of the split (D3W-PB) gates no cell. Applied: B.0, B.3, B.6 (round 2b-i); the lane facts in B.6
   and B.10 (part A); the remaining P9-W condition is D3-Q1 / D3-Q1a's engine-fit identity at the gate (B.6; part B2).
5. PRE-16's G-B7 steps -> PRE-36 (ledger :601): a G-B7 step is an accepted cell that changed the unlevered book and
   was accepted ON its paired dSR: of P9 only P9-H, P9-M, P9-C and P9-MV when accepted; not P9-L (protocol), P9-X and
   P9-V (leverage), P9-W (decides nothing); the v8 steps stay R-2, X-2, X-3, X-5, Y-S with their ledgered dSR and
   Memmel SE. Applied: E (round 2b-i: the G-B7 row and the step values of R-2, X-2, X-3, X-5; part A: their SE basis
   from the v8 driver source and the P9 ledger-citation line).
6. PRE-27's calibration clause on P9-X / P9-V -> PRE-37 (ledger :602): the clause (C2-6, run recipe `-cal`) applies to
   P9-L only; P9-X and P9-V are `gross_match: "none"` (PM7-20 reading B), so for them only "the cell of record is the
   plain recipe, no calibrate flag" applies. Applied: A.4 (round 2b-i).
7. The objection to ALCOMB-9 (b) -> PRE-38 (ledger :603), upheld: the producer of the realised vol ratio (P9-H, P9-MV;
   the OD-3 vol predictions) is the NAV exe's C++ scenario `ann_vol` at the S2 primary scenario, one value per NAV dir;
   the ratio cell / parent is the reader's division of two C++-printed numbers; no line is cut; P9-M's sleeve line
   keeps the eval verb as its producer (ALCOMB-6). Applied: B.2, B.3, B.5 (round 2b-i); Appendix II item 2 (part B1).
8. PRE-23 vs PRE-10 -> PRE-39 (ledger :604): P9-W's parent and paired reference is P9-F0h (the last accepted book that
   does not read the risk model), the book OD-3 reads; the registered prediction reads "history S2 net Sharpe of
   P9-F0h = its TRAIN S2 net Sharpe + P9-W's dSR"; P9-W keeps its place in the order; if P9-MV is accepted nothing in
   P9 measures the walk-forward loss of the mv-aim book, which gets no history read either (PRE-10). Applied: A.4, B.6
   (round 2b-i); F.1 (part A; its pointer to this item answered in part B1).

## Round 2b-ii record

Round 2b-ii part B2b (PRE-2BII-AGENT, ledger :927; r12-post-gate "PRE-2BII" part B), from the gate's final records:
`handoff-root-gate.md` (FINAL), `root-gate-report.md` and `integration-log.md` at docs head `d22246f6`, the ledger to
:1006. Line citations of `root-gate-report.md` in part B2a are to the file at `65ce28f0`, in this part to `d22246f6`.
No statistic is copied (rho as value and flag only, B2-13). The earlier lines these records contradict are re-stated
beside each (PRE-40 form) and listed in the header's "ROUND 2b-ii part B2b" paragraph.

**Heads.** Gate head (code) `06dc9e5a` (`Merge lane SQL4 at 44a76937 ...`; SQL4-CUTOFF-2: checks green, SQL4 in the
head; `root-gate-report.md:8-11`).
The re-pin of record: `2078f877` (ONE pins-only code commit over `06dc9e5a`: `scripts/specs/v8/{lib-v8ysb-gm,
x-leverage-L2.0,y-vol-target-y-1}.json`, +17 / -11; W1-I2 (b), B2-10 (b), B2-11). A follow-up pins-only commit
`3a1a4759` (REPIN-UW, REPIN-R2: Y-F0's `ic.u_output` / `ic.w_output` -> `p9-b0g-yf0-u` / `-w`; `lib-v80.json` fields
-> v15 and `nav.output` -> `p9-b0-r2-nav`) was refused at `verify` (the cycle resolved the u / w pins as pending
attempt roots `p9-b0g-yf0-{u,w}-1`; `lib-v80.json`'s 70-name `fields.list` against v15's 84 rows) and is reverted by
`f8a85ad4` (REPIN-REV), the code head: `git diff 2078f877 f8a85ad4 -- scripts/specs/v8/` is empty; `verify` on the
pinned lineage holds again, line for line as at `883be9f2` (`gate-logs/repin-verify-3.txt`). Docs head `d22246f6`.
GATE-1 DONE; known-red set EMPTY; trial ledger 133 lines `27e40f9f` (0 trials at the gate; head `5a3ef9d9`, N 62);
`build-equity/research-store/` absent; next free build tag `p9-3h`; no permission refusal by gate agent 4.

**Exe table of the re-pin** (source commit = gate code `06dc9e5a`, receipt Source `5f48a25d`, a docs commit; Release
p9-3f = producer of record, `build-equity-rel/bin`, receipt `build-equity/mega-p9-3f-receipt.json`; Debug p9-3g,
`build-equity/bin`, receipt `build-equity/mega-p9-3g-receipt.json`; 27 / 27 + 27 / 27 `disk = receipt` at the gate's
end; EXE-FREEZE from `2078f877` over NAV, IC, eval, composition, admission and store; the 16 test targets:
`root-gate-report.md` step 1) - `handoff-root-gate.md:28-48`:

| exe | Release p9-3f SHA-256 | Debug p9-3g SHA-256 |
|---|---|---|
| atx-equity-strategy-targets (NAV; pinned `exes.nav`) | `255b58acedf7df82a1caa672ff30a58c317a8f0dccf7dbe7d3b7393b91063565` | `5dced610e204ba860bd2c8c8da5c3c6bd58a1ef98c242bec352a6b64d8762a8f` |
| atx-equity-strategy-ic (IC; pinned `exes.ic`) | `ccd54db605f67fef1aa480050a6c785ffe179dae2f77e978dd6d01a5c117d705` | `adffe7421781145a9718e67be05b46038b89a46311f26390eeb3312da236d56d` |
| atx-research-eval (pinned `exes.eval`) | `e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c` | `4791019131c0d74603b83cb772ef027ec8641167fd95844c4674e75bdf39893b` |
| atx-research-composition | `04db0d67c1563184edd6fe8ff66796877c8db20239d5cfb423fa78e047df16f2` | `212f8b9941f0be705ebac138c03e83279fc863ab12051f00dd09501db7eed98e` |
| atx-research-admission | `f53d9277e4ad2e3915343ccca032df3f0098a1a1cd7b4d832b640f5f7437063f` | `411af33a0def5a6c28983dbf1ffc2fbc324694e8ac2e6b24401b27b693c9fa26` |
| atx-research-store | `0eeac21bb95677d8da5268ca829b88dc668521a4c9f625e0fbbde92f96fb3818` | `a54955e965ee2aa035e66cc9ed668a16246c089684a53515431dea428b605c73` |
| atx-research-fields | `883c5b74a95731eb310cfd30cb1ab39ee102e077ab2d7318bd5db941f486584a` | `1c670d86a0fdcae6a86b0eff56a184902d1eed8b244f8af6ab31425e18185fac` |
| atx-equity-strategy | `f883f81f0fbb928cb8fe2e8b1931841e0eef058e7b6398810e10de68cb0495a4` | `47d341c2541e792873ec5d3db0b739af6de61e9962ba46667c755a1ca56ca9a4` |
| atx-equity-strategy-risk | `0c110534dd2df43e995040a07e5368eac788166b5e55cc2e0e2fa5e088be586b` | `6696da719374777aefb49ce9e6b4707187549dff9b838e0c2338d21e852b47a1` |
| atx-equity-strategy-mine | `daf889437983628ccbc375e76a078d3decf079a9d3a79b6cca79cbcc4a063eba` | `4dee20571d4c4481bcf175d8bb688574ba95c60ae29d08b4bb474849f7945412` |
| atx-impl | `71f0c6311aaffffaa48f4c221c18c94e009a3b7ebc5a45798baff009d75bbdb9` | `7095d4477dd7fee80ead2d9bbdb516a726ed3ab943beed95ac1dc5335af63e55` |

**The re-pinned specs** (`2078f877` = `f8a85ad4`) - `handoff-root-gate.md:52-62`:

| spec | SHA-256 before -> after | pins |
|---|---|---|
| `lib-v8ysb-gm.json` (Y-F0, the lineage parent) | `1e3ec118...8626` -> `3631679cd18f1c6a63a6a5dc96cdbe6ee7669b81b8979f529544f1b237d9edea` | `exes` -> the Release NAV / IC + `exes.eval`; `exes_sha256` {eval, ic, nav}; `nav.output` -> `p9-b0g-yf0-nav`; `ic.u_output` / `ic.w_output` unchanged (the v8-era resume roots, REPIN-REV) |
| `x-leverage-L2.0.json` (X-10) | `ebe68e6e...4975` -> `46e9d6c2fdbf42e362518118938bc89af038e3c09d7519d660aaa936554fd0bf` | `nav.output` -> `p9-b0g-x10-nav`; `locked.reference_cell` -> `p9-b0g-yf0-nav/summary.json` `826d5ec6...8f38` |
| `y-vol-target-y-1.json` (Y-1, P9-X's reference spec) | `faffee17...5f77` -> `f4186573a41de3b67cdd815976639ed2123e5c6f43353516c61150d92dbb50d6` | `nav.output` -> `p9-b0g-y1-nav`; `--risk-model` -> `p9-m2i-dcov-v11` `397366473b3a...23c7` (PRE-29); `locked.reference_cell` -> `p9-b0g-x10-nav/summary.json` `deabe71b...0bd` |

[re-stated, FLAGS-ROUTE `f8d852f3`, beside the table's `lib-v8ysb-gm.json` row (the row stands as recorded):
`a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a` (`nav.flags` + `--ann-mean-gross
--info-clock-print`, nothing else); the re-pin of record stays `2078f877`; no pin moved; EXE-FREEZE unaffected; the
X-10 and Y-1 rows unchanged (both inherit the two flags once)]

Not edited: `lib-v80.json` (R-2, `306a070b...3a21`; read by path; its v10 fields pin and v8 NAV output are stale,
stated in the substitution list, PRE-40 form), `mv-aim-v1.json` (`4b4af6e2`, COVMV-PIN); read:
`info-clock-rate-v1.json` `7801c198...a152` (W1-I2).

**Reference dirs of record** (the gate's P9-B0; never deleted; per-file SHA-256 in `gate-logs/b0-files-<dir>.txt`;
each with its `-run` dir) - `handoff-root-gate.md:71-80`; the holds: `root-gate-report.md:51` (X-10), `:846` (u),
`:879` (w), `:897` (NAV):

| dir | role | files | listing SHA-256 | `summary.json` SHA-256 |
|---|---|---|---|---|
| `build-equity/p9-b0-r2-nav` | R-2 re-based (path A, FIELDS-SUB-A; fields v15; w = v8's `mega-v8-r1w-train-std-v80-1`) | 27 | `04bb8060aadf692a63e7f1baafd23f297db67c24d9c1f0a43dce6678a755b1bd` | `48a52f6adb06c2e966cb366f2c71c9dbef55ba48de243afc2cce01df3085c1b9` |
| `build-equity/p9-b0g-x10-nav` | X-10 re-based (holds) | 27 | `6c87c8d516d7782c2484d6cc397323359f9f9b61cc242a5f894885240a74f31d` | `deabe71b5e2dedfb0b224fb46fdcd0e7194a6950ad8f5ecabe9601cd786bd0bd` |
| `build-equity/p9-b0g-y1-nav` | Y-1 re-based on `p9-m2i-dcov-v11` (PRE-29; no identity claimed) | 28 | `cac882e19a5baee9d3d60581d93a278f9f4fc6b769ed0e9154dd408b1552b813` | `b0d4dcbf53e81aff3863ca1d9c7230da73a5c0a911a60f622c9f78aa3796c028` |
| `build-equity/p9-b0g-yf0-u` | Y-F0 u (Release IC, cold; holds, RELCACHE-ALLOW) | 5 | `1cb087abaa50746392269b3452491be8fa2999b9a39e1212f01049acec2916aa` | `acbf61cef24e909bb8975668b1b2206708b5c96cf03194a0728564445fae12b8` |
| `build-equity/p9-b0g-yf0-w` | Y-F0 w (Release IC, warm; holds) | 12 | `ec69344005107bf0e2ab0bb859583db083889521a652dcfe134b2304e27c8ae1` | `f772b394a6885b5965d0c5ad93e4f023515a74ed7e02064a772814357ee904f2` |
| `build-equity/p9-b0g-yf0-nav` | Y-F0 NAV of record (reads `p9-b0g-yf0-w`; `--ann-mean-gross --info-clock-print`; holds) | 27 | `e205d6ecb16e75cba02194b90b313849404c2052b3e1738c4bf020e486fcf797` | `826d5ec65329c24c4c0c1b72ee1f57dcdbdb136cf00d4113c6463310e79b8f38` |

Beside them: the two D-COV stores `p9-m2i-dcov-v11` (manifest `397366473b3a...23c7`) and `p9-m2i-dcov-covv1`
(`ea58691f...f0f3`) = their pins, never rebuilt (CRT-2; GATE-COVDIFF holds); Y-F0's spec keeps `ic.u_output` /
`ic.w_output` at `mega-v8-b0b-train-u-v8ysb-1` / `mega-v8xw-train-theme-erc-v8ysb-2` (the cycle's resume roots,
payloads byte-identical to `p9-b0g-yf0-u` / `-w`: u 3 / 3, w 10 / 10 incl. `train_combined.json` `e13fbc4d`; REPIN-REV).

**The substitution list** (`778d34b8`, committed before any P9-B0 run, + the amendments `bfbe64d2`, committed before
rows 4-6) - `root-gate-report.md:630-680`, `:802-833`: (1) R-2: fields v10 -> v15 (FIELDS-SUB; witness 70 / 70),
ruled path A (FIELDS-SUB-A), R-2 read by path `p9-b0-r2-nav` (amendment 4, REPIN-REV); (2) X-10; (3) Y-1: store ->
`p9-m2i-dcov-v11` (PRE-29); (4)-(5) Y-F0 u / w: token 0 and `--output` only, RELCACHE-ALLOW's classes; (6) Y-F0 NAV:
`--combined` -> `p9-b0g-yf0-w/train_combined.json` (YF0-NAV-ORDER) + `--ann-mean-gross --info-clock-print`; X-5 CUT
(FIELDS-X5); everywhere token 0 -> the Release exe and `--output` -> the new dir; `--ann-mean-gross` on every NAV
reference; `--seal-allow` on none (SEAL-REF: a driver flag, no seal scan in an exe-direct bounded run). Not re-based:
the Y-F0 fit (its flag-absent identity holds outside D2's code-identity stamps).

**`ucrtbase.dll`.** 10.0.26100.7623 `2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF` at step 0 (agents
1-4), at the cache-free P9-B0 runs (14:22:31Z) and at the Y-F0 runs (15:39:47Z) = the CRT-2 record; `ucrtbased.dll`
10.0.22000.832 `19E79E91...F48D` (C.6).

**rho of the pairs (B2-13; value and flag only; Release eval verb p9-3f; T 1005).** P9-X pair (re-based Y-1 vs re-based
Y-F0) 0.9947035686613187, "1 - rho < 1e-6" false; X-10 vs Y-F0 0.9994863034956717, false; Y-1 vs X-10
0.9947113980219978, false (`gate-logs/rho-pairs-{1,2}.txt`). Each cell's own pair (incl. P9-V's) is measured by the
cells root and logged before its read.

**Eval and fit modes.** Eval mode research-fast (the Y-F0 u receipt: no `--eval-mode audit-exact`; VM identity
without `_audit`; PATH-1). Fit mode of every v2 cell: flag-absent, the Python fit (D3-Q1a DIFFERENT; P9-F0h produced
flag-absent).

**Store step.** Catalog digest of the gate head `adbcd348b5d82a1fd4f357274a34bbf387391cf82154a961e701bad39a0dd474`,
reproduced by a second from-scratch catalog; fixture chain digest
`a50fdcf9c9c83bdac0d41ff368666da31578ebb4407bbc4498175a63014f100c` reproduced (after `2078f877`, `3a1a4759` and
`f8a85ad4`); `verify --pins` ok 1,620 / declared 169 / stale 220 / missing 630 / unresolved 1,512; render identity
435 / 0; `classes.json` `5bc27ba4...6557` (G item 6). The first
catalog attempt `p9-b0g-cat-1-run` is void (process-error exit 1 before any catalogue: a relative exe path), kept as
its record.

**Cache listings** (`build-equity/mega-candidate-cache-v8-lo3`; GATE-IDX, GATE-RELCACHE): with the GATE-IDX file 352
files `fb444f7d316a1a4c`; after the owner's deletion, before the Release pass, 351 files `155872de50833fca` (the
originals); after, 552 files `c9ee9291f5ddc0c2` = the 351 unchanged + 201 files (4,466,406,032 bytes) of the Release
identity subtree `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0/` (C.13).

**Lapse table of record (LAPSE-TABLE, ledger :981; B.10).** P9-L, P9-H, P9-M (sleeve line kept), P9-C (ALCLOCK-1 held),
P9-MV (COMPOSE-1a ACCEPT), P9-V RUN; P9-W LAPSED (0) (D3-Q1a DIFFERENT); P9-X pending PRE-30 as PRE30-MVX reads it.
Order unchanged (A.7).

**Cut and not-produced lines.** (1) X-5's step-3 NAV / u / w flag-absent identity: CUT, "not run at the gate: fields
v13 absent (FIELDS-GONE)" (FIELDS-X5); (2) the step-7 X-5 opt-in cache identity: CUT (FIELDS-X5; M6b items 5 / 6
cited); (3) TEST-2's cut list: no item left "not run at the gate" (every item had run when TEST-2 arrived); (4) P9-W:
LAPSED (0): its paired-dSR print is not produced, F.1's prediction prints "not produced: P9-W lapsed (0), D3-Q1a" and
E's line "P9-W lapsed (0), D3-Q1a: no dSR" (PW-LAPSE-PRINT; B.6, F.1, E); (5) "cross-build reference" is NOT printed
beside G-B6 (FIELDS-SUB-A); (6) no gross-alpha line is cut (C3-6 held; B.0) and no `info_clock` block is cut (ALCLOCK-1
held; B.4); (7) B.2's "ex-ante vol ratio" stays cut at P9-H's registration commit (round 2a; Appendix IV item 11).

**Findings and rulings the cells inherit.** FINDING FIELDS-GONE (ledger :978; A.4): fields v9 / v10 / v13 / v14 / v15a
absent, v11 manifest only, v15 whole; nothing rebuilt from atx-db; an owner item. FINDING FIELDS-RISK (ledger :1005;
A.4): `verify` refuses `lib-v8ysb-gm.json` run ALONE (its template chain resolves a `baseline_fields` of v13); as a
parent it resolves; a cell preflight refusing on it is a driver stop before any read (UNDEFINED (0) as B.0 words it,
logged) and a hand-back for the PM; nothing pre-decided. D3-Q1a's difference is a FINDING for the freeze report
(ledger :975; B.6). PW-LAPSE-PRINT (B.6, F.1, E); B8A-PROD-2 (B.8 (a): step 2 with the fields substituted to v15);
SEAL-REF (B.0, B.1); REPIN-REV (A.4, B.1, C.15); GP-TICKS (G items 3 and 6: G-P5 PARTIAL, G-P1 at P9-H; G-P2 / 3 / 4 / 6 /
7 / 8 / 9 / 11 ticked); PRE34-WHO (A.4, B.7: the printed-precision check is the cells root's, at the P9-X step).

**Disclosed for GATE-REV.** FIELDS-SUB's letter gave PATH B for R-2 (`root-gate-report.md` agent 3); FIELDS-SUB-A ruled
path A on the classification (A.4). The Y-F0 u compare tool's first pass put M1d's list (209 leaves) outside; it is
kept (`yf0u-cmp-pass1.txt`) and the tool corrected (`tools/CHANGES.txt` (3)). The follow-up re-pin `3a1a4759` and its
two `verify` refusals are recorded, then reverted (REPIN-REV). The reference NAVs are exe-direct bounded runs with no
C-13 binding to their specs; their argv carries the substitution list's opt-in flags, which every cell carries alike
(`--ann-mean-gross`; P9-C `--info-clock-print`) - `handoff-root-gate.md:140-142`. [FLAGS-ROUTE, `f8d852f3`: the
route: every P9 cell inherits `--ann-mean-gross` and `--info-clock-print` from the lineage parent's `nav.flags`
(`lib-v8ysb-gm.json`, `a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a` after `f8d852f3`; the driver
builds the NAV argv from the resolved spec's `nav.flags`, `scripts/cycle/phases.py:824`); the references carry both by
hand [SG4-REF: of the P9-B0 NAV references only `p9-b0g-yf0-nav` carries `--info-clock-print` (with `--ann-mean-gross`);
`p9-b0-r2-nav`, `p9-b0g-x10-nav`, `p9-b0g-y1-nav` carry `--ann-mean-gross` only (their receipts); every spec over the
lineage parent carries both (FLAGS-ROUTE); the one-sided case is FLAGS-ROUTE-2 (b)]; the spec route reproduced
`p9-b0g-yf0-nav` (HOLDS: 27 / 27 files byte-identical,
`gate-logs/flags-identity-yf0.txt`; B.0 "Gross figure opt-in"); the hand cells P9-X / P9-V carry `--info-clock-print`
one-sided against `p9-b0g-y1-nav`, disclosed, not a void (FLAGS-ROUTE-2 (b); B.0 "Void", B.7, B.8); token order is no
input (FLAGS-ORDER)]
