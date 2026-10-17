# P9 sprint: PM handoff 11 (owner stop 10, 2026-10-08)

For the next parent (PM) agent. Supersedes `status-10.md`. The goal prompt to start the next session is in
`next-goal-prompt.md` (same dir, refreshed at this stop). Written at the owner's stop ("stop here", 2026-10-08, while
root M5 had just been dispatched); hand-backs that had not arrived when this file was written are marked (pending) and
their pool handoff / review files are the truth. Verify every head with `git log` before dispatch. The ledger
`progress.md` block from "- STOP 10" records every hand-back as it arrived.

## Read order

1. This file.
2. The contracts every agent gets: `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md` (TEST-1), `resume10-addendum.md` (wins where
   they differ; LEASE-DEAD, HOST-4, REV-FRESH, ...). Per-agent text: `r10-dispatch.md` (lanes, root M3 / M4, the scoped
   re-reviews, SQL4; it points at `r7-dispatch.md` / `r8-dispatch.md` / `r9-dispatch.md`). Reviewers:
   `r7-review-dispatch.md` + `r8-review-notes.md`. Root: `r7-root-merge.md` + `r10-dispatch.md` "ROOT M4" + the ledger's
   "Dispatch root M5" line.
3. The ledger `progress.md` from the line "- RESUME 10" to the end. Every "Ruling" line is binding (so are the older
   ones); the RESUME 10 rulings are listed at the end of this file.
4. `handoff-root-m4.md` (root after M4: code head `5c811b56`) and `handoff-root-m5.md` (root M5 stopped at STOP 10
   BEFORE any merge: nothing of B2 fix 2 / OD3P follow-up / D3W-PB / SIGN / A4 landed; docs head `e3b6c111`; ALL of
   M5 is the next root agent's list, then M6).
5. `p9-prereg.md` (round 2b-i committed at `685e238b`; 2b-ii after the gate: the ledger's "PRE 2b-ii list" lines,
   three of them now), `p9-prereg-flags.md`, `wave3-carry.md`, `root-gate-brief.md` (the single gate, GATE-1).
6. Lane handoffs / reviews in the pools, when you dispatch that lane (table below).

Plan: `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md`. Root tree `C:/atx-wt/pool-2`, branch
`feat/platform-p9-20261003`. Trial ledger `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f`, unchanged
(0 trials this session).

## State in short

Waves 2 and 3 are MERGED in root except the follow-ups: root M3 merged C3b `b182dfdd`, D3W `08f04e07`, ABS `898bf120`
(+ SQL2-CLS rows `094bcd3b`); root M4 merged D3 `0a2ceb2b`, OD3P `7f6f1c75` (at `dc11c977`), AL-CLOCK `858a4fca` (at
the first APPROVE `e9b3a323`; its fix round 1 is done and in re-review), COV-MV `5c811b56` (at `25e11b32`: fix round 1
APPROVE, nothing left). Code head at the stop `5c811b56`, docs head `767609b4` (root M5 may have moved both: read
`handoff-root-m5.md`). FIVE follow-ups are APPROVE and queued for root M5 / M6: B2 fix 2 `101f695a` (clears the
known-red set), OD3P follow-up `4a7372e2`, D3W-PB `44a331e6` (chain `c044f5f6` -> `460f5ba2` -> `44a331e6`), SIGN
`e2844926` (chain `c2010ed1` -> `50a43599` -> `e2844926`; SQL2-CLS row `atx.sign-receipt/v1` class `ic-output`), A4
`fc3e414e` (chain `6dab6948` -> TRAIN identity on both fields (A4-Q2) -> flips -> APPROVE head). In flight at the
stop: AL-CLOCK fix 1 re-review (lane head `f892eab1`), SQL3 item 5 (lane), SQL4 (new stretch lane, pool-32), A4 fix
round 1 (tests only), root M5. The known-red set holds ONE entry (the legacy n37 test, B2-N37) until B2 fix 2 merges.
No gate run, no P9 cell run. The prereg is at round 2b-i.

Host at the stop: free RAM about 2.4-2.9 GB; C: about 58 GB free; another session's atx-vol build ran in
`C:\atx\build-vol` for an hour (HOST-5: foreign, ignored by our wait loops). Every pool lease prints `owner=dead`
(LEASE-DEAD: untouched; lanes work in their pools regardless).

## Lanes at the stop

| lane | pool / branch | head | state | next |
|---|---|---|---|---|
| wave 2 + C3b, D3W, ABS, D3, OD3P (round 1), AL-CLOCK (first APPROVE), COV-MV (fix 1 in) | merged | root `5c811b56` | complete | gate |
| B2 fix 2 | pool-14 `feat/p9-b2-20261003` | `101f695a` APPROVE (code `509f4b79`; fix `71abeb2f`, test only) | re-review 3 `task-B2-rereview-3.md` 0 Required; B2-S3 | root M5 (1): merge, re-run n37 with root's verb + blind flags -> known-red EMPTY; (pending) |
| OD3P follow-up | pool-21 `feat/p9-od3p-20261004` | `4a7372e2` APPROVE (code `cc6686f9`) | re-review 2 0 Required; OD3P-ENV; OD3P-S2 | root M5 (2): the two lane files 96 / 0 with the verb; (pending) |
| D3W-PB (stretch) | pool-23, above D3W | `44a331e6` APPROVE (fix `90bc1962`; lane `460f5ba2`; pre-deletion `c044f5f6`) | review 0 Required / 7 Suggested; S-g keep-both | root M5 (3): chain with the TRAIN identity at `c044f5f6`; before the P9-B0 re-pin (EXE-FREEZE); (pending) |
| SIGN (stretch) | pool-29 `feat/p9-sign-20261006` | `e2844926` APPROVE (deletion `50a43599`; pre-deletion `c2010ed1`) | review 0 Required / 5 Suggested; class `ic-output` | root M5 (4): `c2010ed1` + root's identity on the ledgered screen rows by path, then the deletion; (pending) |
| A4 (stretch) | pool-27 `feat/p9-a4-20261005` | `fc3e414e` APPROVE (lane `7c9140c4`; pre-flip `6dab6948`) + fix round 1 in flight (pending) | review 0 Required / 6 Suggested; A4-S orders a tests-only fix round (S-1 seal rows, S-2 probe per clock) | root M5 (5): `6dab6948`, TRAIN identity on BOTH fields (A4-Q2: both are in Y-F0's manifest `26fee5ce`), flips, then the APPROVE head of record; the fix head follows if APPROVE (M6) |
| AL-CLOCK fix 1 | pool-30 `feat/p9-alclock2-20261007` | `364391e0` = re-review 1 PARTIAL (no verdict; diff not yet read) above the lane head `f892eab1` | S-3 calibrate gtest, S-1 / S-2 / S-4; builds `p9-alclock-k / -l / -m` (ALCLOCK-M); the file's 9-step "Exact next step" | fresh reviewer resumes into the same file (ALCLOCK-REV-RESUME); if APPROVE: root M6 second AL-CLOCK merge; until then `e9b3a323` (merged) is the SHA of record |
| SQL3 item 5 | pool-13 `feat/p9-sql3-20261004` | `d43b3a8b` (stop handoff) over `d91ce86c` (merge of root `5c811b56`) over code `b55c0d11`; clean | call lines fixed (relative-root slip) and tested: hook 13 / 13, identity 16 / 16, writers 98 / 0 with root's eval exe; NOT done: teeth (TEETH-CRLF fix in the handoff), report section, SQL3-TREE deletion; SQL3-WQ (`wave_queue.py:146`) in the same round | finish from `handoff-SQL3.md`; review 2 (BASE `69b8c162`); root M6 |
| SQL4 (stretch; SQL4-GO) | pool-32 `feat/p9-sql4-20261008` | `73a871fb` (handoff) over WIP `a1955a71`; clean | tasks 1 (IC flag), 2, 3, 4 coded, NOT compiled (the new TU not in CMake; `atx-impl-strategy` does not link the store lib: lines in the handoff); pending 1 (marginal flag), 5, 6, 7, report; SQL4-Q1 (P10), SQL4-Q2 (reviewer judges) | finish from `handoff-SQL4.md`; first review; root M6 LAST, before the gate's P9-B0 re-pin, else P10 untouched (SQL-12) |
| A4 fix 1 (tests only) | pool-27, above `fc3e414e` | `e982afef` (handoff only; S-1 / S-2 NOT started; design in `handoff-A4.md` "RESUME 10 fix round 1 (A4-S)") | four seal gtests + three per-clock probe gtests + fixture rows via `make_holdings_fixture.py` | resume from the handoff; scoped re-review; root merges `fc3e414e` at its M5 slot regardless (A4-S) |
| PRE | sprint dir `p9-prereg.md` | round 2b-i | 2b-ii after the gate: the ledger's three "PRE 2b-ii list" lines | review, then the PM rules the file |

Frozen pools (TEETH-POOL): pool-8 (AL-CLOCK, six C++ files hold teeth set 1) and pool-24 (COV-MV, two `// TEETH`
lines) wait for the OWNER; the exact `git checkout --` lines are in the ledger ("PERMISSION REFUSALS carried from
RESUME 8"). Pool-23 `build-equity/` holds about 2.4 GB of binaries built from a MUTATED `fit.cpp` (teeth
`p9-d3wpb-c`; never evidence); its deletion was refused (PERM-1; TEETH-BIN-23): the owner deletes it by hand if wanted;
no agent points an exe variable at pool-23's `bin/`.

## Open with the owner (surfaced; none blocks engineering)

1. **Permission refusals** (PERM-1 held, nothing retried): NEW this session (R10 #1): the D3W-PB reviewer's deletion
   of its own object tree, one command `cd C:/atx-wt/pool-23/build-equity && rm -rf .ninja_deps .ninja_log
   CMakeCache.txt CMakeFiles CPackConfig.cmake CPackSourceConfig.cmake CTestTestfile.cmake atx-core atx-engine atx-impl
   atx-tsdb bin build.ninja cmake_install.cmake compile_commands.json lib spdlog.pc tests vcpkg-manifest-install.log
   && ...` (ledger "PERMISSION REFUSAL (R10, #1)"). Carried: the two teeth reverts (pools 8, 24), the DATA-2 deletion
   dispatch, B2's scratch Write, and the earlier ones (ledger RESUME 9).
2. **Frozen pools 8 and 24**: restore by hand with the two `git checkout --` lines, or leave them; the lanes
   continued in pools 30 / 31 and are merged.
3. **Leases**: every pool lease prints `owner=dead` (the heartbeat keepers died with earlier sessions); nothing was
   recovered or released (LEASE-DEAD; the owner's rule). Pool-32 was leased by the PM for SQL4 (lease
   `p9-sql4-20261008`).
4. **`C:\atx\data` clean-up** (DATA-3): unchanged; `data-classification.md` is the deliverable.
5. **Host**: another session built atx-vol in `C:\atx\build-vol` for about an hour; HOST-5 lets our agents ignore
   foreign builds (the memory gate still decides admission).
6. **Rulings the owner may want to look at**: SQL4-GO (a new stretch lane dispatched this late; P10 whole if it misses
   the gate), HOST-5, COVMV-BYTES (the with-flag before / after byte compare replaced by the reviewer's hunk
   classification + the S-1 bit-identity gtest), A4-S (a tests-only fix round ordered for seal / probe gaps), B2-N37
   outcome (cause (b): arithmetic, max |relative difference| 4.967e-14; the test compares at 1e-12; golden untouched),
   COMPOSE-1 (no composition test for `mv-aim-v1` + `info-clock-rate-v1`; a parse check at the gate; P9-MV lapses on a
   refusal), TEETH-BIN-23.
7. **Owner items for the final report** (unchanged): local `main` / `origin/main` (OD5-3), the deterministic `pow` /
   `exp2` re-pin (OD5-2), data purchase (OD5-5), the atx-db request file (AL-DATA), the orphaned scratch dir
   (SCRATCH-2), the broken `dev` preset (also failed pool-32's lease configure: `atx-vol-install.cmake:142`), the
   shared `spdlog-build` relink (SPDLOG-RELINK, P10), the P10 list growing in the ledger (C3B-S, B2-S, D3W-S, COVMV-S2,
   OD3P-S2, A4-S S-3..S-6, D3WPB-Q1's remaining Python composition rules, ABS-S, D3-S).

## Open questions, NOT ruled (the next PM rules these)

- Whatever the pending hand-backs list as open (root M5, AL-CLOCK re-review 1, SQL3 item 5, SQL4, A4 fix 1): read
  each handoff / review before dispatching.
- If root M5 stopped mid-chain: which lanes landed, which chains are half-merged (a chain stops at a lane boundary;
  MERGE-SHA; nothing is reverted).
- Whether SQL4 can still make the gate (SQL-12 / SQL4-GO: APPROVE and merged before the gate's P9-B0 re-pin, else P10
  untouched); the PM may also stop it at the gate.

## Gates and rules the next PM must hold (new this session; status-10 lists the earlier ones)

- LEASE-DEAD (no lease action), HOST-4 (priority order; 180-min wait bound), HOST-5 (foreign builds ignored),
  REV-FRESH (fresh reviewers; BASE = the previous APPROVE SHA), FU-REVIEWS (file names and BASEs), MERGE-SHA (chains
  for D3W-PB, SIGN, A4 as listed), EXE-FREEZE (D3W-PB, SIGN, A4, SQL4 and the AL-CLOCK fix head relink exes: all
  before the gate's P9-B0 re-pin), TEETH-BIN-23 (nobody deletes pool-23's build tree; nobody uses its exes).
- Known-red set = `test_legacy_n37_numbers_reproduced` only, until B2 fix 2 (`101f695a`) merges; the gate needs it empty.
- A4-Q2: both flipped fields are in Y-F0's fields manifest -> root's TRAIN identity on both (SHA only, bounded) before
  the flips merge; a miss blocks A4 (P10 whole).
- COMPOSE-1 (the gate's parse check of both rules), MV-FLAG-P0 (root at the OD-3 P0 step), COVMV-S2 S-b and
  SIGN-SUBST and D3WPB-Q2 and A4-Q2 in the PRE 2b-ii list, B2-S3 S-1 and OD3P-S2 S-4 as root slips at M5.
- Everything of status-10 and status-8: TEST-1 / ROOT-1a, GATE-1, PRE-35, D3-Q1 .. Q3, D3-S1, PRE-15-N (124),
  ALCLOCK-4, B2-EVAL-KEEP, C3-13, W3-9, TEETH-RESTORE, TEETH-POOL, PERM-1, B2-14, SEAL-ALLOW-3, ledger appends by scratch
  file + `cat >>` (a heredoc with apostrophes fails in the hook layer), never edit earlier lines.

## Next steps, in order

1. Verify the stop state: `git log` / `git status` in pools 2, 13, 14, 21, 23, 27, 29, 30, 32 (8 and 24 frozen,
   read-only); read `handoff-root-m5.md` and each pending handoff / review; trial ledger (133, `27e40f9f`);
   `lease-worktree.ps1 -Status` (every lease `owner=dead` is expected; never stale recovery). DIRTY-1 / DIRTY-2 for any
   dirty tree (SQL4's pool-32 is the likely one).
2. Root M5 / M6 (fresh root agent per group, one at a time): whatever of the "Dispatch root M5" list did not land, in
   its order (B2 fix 2, OD3P follow-up, D3W-PB chain, SIGN chain, A4 chain); then M6: AL-CLOCK fix 1 (if APPROVE),
   SQL3 item 5 (after its review), the A4 fix head (if APPROVE), SQL4 (if APPROVE; last). Step 0 of the first root
   agent: commit `progress.md`, `status-11.md`, `next-goal-prompt.md` as they stand (`git add -f`; never the PNGs).
3. In parallel: finish SQL3 item 5 and SQL4 from their handoffs (fresh agents); the AL-CLOCK re-review 1 if it stopped
   PARTIAL (resume by a fresh reviewer from its "Exact next step"); A4 fix round 1 and its scoped re-review; SQL3
   review 2; SQL4 first review.
4. The single gate (GATE-1; `root-gate-brief.md`) on the head with every merge: suites Debug + Release (0 failed,
   named skips only), golden at 1 and 4 workers, canaries, the parents' flag-absent identity, D3-Q1's engine-fit
   identity, the gate greps (D3-Q2, D3-S1), COMPOSE-1's parse check, P9-B0 (substitution list first; R-2, Y-F0, Y-1
   re-based on the Release v1.1 store, PRE-29; `--ann-mean-gross` on every reference; `--info-clock-print` on Y-F0 iff
   AL-CLOCK is in: it is), the re-pin incl. `exes.eval`, the store step (M2A-4, SQL3-2), scoreboard, G-P ticks, CRT
   record, rho of the P9 pairs, the whole-wave review split by area. EXE-FREEZE from the re-pin.
5. PRE round 2b-ii (Appendix IV values + the ledger's three "PRE 2b-ii list" lines), a review by a fresh agent, the PM
   rules the file.
6. Cells in order: P9-L, P9-H, P9-M, P9-C, P9-MV, P9-W, P9-X, P9-V (lapses per PRE-14 / PRE-35 / D3-Q1 / COMPOSE-1;
   P9-X per PRE-30 / PRE-34); adoption print; OD-3 once on P9-F0h (four history lines; MV-FLAG-P0 at P0) after OD3P +
   ABS are merged (they are); freeze gate; the final list of every ruling.

## Rulings made this session (full text in the ledger from "- RESUME 10")

Process: LEASE-DEAD, HOST-4, HOST-5, REV-FRESH, FU-REVIEWS, ROOT-M3-SCOPE, SQL4-GO, POOL-MAP (R10-1), TEETH-BIN-23,
STOP 10. Lanes: B2-FIX2-EXE, SIGN-RESUME, D3WPB-SEQ, COVMV-BYTES, M3-Q1, M3-Q2, SIGN-CLS, SIGN-TEETH, SIGN-WD,
SIGN-SUBST, A4-BUILD, A4-S, B2-N37-B6, B2-MERGE-MSG, COVMV-S2, OD3P-ENV, B2-S3, OD3P-S2, SIGN-CLS-2, SIGN-TEETH-TOOL,
ALCLOCK-M, COMPOSE-1, MV-FLAG-P0, C3B-DOC. Verdicts recorded: D3W-PB APPROVE `44a331e6`, COV-MV APPROVE `25e11b32`,
A4 APPROVE `fc3e414e`, B2 (fix 2) APPROVE `101f695a`, OD3P (follow-up) APPROVE `4a7372e2`, SIGN APPROVE `e2844926`.
Owner's words recorded: the /goal text of 2026-10-08 (the STOP 9 prompt as pasted), STOP 10.

## Standing warnings (in addition to status-10)

- Agents' wait loops that poll for any `ninja` block on foreign builds: HOST-5 says to ignore command lines outside
  `C:\atx-wt\`; put it in every dispatch.
- The R9 / R10 reviewers are gone with the session: every re-review is fresh (REV-FRESH) and must read the previous
  review file first.
- `.superpowers/` is git-ignored: `git status` hides untracked records; `ls -lt` each pool's sprint dir.
- A lane that deletes a configure-generated file makes the next `research-build.ps1` fail at ninja start with 0 files
  compiled (A4's `p9-a4-f`): re-configure with the allowed command first (A4-BUILD).
