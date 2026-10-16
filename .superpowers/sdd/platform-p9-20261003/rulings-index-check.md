# P9 rulings-index check (IDX-1 re-check)

Written 2026-10-16 by the IDX-CHECK docs agent (ruling IDX-1: "the PM re-checks the index against the ledger before the report"; this check is delegated). It is read-only on both inputs and edits neither.

- **Index:** `rulings-index.md` at commit `5f48a25d`, 483 rows in Table 1, compiled from the ledger's L1-L939.
- **Ledger:** `progress.md` read 2026-10-16T12:49Z: 965 lines, sha256 `342addef9e31d703d809acd09caeb31f873ad5114f1139e3e739ad79e52058ed`. Its first 939 lines hash to `8f7a192aa2675d8fdf4f3c7d8e1b32f8ac49609c8cfcd4acdf35546575ffc6f7`, which is the index's final-read hash. So the index was built from exactly these bytes.
- **Scope:** items 1-3 cover L1-L951. Item 4 lists every ruling line from L952 to the end (L965).

## Summary

- **Rows checked:** 483 (476 ruling rows and 7 † restatement rows).
  - Every cited line exists.
  - 448 rows have an ID. Each cited line carries "Ruling" next to that exact ID: 341 lines use the form `Ruling <ID>` and 107 use `<ID> Ruling`. The 4 grouped rows (L517, L524, L526, L528) were checked for every ID they list.
  - Each of the 28 no-ID rows cites a line that starts `- Ruling:` or `- Ruling (`.
  - Each of the 7 † rows cites the ID-led restatement line it describes. None of those lines contains "Ruling", as the index states.
- **Block column:** 0 mismatches. In two rows the ruling is embedded in a RESUME or STOP line: L220 PY-HYG is in the RESUME s3b line and L337 PLOT-1 is in the STOP 3 line. Each row takes the block its own line opens, which is consistent.
- **Mismatches by kind (Table A):**
  - wrong line: 0
  - wrong ID: 0
  - a gist that copies a number from a result line: 0. Every number in a gist is a rule parameter, a cap, a pool or slot number, a count or a SHA prefix.
  - unfaithful gist: **5**. Each one leaves out a condition or a binding clause, so the gist reads wider or narrower than the decision: L719, L723, L745, L774, L889.
  - 3 more gists are borderline and not counted (Table A2).
- **Missing rulings (Table B):** **6**: L941, L945, L948, L949, L950, L951.
  - All six come after the index's L939 cut-off. No ruling in L1-L939 is missing.
  - Two lines carry the word "Ruling" but are not rulings: L615 (the STOP 6 count) and L922 (the RESUME 12 line). The index rightly leaves them out.
- **Second table (Table C):** 66 owner rows, 14 refusals (R1-R14), 87 rows in 3a, 28 rows in 3b and the 15 line references of 3c were checked.
  - **1 row** cites a line that does not name its item as an owner item: Table 2 L677. One more row is weak: Table 2 L51.
  - **Missing items: 4.** These are 2 owner items named in rulings (L337, L941) and 2 permission refusals from the L940 hand-back (the second refusal is named by ruling L941).
  - **0 P10 items** are missing. L949's P10 mention restates NAV-2STORE, which the 3a row for L889 already has.
- **Later in-scope rulings that change how index rows read (Table D):** 4 entries: L945 (DISK-4), L948 (COVMV-PIN), L949 + L950 (MVX-F0H, MVX-PRE13), L941 (GATE-IDX).
- **Delta from L952 (Table E):** **11** ruling lines, which the final report must add.

## Table A: mismatches in Table 1

| ledger line | ruling id | kind | index gist (as written) | decision clause / suggested gist |
|---|---|---|---|---|
| 719 | ABS-4 | unfaithful (condition dropped) | "Absence implemented by blanking the listed member's signal in `score_role` is accepted in principle." | The ledger accepts it in principle *subject to the reviewer's trace*: (a) cache isolation, (b) flag absent = bytes unchanged, (c) the listed member is in no-data form on every date, (d) the blanking point is named. If (a) fails, it is Required. Suggested gist: "Blanking the listed member's signal in `score_role` is accepted, subject to the reviewer's trace (cache isolation, flag-absent bytes, no-data rows); a cache leak is Required." |
| 723 | COVMV-S | unfaithful (Required set understated) | "... S-1 (hoist the eigen print's workspace) required, listed items, then a scoped re-review." | The decision makes **S-1, S-2 and S-6** Required. S-3 to S-5 are done if cheap and S-7 is record only. Suggested gist: "COV-MV fix round 1 before its merge slot: S-1, S-2 and S-6 required; S-3 to S-5 if cheap; S-7 record only; then a scoped re-review." |
| 745 | ALCLOCK-S | unfaithful (Required set understated) | "... the S-3 calibrate-gross gtest required, other listed items; S-5 to P10." | The decision makes **S-3, S-2 and S-1** Required. S-4 is done if small and S-5 goes to P10. Suggested gist: "AL-CLOCK fix round 1 before its merge: S-3 (calibrate-gross gtest), S-2 and S-1 required; S-4 if small; S-5 to P10; then a scoped re-review." |
| 774 | HOST-4 | unfaithful (amendment dropped) | "HOST-2 / HOST-3 stand: one C++ build at a time; this wave's build priority order is fixed." | The same decision clause also raises the admission wait bound from 90 to 180 minutes per build for that wave. That amends HOST-3's 90-minute bound, so "HOST-3 stands" alone misleads. Suggested gist: "HOST-2 / HOST-3 stand (one C++ build at a time, Python first); this wave's build priority is fixed; the admission wait bound rises to 180 minutes." |
| 889 | PRE-MVX | unfaithful (condition dropped) | "... P9-F is P9-X's book; a two-store NAV is P10." | The ledger says P9-F "is P9-X's book **if P9-X is accepted**". The in-scope ruling MVX-PRE13 (L950) reads that clause as the second branch of PRE-13, not a replacement for it: P9-V comes first. Suggested gist: "P9-X's parent is P9-F0h, not P9-F0; P9-V's parent stays P9-X; P9-F is P9-X's book if accepted (PRE-13 branch, MVX-PRE13); a two-store NAV is P10." |

### A2: borderline gists (lossy, not counted)

| ledger line | ruling id | note |
|---|---|---|
| 216 | M1a-RED | The gist leaves out the clause "until A3's fix is merged and its root cause known, no further field is flipped to `kind: engine` beyond the three". ALSIG-FLIP (L227) relies on that clause. |
| 695 | D3W-S | S-2 and S-8 are REQUIRED in D3W-PB, and S-3 is REQUIRED in ABS. The gist says only that they "go to the next lane owning each file". |
| 832 | COMPOSE-1 | The gist leaves out the gate's tiny-world parse of both flags together (parse / rule-table stage only), and the rule that a refusal lapses P9-MV at 0. COMPOSE-1a (L956, delta) refines this. |

### A3: other observations (not mismatches)

- The index header says "No line holds more than one `Ruling` keyword". L936 holds two: the second is the quoted "Ruling <ID>" inside IDX-1's own text. No row is affected.
- The index's refusal labels R1-R14 clash with the ledger's own `R<n>` tags, which mean RESUME n: "(R7)", "(R6)" and "(R5)" at L672, "R10, #1" at L790 and "R11, #1" at L882. The index maps L672's three correctly to its own R8, R6 and R3. The final report should use one labelling scheme and say which.

## Table B: rulings in L1-L951 missing from the index

All six come after the index's L939 cut-off. The index's block rule ends RESUME 12 only at a line starting `- RESUME` or `- STOP`. L946 starts `- OWNER STOP 12`, so L948-L951 fall in RESUME 12 by that rule, although they sit after the owner's STOP 12.

| ledger line | block | ruling id | gist |
|---|---|---|---|
| 941 | RESUME 12 | GATE-IDX | While the leftover `index.sqlite` sits in the lo3 candidate cache, no run of record names that cache. The refused deletion is not redone: the owner deletes the file. If it is still present, the gate hands back BLOCKED-OWNER. |
| 945 | RESUME 12 | DISK-4 | The RESUME 12 clean-up ruling (L937) and its execution line (L943) are re-labelled DISK-4. DISK-3 (L494 / L496) keeps its ID, and the final report cites DISK-4. |
| 948 | RESUME 12 (after OWNER STOP 12) | COVMV-PIN | Yes: the `mv-aim-v1.json` SHA that B.5 records is a prereg pin. COVMV-S2 S-b's sentence edit is not made (PRE-40 form), and the same holds for NAV-S S-2. |
| 949 | RESUME 12 (after OWNER STOP 12) | MVX-F0H | Yes: every B.7 / B.8 mention of P9-X's or P9-V's parent, reference or comparison base means P9-F0h. G-B1 and G-B6 keep P9-F0. |
| 950 | RESUME 12 (after OWNER STOP 12) | MVX-PRE13 | Yes: PRE-13 stands whole, with its base read as P9-F0h. PRE-MVX's "P9-F = P9-X's book if accepted" is PRE-13's second branch, not a replacement. |
| 951 | RESUME 12 (after OWNER STOP 12) | PRE-2BI-REST | Yes, these are part B's: round 2b-i items that the PARTIAL header does not list as applied are applied in part B wherever they bind a cell or the OD-3 read. Superseded rulings are cited, not applied. |

Lines that carry the word but are not rulings: L615 (STOP 6 state: a count of "Ruling" lines) and L922 (RESUME 12: "every Ruling line"). The index rightly leaves both out.

## Table C: the second table (owner items, refusals, P10 items)

### C1: rows that do not cite a line naming the item

| table | ledger line | row source | issue |
|---|---|---|---|
| 2 | 677 | SPDLOG-RELINK | The line names a **P10** item (per-preset dependency build dirs), not an owner item, and never mentions the owner. The same item is already 3a's row for L677. Either drop the row from Table 2 or cite an owner line. |

Weak attribution (not counted): Table 2's row for L51 cites a line that names the failing `dev` preset configure ("ignored for lanes"), but not as the owner's. The owner listing is at L463 (OD5-3) and L586 (POOL-MAP R6-3), so the row should cite one of those.

Every other row cites a line that names its item:

- Table 2: the other 65 rows.
- 2a, R1-R14:
  - The code blocks of R1, R2, R5, R6, R7, R8, R10, R11, R12 and R13 appear in the cited line's text. R8 matches piece by piece.
  - The descriptions of R3, R4, R9 and R14 match their lines (L496, L560, L658, L930).
  - L903 and L911 do say "not a permission refusal".
- 3a: all 87 rows. Each cited line defers that item to P10.
- 3b: all 28 rows.
- 3c: all 15 line references. Each mentions P10 only in the way the index states.

### C2: items missing from the second table

| ledger line | source | item | belongs in |
|---|---|---|---|
| 337 | PLOT-1 (STOP 3 line) | The owner asked for an equity-curve plot of the latest accepted engine book. PLOT-1 names it as the re-based Y-F0 NAV from P9-B0. This is an owner deliverable. | Table 2 |
| 941 | GATE-IDX | The owner deletes `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` by hand. Until then no run of record names that cache, and the gate hands back BLOCKED-OWNER if the file is still present. | Table 2 |
| 940 | root M6B-FINISH hand-back, PERM-1 #2 (named by ruling L941) | Permission refusal: the auto-mode classifier refused deleting that `index.sqlite` by its literal name. The ledger does not record the command; it points to `m6b-logs/perm-refusals.txt`. | 2a (next label after R14) |
| 940 | root M6B-FINISH hand-back, PERM-1 #1 | Permission refusal: deleting the previous M6b agent's scratch catalog in that session's scratchpad was refused, and the catalog was left in place. The command is in the same log file, not in the ledger. | 2a |

No P10 item is missing. Every ruling line in L1-L939 that mentions P10 is in 3a or 3c. Of the rulings added after L939, only L949 mentions P10 ("a two-store NAV is P10"), and that restates NAV-2STORE, which the 3a row for L889 already has.

Borderline (not counted):

- **L343, WQ-1:** "unless the owner directs otherwise". The row is in 3a, and Table 2's row for L393 already states the same owner option.
- **L220:** "owner rejected the bare-root collection probe". This owner decision sits in the RESUME s3b line beside PY-HYG.
- **Owner instructions that rulings carry out** (L231, L584, L629, L641, L688, L931): these are directives to the PM, not items for the owner. Add them only if the final report lists owner instructions as such.

## Table D: later in-scope rulings that change how index rows read

| ruling (line) | index rows it touches | effect for the final report |
|---|---|---|
| DISK-4 (945) | Table 1 L937; Table 2 L937; header note 1 ("DISK-3 ... reused") | Cite L937 as DISK-4. DISK-3 is only L494 / L496, so the "ID reused" note no longer applies. |
| COVMV-PIN (948) | Table 1 L814 (COVMV-S2), L890 (NAV-S) | The condition "edited if unpinned" resolves to "pinned": the registration sentence is not edited, and its staleness is stated beside it (PRE-40 form). |
| MVX-F0H (949), MVX-PRE13 (950) | Table 1 L889 (PRE-MVX), L524 (PRE-13) | The P9-F rule is PRE-13 with base P9-F0h. G-B1 and G-B6 keep P9-F0. |
| GATE-IDX (941) | Table 1 L660 (SQL3-2) | While `index.sqlite` exists, a run naming the lo3 cache would record a `store` block, so no run of record names that cache. |

## Table E: delta from L952 (ruling lines L952-L965, as read 2026-10-16T12:49Z)

| ledger line | ruling id |
|---|---|
| 953 | R13-RESUME |
| 954 | GATE-TMP |
| 955 | GATE-RELCACHE |
| 956 | COMPOSE-1a |
| 957 | HOST-6 |
| 959 | PRE-2BII-B1 |
| 960 | D3Q3-ARGV |
| 961 | D3W-TPL |
| 962 | D3W-V3READ |
| 963 | F1-FIELDS |
| 964 | COMPOSE-PRE |

Total: 11. L952 (RESUME 13), L958 (dispatch of root GATE agent 2) and L965 (dispatch of PRE part B1) are not rulings.

## Method

1. Table 1 was parsed from the index. For each row, a script read the cited ledger line and checked three things: that "Ruling" appears there, that the row's exact ID appears next to it (as `Ruling <ID>` or `<ID> Ruling`), and that the row's block matches the index's own block rule.
2. Each gist was read against its line's decision clause: the text before the first ` -- `. Where that cut fell inside a parenthesis, the whole line was read.
3. Every number in a gist was listed and checked to be a rule parameter, ID, pool, slot, count or SHA. None is a result figure.
4. All lines in L1-L951 carrying `Ruling` were compared with the set of rows.
5. For the second table:
   - Every cited line was read for the item: "owner", the refusal text or "P10", plus the row's named tokens.
   - The refusal code blocks were matched against the cited line's text.
   - Every ruling line in L1-L951 that mentions P10 or owner was compared with the rows.
6. No data file, `build-equity/` path, `trials.jsonl` or file dated 2024-01-01 or later was opened. No build or test was run. No figure from a result or hand-back line is copied here.
