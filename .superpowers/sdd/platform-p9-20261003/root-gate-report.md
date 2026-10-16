# Root gate report (GATE-1: the single wave-2 + wave-3 gate), P9, 2026-10-15

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Chain agent 1 (GATE-DISPATCH-12; dispatch
`r12-dispatch.md` "ROOT GATE", `r11-dispatch.md` "ROOT GATE" + "ROOT GATE: rulings from the whole-wave review",
`root-gate-brief.md` steps 0-9). Evidence and tools: `gate-logs/` (tools in `gate-logs/tools/`). Compared by SHA-256,
JSON path, exit code and counts only; no 2020-2023 statistic is read or recorded anywhere in this file.

- **Gate head (code): `06dc9e5a`** (`Merge lane SQL4 at 44a76937 ...`). Docs head at dispatch `0a481348`; the docs
  head moves under PAR-1 (the IDX docs agent committed `5f48a25d` `rulings-index.md` beside root; the PRE agent edits
  `p9-prereg.md`). `git diff --name-only 06dc9e5a..HEAD` outside `.superpowers/` is empty at every build and run below.
- **SQL4 verdict carried (SQL4-CUTOFF-2):** checks green at M6B-FINISH: SQL4 is in the gate head; a red in any of its
  24 files attributes to merge commit `06dc9e5a`.
- **Known-red set: EMPTY.** Named skips are listed per suite below; the Release
  `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` named skip stays (OD5-2).
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 `27e40f9f...` at every check (0 trials).
- **GATE-IDX:** `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` (73,728 bytes, sha256 `3a177dfd...c6f0`) was
  present at step 0; this agent never deletes it and runs nothing naming that cache while it exists (see step 3).
  The owner deleted it by hand; agent 4 found it absent at 15:11:39Z (the 351 originals, `155872de50833fca`).

## Status by step after chain agent 4 (2026-10-16 ~16:10Z; FINAL; detail in "Chain agent 4" at the end)

**Gate status: DONE** (GATE-1). Code head **`f8a85ad4`** (the revert of `3a1a4759`, REPIN-REV): the re-pin of record is
**`2078f877`** (`git diff 2078f877 f8a85ad4 -- scripts/specs/v8/` is empty); EXE-FREEZE in force from `2078f877` (no exe
touched since). The follow-up `3a1a4759` (REPIN-UW / REPIN-R2) and its two verify refusals are disclosed in "Step 6,
agent 4, follow-up" and its revert in "Step 6, agent 4: REPIN-REV" at the end. No code red; known-red set EMPTY; 0 trials (133 / `27e40f9f`); no build (next free tag
`p9-3h`); exes of record p9-3f / p9-3g, 27 / 27 + 27 / 27 `disk = receipt` at agent 4's start and end.

| step | state | evidence (`gate-logs/`) |
|---|---|---|
| 0 start state | **holds** (agent 4: heads, ledger, research-store absent, exes `disk = receipt`, `ucrtbase.dll` = CRT-2, GATE-IDX cleared) | `exe-sha-{rel,dbg}-4.txt`, `cache-lo3-listing-agent4-idx.txt` |
| 1 exes of record | **holds** (agent 1; unchanged) | `exe-sha-{rel,dbg}-4-end.txt` |
| 2 suites | **holds** (agent 2) | as after agent 2 |
| 3 identity, canaries | **holds** with the cut: Y-F0 NAV / fit identity, ALCLOCK-1, D3-Q1a DIFFERENT (P9-W LAPSED (0)), COMPOSE-1a ACCEPT, greps, golden, canaries (agent 2); Y-F0 u / w flag-absent identity on the Release IC exe **holds** (rows 4-5, RELCACHE-ALLOW); X-5 rows **CUT** (FIELDS-X5); R-2's identity = its NAV only (R2-IDENTITY), met by step 4 row 1 (FIELDS-SUB-A); R-2's u / fit / w **CUT**: "not run in P9: NAV-only reference; its v8-era u / fit / w inputs include fields v10, absent (FIELDS-GONE)" | `yf0u-cmp.txt`, `yf0w-cmp.txt` |
| 4 P9-B0 | **holds**: R-2 `p9-b0-r2-nav` (path A, FIELDS-SUB-A), X-10 `p9-b0g-x10-nav`, Y-1 `p9-b0g-y1-nav` (PRE-29), Y-F0 `p9-b0g-yf0-u` / `-w` / `-nav` (identity holds; the NAV reads the re-based w, YF0-NAV-ORDER); `ucrtbase.dll` re-recorded | `b0-runs.txt`, `yf0{u,w,nav}-cmp.txt`, `ucrt-b0-4.txt` |
| 5 rho | **holds**: P9-X pair 0.9947035686613187 (flag false); X-10 vs Y-F0 0.9994863034956717 (false); Y-1 vs X-10 0.9947113980219978 (false) | `rho-pairs-{1,2}.txt` |
| 6 re-pin | **holds**: the re-pin of record `2078f877` (pins only; `verify` holds; fixture `a50fdcf9...100c`); the follow-up `3a1a4759` (REPIN-UW / REPIN-R2) refused at `verify` and was reverted by `f8a85ad4` (REPIN-REV); after the revert `verify` holds again (line-for-line = the `883be9f2` record) and the fixture digest reproduces | `repin-verify{,-2,-3}.txt`, `fixture-chain-{4,5,6}.txt` |
| 7 store step, G-P | **holds**: catalog digest `adbcd348b5d82a1fd4f357274a34bbf387391cf82154a961e701bad39a0dd474`, reproduced from scratch; `verify --pins` counts; render identity 435 / 0; G-P1 not checkable, G-P5 PARTIAL, the rest ticked; G-P10 (a)-(c), (e) landed, (d) PARTIAL | `realcat-gate*.txt` |
| 8 whole-wave review | DONE before the chain | `wave-review-*.md` |
| 9 records | this file, `integration-log.md`, **`handoff-root-gate.md`** (final) | - |

## Status by step after chain agent 3 (2026-10-16 ~14:30Z; detail in "Chain agent 3" at the end; rows not listed are as after agent 2)

**Gate status: BLOCKED-OWNER** at the GATE-IDX check (14:21:49Z: `build-equity/mega-candidate-cache-v8-lo3/index.sqlite`
still present; the owner's to delete). Every cache-free item of GATE-3-ORDER is done and committed. No code red;
known-red set EMPTY; 0 trials; no build (next free tag `p9-3h`); EXE-FREEZE not started.

| step | state | evidence (`gate-logs/`) |
|---|---|---|
| 0 start state | **holds** (re-verified by agent 3: heads, ledger 133 / `27e40f9f`, research-store absent, 27 / 27 + 27 / 27 `disk = receipt`) | `exe-sha-{rel,dbg}-3.txt` |
| 3 identity | **PARTIAL** (as agent 2) + X-5 rows **CUT**: "not run at the gate: fields v13 absent (FIELDS-GONE)" (FIELDS-X5); Y-F0 u / w flag-absent identity not run (GATE-IDX) | - |
| 4 P9-B0 | **PARTIAL**: substitution list committed first (`778d34b8`); FIELDS-SUB witness **70 / 70**; R-2 run on v15 **completed**, comparison outside the `/producer` + opt-in list only at the FIELDS / DERIVED / M1d-C1 / C1-COST classes (no other path; the classification table is the disclosure): **PATH A, ruled FIELDS-SUB-A** (agent 3 recorded path B by the letter and asked): the re-based R-2 of record is `p9-b0-r2-nav`; X-10 re-based **holds** (`p9-b0g-x10-nav`); Y-1 re-based on the v1.1 store (`p9-b0g-y1-nav`; COV-9 S9 holds; differs from both earlier Y-1 dirs at 26 / 28 files, PRE-29); Y-F0 u / w / NAV **not run** (GATE-IDX) | `fields-witness.txt`, `b0-runs.txt`, `r2-cmp-raw.txt`, `r2-classify.txt`, `x10-cmp.txt`, `y1-cmp-{b0,v8}.txt`, `cov9-role-check-3.txt`, `v11-store-check-3.txt`, `ucrt-b0-3.txt` |
| 5 rho | **PARTIAL**: Y-1 vs X-10 = 0.9947113980219978, flag false; the P9-X pair and X-10 vs Y-F0 wait for Y-F0 | `rho-pairs-1.txt` |
| 6 re-pin | not reached (needs Y-F0's re-based references); EXE-FREEZE not started | - |
| 7 store step | as after agent 2 (catalog, `verify --pins`, render identity, second catalog, G-P ticks not run: post-re-pin); X-5 opt-in cache identity **CUT** (FIELDS-X5: M6b items 5 / 6 cited) | - |
| 9 records | incremental: this file, `integration-log.md`, `handoff-root-gate-3.md` | - |

Rulings acted on by agent 3 (rows not listed: as after agent 2):

| ruling | state | evidence (`gate-logs/`) |
|---|---|---|
| GATE-3-ORDER | followed: witness, substitution list (before any P9-B0 run), R-2, X-10, Y-1, then (cache-free) the Y-1 vs X-10 rho, then the GATE-IDX check | this file "Chain agent 3" |
| FIELDS-SUB / FIELDS-SUB-A | witness 70 / 70; R-2 run completed on v15; every differing path classified, no unclassified path; **path A ruled (FIELDS-SUB-A)**: the re-based R-2 of record is `p9-b0-r2-nav` (PRE-22 met by the fields substitution), the classification table its disclosure; G-B6 and the OD-3 R-2 line read it | `fields-witness.txt`, `r2-classify.txt` |
| FIELDS-X5 | X-5 step-3 rows and step-7 opt-in cache identity cut, recorded as ruled; M6b items 5 / 6 cited | - |
| GATE-MYPY | every commit by pathspec with the exclude; `git show --stat` 0 `mypy_cache` each | git |
| GATE-IDX | file present at start, at the check (14:21:49Z) and at the hand-back (14:29:49Z): BLOCKED-OWNER | `cache-lo3-listing-agent3-idx.txt` |
| GATE-RELCACHE | not reached (no Release u / w pass); built into `tools/b0_ic.py` | - |
| PRE-22 / PRE-29 / PRE-30 | R-2 per FIELDS-SUB (path A, FIELDS-SUB-A: `p9-b0-r2-nav`); Y-1 re-based on the v1.1 store by its pin | `b0-runs.txt` |
| PRE-34 | byte / JSON-path form: re-based Y-1 vs v8's ledgered Y-1 "differ at" 26 / 28 files; the printed-precision check is the cells root's (B.7) | `y1-cmp-v8.txt` |
| PRE-7a / C3-9 | `--ann-mean-gross` on R-2, X-10, Y-1 (opt-in leaves only, absent on every reference side) | `x10-cmp.txt`, `r2-classify.txt` |
| ALCLOCK-1 | `--info-clock-print` reserved for Y-F0's NAV (row 6; not run) | substitution list |
| SEAL-ALLOW-3 | no seal scan in a bounded hand run; flag on no argv; no seal stop | substitution list |
| COV-9 / COVR-1 | S9 holds before the Y-1 run; bounded runner; the v1.1 store = its pin | `cov9-role-check-3.txt`, `v11-store-check-3.txt` |
| SQL3-2 / SQL-11 | no `store` block in any receipt; no cache flag on any NAV argv; no cache-index flag anywhere | `b0-runs.txt` |
| B2-13 | Y-1 vs X-10 rho logged (value, flag) | `rho-pairs-1.txt` |
| M2c-RED-2 (d) / CRT-2 | `ucrtbase.dll` recorded at the P9-B0 runs: unchanged | `ucrt-b0-3.txt` |
| RUN-1 / RUN-2 / EXE-CUR / SPDLOG-RELINK | `--seconds 600 --max-rss-mib 2048`; free RAM stated per run; exe = receipt before and after each run | `b0-runs.txt` |
| TEST-2 | no suite run by agent 3 | - |
| LAPSE-TABLE | fit mode flag-absent in the records; P9-W LAPSED (0) carried | round-2b-ii table |

## Status by step after chain agent 2 (2026-10-16 ~13:45Z; the tables below it are agent 1's, as they stood at OWNER STOP 12)

**Gate status: BLOCKED-OWNER** at step 4: (i) GATE-IDX: `build-equity/mega-candidate-cache-v8-lo3/index.sqlite`
still present (the owner's to delete; no run of record named the cache); (ii) NEW: the fields directories
`build-equity/train-2020-2023-lo3-fields-v10/` and `-v13/` (with v9 / v14 / v15a) are absent from disk: R-2's NAV argv
(the P9-B0 re-base) and X-5's NAV / u / w identity argvs cannot run. No code red; known-red set EMPTY; 0 trials.

| step | state | evidence (`gate-logs/`) |
|---|---|---|
| 0 start state | **holds** (re-verified by agent 2; exes re-hashed `disk = receipt` 27 / 27 + 27 / 27) | `start-state-2.txt` |
| 1 exes of record | **holds** (agent 1; unchanged) | `mega-p9-3{f,g}-*`, `exe-sha-{rel,dbg}.txt` |
| 2 suites | **holds**: Release gtests 4,098 / 0 failed; Debug gtests 4,099 / 0 failed; ctest 291 + 728 on both trees, 0 failed; `scripts/tests` seeds 0 and 1: 703 / 0 failed / 3 named skips each; stage chain 10; `atx-engine/tools` 391; `atx-impl/tools` 770; `atx-impl/strategies` 14; fields 4 / 26 / 16 / 37; mirror guard 7; GATE-TMP closed (the tool's TMP root; two TMP-root effects shown, `tools/CHANGES.txt`); GATE-COVDIFF holds (3 x exit 0) | `gt-exits.txt`, `ctest-exits.txt`, `pyt-exits.txt`, `covdiff-runs.txt` |
| 3 identity, canaries | **PARTIAL**: Y-F0 NAV (argv, `--spec`) and fit identity hold; ALCLOCK-1 holds; D3-Q1a DIFFERENT (P9-W lapses at 0, PRE-35; finding); COMPOSE-1a ACCEPT; D3-Q2 / D3-S1 0 / 0; golden 1 / 4 workers OK both builds; canaries 15 / 15 both builds; **X-5 NAV identity not run (fields v13 absent); X-5 u / w not run (GATE-IDX + v13)** | `nav-identity-yf0.txt`, `fit-identity-yf0.txt`, `d3q1a.txt`, `compose-1a.txt`, `d3-greps.txt`, `inputs-check.txt` |
| 4 P9-B0 | **not started: BLOCKED-OWNER** (GATE-IDX; R-2's fields v10 absent) | this file "Steps 4-6" |
| 5 rho | not reached (needs step 4) | |
| 6 re-pin | not reached; EXE-FREEZE not started | |
| 7 store step, scoreboard, G-P, CRT | **PARTIAL**: scoreboard + timings reproduce the lineage; PY-S S-6 "equal"; CRT record; catalog / G-P / X-5 opt-in cache identity not run (post-re-pin; GATE-IDX) | `scoreboard-gate.txt`, `pys-s6-ledger-n.txt`, `crt-record.txt` |
| 8 whole-wave review | DONE before the chain | `wave-review-*.md` |
| 9 records | incremental: this file, `integration-log.md`, `handoff-root-gate-2.md` | |

## Status by step (agent 1, at OWNER STOP 12)

| step | state | evidence |
|---|---|---|
| 0 start state | **holds** (re-verified) | `gate-logs/start-state.txt`, below |
| 1 exes of record | **holds**: p9-3f Release + p9-3g Debug, 27 targets each, 0 warnings, 27 / 27 + 27 / 27 `disk = receipt` | `gate-logs/mega-p9-3{f,g}-*`, `exe-sha-{rel,dbg}.txt` |
| 2 suites | **PARTIAL** (OWNER STOP 12): Release gtests 16 / 16 targets 0 failed; `scripts/tests` seed 0 on Release exit 1 (1 failed / 702 passed / 3 skipped; the one red = GATE-TMP's cause, re-run pending); S-7 store hand check holds; the rest not run | `gate-logs/gt-exits.txt`, `pyt-exits.txt`, `covdiff-s7-files.txt` |
| 3 identity, canaries | **PARTIAL**: canaries 15 / 15 on both builds (inside the builds); Release golden cases OK; the rest not reached | step 1, step 2 |
| 4 P9-B0 | not reached | |
| 5 rho | not reached | |
| 6 re-pin | not reached | |
| 7 store step, scoreboard, G-P, CRT | not reached | |
| 8 whole-wave review | DONE before this agent (WAVE-REV-EARLY, SQL4-ADD; GATE-0 committed the five files) | `wave-review-*.md`, ledger :905, :913 |
| 9 records | incremental | this file, `integration-log.md`, `handoff-root-gate*.md` |

## The rulings: state after chain agent 2 (RESUME 13; rows not listed are as in agent 1's table below)

| ruling | step | state | evidence (`gate-logs/`) |
|---|---|---|---|
| GATE-TMP | 2 | **closed**: the tool's TMP root moved to a short root OUTSIDE the work tree (the ruled `build-equity/pt/<inv>` is refused by K3 `fresh_root` / `no_git_refusal`, both pre-P9; open question for the PM); seed 0 re-run green, the single named test green | `tools/CHANGES.txt`, `pyt-exits.txt` |
| TEST-2 (owner 2026-10-16) | 2 | received after every whole suite of step 2 had completed and been logged: all stand as run; focused checks (a) single test green and (b) the three `cov-diff --legacy` runs done; no new whole-suite invocation started | this file "Chain agent 2" |
| GATE-COVDIFF | 2 | **holds**: COV-9 role check first (S9 0 of 990), then the three bounded `cov-diff --legacy` runs exit 0, "legacy equal: 1405 sessions", no store block | `cov9-role-check.txt`, `covdiff-runs.txt` |
| D3-Q1a | 3 | **DIFFERENT**: admission identical, the engine-fit weights differ from the Python fit in 694 float leaves (`status` "agree"); P9-W lapses at 0 (PRE-35); P9-F0h is produced flag-absent (Python fit) | `d3q1a.txt` |
| COMPOSE-1 (1a) | 3 | **ACCEPT** at the store-backed parse stage on the tiny world (both flags accepted together; stops at the same later stage as the rate-only control); `validate_nav_config` / `validate_nav_input` left to P9-MV's preflight | `compose-1a.txt` |
| ALCLOCK-1 | 3 | **holds**: flag-absent Y-F0 NAV identity; opt-in adds only `/scenarios/*/info_clock` leaves (5 / 5 scenarios) | `nav-identity-yf0.txt` |
| D3-Q2, D3-S1 | 3 | **hold**: 0 / 0 over `scripts/specs/p9/` | `d3-greps.txt` |
| GATE-IDX | 3, 4, 7 | file **still present** at 13:40:01Z: no run naming the cache; step 4 not started: **BLOCKED-OWNER** | `cache-lo3-listing-agent2-start.txt` |
| PY-S S-6 | 7 | **equal** (unscored 63, scored 62, each three ways) | `pys-s6-ledger-n.txt` |
| CRT-2 | 7 | recorded: stores = pins, CRT unchanged | `crt-record.txt` |
| PERM-1 | - | one refusal (the unstage of an editor-side `.mypy_cache`); not retried | `perm-refusals.txt` |

## The "ROOT GATE: rulings" list (`r11-dispatch.md`) and the dispatch rulings: state at OWNER STOP 12

| ruling | step | state | evidence |
|---|---|---|---|
| STORE-M1 (+ STORE-S S-1) | 0 | DONE at GATE-0 (`7320e1b9`) | `handoff-root-gate0.md` |
| IC-S | 0 | DONE at GATE-0: no edit; G-P5 prints "PARTIAL: four pairs unlisted, P10 retirement" | `handoff-root-gate0.md`, `gate0-logs/frozen-pairs-probe.txt` |
| five `wave-review-<area>.md` committed | 0 | DONE at GATE-0 (`22f99018`) | git |
| GATE-EXES | 1 | **holds**: `atx-research-composition` and `atx-research-admission` built Debug (p9-3g) and Release (p9-3f), receipts, in the exe table | step 1 |
| GATE-COVDIFF | 2 | **PARTIAL**: S-7 hand check holds on the three stores; the three `cov-diff --legacy` runs not run; `v8-risk-lo3-v10` cited from M2d | step 2 |
| D3-Q1a | 3 | not reached | - |
| COMPOSE-1 | 3 | not reached | - |
| PRE-MVX | 4 / cells | the prereg's lines (B.7 "Parent", B.8, E) are the PRE agent's (PRE-2BII-PAR part A); nothing for root before P9-B0 | ledger :889, :932 |
| COVMV-S2 S-b + NAV-S S-2 | 6 | not reached | - |
| G-P10 (d) / (e), STORE-S S-4 | 7 | not reached | - |
| Fields area | - | APPROVE, nothing for root; its step-2 runs not reached (fields gtests Release 110 / 110 ran) | step 2 |
| Python area | 2, 7 | its whole-suite runs: seed 0 ran (step 2), the rest not reached; SQL3-2 holds at step 0; PY-S S-6 hand comparison (step 7) not reached | step 2 |
| GATE-IDX | 3, 4, 7 | the file was present at step 0 and at the stop; nothing naming the cache ran | `handoff-root-gate-1.md` |
| GATE-PAR | 2 | builds alone; at most two low-RAM suite invocations at a time (Release gtests beside `scripts/tests` seed 0); the S-7 hashing beside them | `gate-logs/builds.txt`, `gt-exits.txt`, `pyt-exits.txt` |
| PAR-1 | all | root added only its own paths; the PRE agent's `p9-prereg.md` and the IDX agent's file are theirs | git |

## Round-2b-ii facts (Appendix IV items) delivered so far

None of items 1-6, 8, 9, 12, 15, 20, 22, 23, 25, 26 is final yet (P9-B0, the re-pin and rho are not reached). Inputs
already fixed for them: item 4's exe paths, SHA-256 and tags are step 1's table (final only when the re-pin commit
names them); item 25: the `ucrtbase.dll` value at step 0 (10.0.26100.7623 `2C08E77F...65EF`), to be re-recorded at
P9-B0 itself.

Agent 2: item 25's value re-read at its step 0 (2026-10-16): unchanged (10.0.26100.7623 `2C08E77F...65EF`); still to be
re-recorded at P9-B0 itself. Delivered as gate outcomes (not Appendix IV items): D3-Q1a DIFFERENT (P9-W lapses at 0)
and COMPOSE-1a ACCEPT. The items tied to P9-B0, rho and the re-pin stay open (step 4 not started).

Agent 3 (2026-10-16 ~14:30Z; BLOCKED-OWNER at GATE-IDX; by Appendix IV item; a "pending" item is NOT final):

| item | fact | evidence |
|---|---|---|
| 1 R-2 | **PATH A (ruled FIELDS-SUB-A)**: the re-based R-2 of record is `build-equity/p9-b0-r2-nav` (PRE-22 met by the fields substitution; witness 70 / 70), run on fields v15 (listing `04bb8060aadf692a63e7f1baafd23f297db67c24d9c1f0a43dce6678a755b1bd`, `summary.json` `48a52f6adb06c2e966cb366f2c71c9dbef55ba48de243afc2cce01df3085c1b9`); disclosure: it differs from the v8 NAV of record only at the FIELDS / DERIVED / M1d-C1 / C1-COST paths (`r2-classify.txt`); G-B6 and the OD-3 R-2 history line read it (the OD-3 R-2 line is readable: its TRAIN NAV = `p9-b0-r2-nav`) | "Chain agent 3", `r2-classify.txt` |
| 2 Y-F0 u / w / NAV | **pending** (GATE-IDX); new dirs reserved `p9-b0g-yf0-{u,w,nav}` | substitution list rows 4-6 |
| 3 Y-1 | `build-equity/p9-b0g-y1-nav` (store `p9-m2i-dcov-v11`, manifest `397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7`): 28 files, listing `cac882e19a5baee9d3d60581d93a278f9f4fc6b769ed0e9154dd408b1552b813`, `summary.json` `b0d4dcbf53e81aff3863ca1d9c7230da73a5c0a911a60f622c9f78aa3796c028`, `vol_target.csv` `8e614ea7bbf47d363a5c357b7442c7b3a7b86df753879715f5d20bd083f767a7`; PRE-34's byte / JSON-path form vs v8's ledgered Y-1 NAV: differ at 26 / 28 files (PRE-29's store change); the printed-precision agreement is the cells root's (B.7) | `b0-files-p9-b0g-y1-nav.txt`, `y1-cmp-v8.txt` |
| 4 exe table | the step-1 table stands (Release p9-3f; 27 / 27 `disk = receipt` at agent 3's start and before / after every run); final only in the re-pin commit (**pending**, step 6 not reached) | `exe-sha-rel-3.txt` |
| 5 C3-6 | holds, cited from M2j (`handoff-root-m2j.md:44`: X-5 and Y-F0 flag-absent NAV 26 / 27 identical, producer keys only; the opt-in leaves `/scenarios/0..4/ann_mean_gross` only, `handoff-root-m2j-fin.md:44`) | - |
| 6 eval mode | **pending** (read from the re-based Y-F0 u receipt; u not run) | - |
| 8 P9-L parent / reference | = item 2: **pending** | - |
| 9, 12, 15 fit mode | **flag-absent** (D3-Q1a DIFFERENT, P9-W LAPSED (0); LAPSE-TABLE) | `d3q1a.txt`, ledger :975, :981 |
| 20 rho P9-X pair | **pending** (re-based Y-1 vs re-based Y-F0) | - |
| 22 rho P9-V pair | **pending** (before P9-V's read) | - |
| 23 | B8A-PROD: the cells root | ledger :971 |
| 25 `ucrtbase.dll` at P9-B0 | 10.0.26100.7623 `2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF` at the cache-free runs (14:22:31Z; = CRT-2); to be re-recorded at the Y-F0 runs | `ucrt-b0-3.txt` |
| 26 rho of the references | Y-1 vs X-10 (gate refs): 0.9947113980219978, "1 - rho < 1e-6" **false**; the rest **pending** | `rho-pairs-1.txt` |

Beside the items: SQL4 verdict green (in the head, SQL4-CUTOFF-2); D3-Q1a DIFFERENT; COMPOSE-1a ACCEPT; ALCLOCK-1 held;
cut line X-5 (FIELDS-X5); substitution list `778d34b8`; cache listing before any Release pass: 352 files `fb444f7d316a1a4c`
(351 + the GATE-IDX file; without it `155872de50833fca`); no Release pass ran, so no "after" listing.

Agent 4 (2026-10-16 ~16:10Z; FINAL; every item below is final; items not listed are as agent 3's table, which stands):

| item | fact | evidence (`gate-logs/`) |
|---|---|---|
| 1 R-2 | **`build-equity/p9-b0-r2-nav`, the re-based R-2 of record (path A, FIELDS-SUB-A)**; read BY PATH (REPIN-REV): `lib-v80.json` untouched at the re-pin of record (`306a070b`; its fields pin names the absent v10 and its NAV output the v8 dir: stale, stated in the substitution list, PRE-40 form; `3a1a4759`'s re-pin of it was reverted): listing `04bb8060aadf692a63e7f1baafd23f297db67c24d9c1f0a43dce6678a755b1bd`, `summary.json` `48a52f6adb06c2e966cb366f2c71c9dbef55ba48de243afc2cce01df3085c1b9`; the classification table (`r2-classify.txt`) is its disclosure; G-B6 and the OD-3 R-2 line read it; its TRAIN fields dir is v15 (`26fee5ce`), its w the v8 w its argv reads (`mega-v8-r1w-train-std-v80-1/train_combined.json` `bbbf6f2b...cd98`, not re-based) | `b0-files-p9-b0-r2-nav.txt` |
| 2 Y-F0 u / w / NAV | u **`build-equity/p9-b0g-yf0-u`** (5 files, listing `1cb087abaa50746392269b3452491be8fa2999b9a39e1212f01049acec2916aa`, `summary.json` `acbf61cef24e909bb8975668b1b2206708b5c96cf03194a0728564445fae12b8`); w **`build-equity/p9-b0g-yf0-w`** (12 files, listing `ec69344005107bf0e2ab0bb859583db083889521a652dcfe134b2304e27c8ae1`, `summary.json` `f772b394a6885b5965d0c5ad93e4f023515a74ed7e02064a772814357ee904f2`, `train_combined.json` `e13fbc4dab75c00cc47d7a06d8fcfbefd2d1368407565e99001136d19de9da8c`); NAV **`build-equity/p9-b0g-yf0-nav`** (27 files, listing `e205d6ecb16e75cba02194b90b313849404c2052b3e1738c4bf020e486fcf797`, `summary.json` `826d5ec65329c24c4c0c1b72ee1f57dcdbdb136cf00d4113c6463310e79b8f38`; reads the re-based w; `--ann-mean-gross --info-clock-print`); each holds against M1d's reference (RELCACHE-ALLOW / the NAV form); the references of record for provenance (the Release exe, the receipts, the identity); Y-F0's spec keeps `ic.u_output` / `ic.w_output` at the v8-era cycle dirs `mega-v8-b0b-train-u-v8ysb-1` / `mega-v8xw-train-theme-erc-v8ysb-2`, the cycle's resume roots (completed state), byte-identical in every payload (REPIN-REV; `3a1a4759`'s re-pin of them reverted) | `b0-files-p9-b0g-yf0-{u,w,nav}.txt`, `yf0{u,w,nav}-cmp.txt` |
| 3 Y-1 | as agent 3 (`p9-b0g-y1-nav`); the re-pin points `y-vol-target-y-1.json` at it and at the v1.1 store | step 6 |
| 4 exe table of the re-pin | the re-pin of record **`2078f877`** (the follow-up `3a1a4759` and its revert `f8a85ad4` move no exe pin) pins Release p9-3f (source = gate code `06dc9e5a`, receipt Source `5f48a25d`): NAV `build-equity-rel/bin/atx-equity-strategy-targets.exe` `255b58acedf7df82a1caa672ff30a58c317a8f0dccf7dbe7d3b7393b91063565`, IC `build-equity-rel/bin/atx-equity-strategy-ic.exe` `ccd54db605f67fef1aa480050a6c785ffe179dae2f77e978dd6d01a5c117d705`, eval `build-equity-rel/bin/atx-research-eval.exe` `e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c`; every other exe of the frozen set at step 1's table (composition `04db0d67...16f2`, admission `f53d9277...063f`, store `0eeac21b...3818`, risk `0c110534...586b`, fields `883c5b74...584a`); EXE-FREEZE from `2078f877`; 27 / 27 + 27 / 27 `disk = receipt` at the end | `exe-sha-{rel,dbg}-4-end.txt`, step 6 |
| 6 eval mode | **research-fast** (the default): the Y-F0 u receipt's argv carries no `--eval-mode audit-exact`, and the Release VM identity is `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0` without the `_audit` suffix (PATH-1: every cell runs in its reference's mode) | `receipt-p9-b0g-yf0-u.json`, `yf0u-cmp.txt` |
| 8 P9-L parent / reference | = item 2 (`p9-b0g-yf0-nav` the paired parent NAV; the re-pinned `lib-v8ysb-gm.json` names it) | step 6 |
| 20 rho P9-X pair | re-based Y-1 vs re-based Y-F0: **0.9947035686613187**, "1 - rho < 1e-6" **false** | `rho-pairs-2.txt` |
| 22 rho P9-V pair | P9-V pairs with P9-X (PRE-MVX); its references are P9-X's: the P9-X pair above; the cell pair itself is measured at the cells (before P9-V's read) | `rho-pairs-2.txt` |
| 25 `ucrtbase.dll` at P9-B0 | 10.0.26100.7623 `2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF` at the Y-F0 runs (15:39:47Z) = the cache-free runs = CRT-2 | `ucrt-b0-4.txt`, `ucrt-b0-3.txt` |
| 26 rho of the references | P9-X pair 0.9947035686613187 (false); X-10 vs Y-F0 **0.9994863034956717** (false); Y-1 vs X-10 0.9947113980219978 (false) | `rho-pairs-{1,2}.txt` |

Beside the items (final): SQL4 verdict green (SQL4-CUTOFF-2); D3-Q1a DIFFERENT (P9-W LAPSED (0)); COMPOSE-1a ACCEPT;
ALCLOCK-1 held (step 3 and the NAV of record); cut line X-5 (FIELDS-X5); the substitution list `778d34b8` + agent 4's
amendments `bfbe64d2` (row 1 path A; rows 4-6 classes and the NAV's w) and the tool correction `c0112afc`; the re-pin
`2078f877`; the catalog digest `adbcd348b5d82a1fd4f357274a34bbf387391cf82154a961e701bad39a0dd474` (reproduced); the
fixture digest `a50fdcf9...100c` (reproduced); `classes.json` `5bc27ba4...6557`. Cache listing **before** the Release
pass: 351 files `155872de50833fca` (after the owner's GATE-IDX deletion; 352 `fb444f7d316a1a4c` with it); **after**:
552 files `c9ee9291f5ddc0c2` (the 351 unchanged + the 201-file Release identity subtree, 4,466,406,032 bytes).

## Step 0: start state (re-verification only; GATE-0 did the slips)

`gate-logs/start-state.txt` (2026-10-15T20:07Z):

- HEAD `5f48a25d` (docs; IDX's commit above `0a481348`); code head `06dc9e5a`: 0 files differ outside
  `.superpowers/`. Branch `feat/platform-p9-20261003`. `git log --format=%s -40` holds every wave-2 / wave-3 merge
  named in `r11-dispatch.md` "ROOT GATE" through M6b (12 merge subjects in the last 40, SQL4 `06dc9e5a` on top).
- `git status --porcelain`: the five untracked PNGs under `docs/plans/` (not root's, never added) plus the PM's /
  docs agents' tracked edits in the sprint dir (`progress.md`, `r12-dispatch.md`, the PRE agent's `p9-prereg.md`).
  No code path dirty.
- Trial ledger 133 lines, sha256 `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`.
- `build-equity/research-store/` absent (SQL3-2).
- `ucrtbase.dll` (C:\Windows\System32) file version 10.0.26100.7623, SHA-256
  `2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF` = the M2i record of the two Release D-COV
  stores (CRT-2): unchanged. `ucrtbased.dll` 10.0.22000.832 `19E79E91...F48D` (unchanged).
- Host: free RAM 5,776 of 16,069 MB, commit free 4,966 MB; C: 72.8 GB free; no build running (editor-side python
  only).
- GATE-IDX file present (above).

## Step 1: exes of record (EXE-CUR, EXE-FREEZE, GATE-EXES, SPDLOG-RELINK)

Two target-scoped builds through `scripts/research-build.ps1` (wrapper `gate-logs/tools/build.sh`, HOST-4 / HOST-5
admission), sequential, nothing else running beside either, both with `-Canary`. The same 27 targets each: the 11
exes `atx-equity-strategy-targets` (NAV), `atx-equity-strategy-ic` (IC), `atx-equity-strategy`, `-risk`, `-mine`,
`atx-research-eval`, `atx-research-composition`, `atx-research-admission` (GATE-EXES), `atx-research-store`,
`atx-research-fields`, `atx-impl`, and the 16 test targets the suites run (`atx-impl-strategy-{ic,target,mine}-tests`,
`atx-impl-strategy-tests`, `atx-impl-tests`, `atx-engine-{alpha,factory,book,risk}-tests`,
`atx-engine-research-{fields,admission,store,catalog,composition,ledger,eval}-tests`).

| tag | preset | receipt Source (code) | exit | TUs / links | warning lines | canary (G-P8) | wall | evidence |
|---|---|---|---|---|---|---|---|---|
| **p9-3f** | `equity-rel` (Release, `build-equity-rel`) | `5f48a25d` (= code `06dc9e5a`; 8 dirty entries = the sprint-dir docs and the five PNGs, no code path) | 0 | 244 / 34 | **0** | check, Release goldens: **15 passed**, exit 0 | 526 s | `gate-logs/mega-p9-3f-{receipt.json,build.log,canary.log,cache-stats.log}` |
| **p9-3g** | `equity-dev` (Debug, `build-equity`) | `5f48a25d` (same) | 0 | 68 / 19 | **0** | check, Debug goldens: **15 passed**, exit 0 | 183 s | `gate-logs/mega-p9-3g-*`, `porcelain-before-p9-3g.txt` |

SPDLOG-RELINK: after the last link every exe of both trees was re-hashed against its newest receipt
(`gate-logs/tools/exe_sha_end.py`): **27 / 27 Release and 27 / 27 Debug `disk = receipt`** (`gate-logs/exe-sha-rel.txt`,
`exe-sha-dbg.txt`). Superseded by these builds: every earlier root exe (p9-2p .. p9-3e Debug, p9-2q / p9-2s Release).
Release is the producer of record (OD5-2, M2c-RED-2 (e)). EXE-FREEZE starts at the re-pin (step 6); it covers the NAV,
IC, eval, composition, admission and store exes (GATE-EXES).

### Exe table (tag, SHA-256; source = gate code head `06dc9e5a`, receipt Source `5f48a25d`)

| exe | Release p9-3f (`build-equity-rel/bin`) | Debug p9-3g (`build-equity/bin`) |
|---|---|---|
| atx-equity-strategy-targets (NAV) | `255b58acedf7df82a1caa672ff30a58c317a8f0dccf7dbe7d3b7393b91063565` | `5dced610e204ba860bd2c8c8da5c3c6bd58a1ef98c242bec352a6b64d8762a8f` |
| atx-equity-strategy-ic (IC) | `ccd54db605f67fef1aa480050a6c785ffe179dae2f77e978dd6d01a5c117d705` | `adffe7421781145a9718e67be05b46038b89a46311f26390eeb3312da236d56d` |
| atx-research-eval | `e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c` | `4791019131c0d74603b83cb772ef027ec8641167fd95844c4674e75bdf39893b` |
| atx-research-composition | `04db0d67c1563184edd6fe8ff66796877c8db20239d5cfb423fa78e047df16f2` | `212f8b9941f0be705ebac138c03e83279fc863ab12051f00dd09501db7eed98e` |
| atx-research-admission | `f53d9277e4ad2e3915343ccca032df3f0098a1a1cd7b4d832b640f5f7437063f` | `411af33a0def5a6c28983dbf1ffc2fbc324694e8ac2e6b24401b27b693c9fa26` |
| atx-research-store | `0eeac21bb95677d8da5268ca829b88dc668521a4c9f625e0fbbde92f96fb3818` | `a54955e965ee2aa035e66cc9ed668a16246c089684a53515431dea428b605c73` |
| atx-research-fields | `883c5b74a95731eb310cfd30cb1ab39ee102e077ab2d7318bd5db941f486584a` | `1c670d86a0fdcae6a86b0eff56a184902d1eed8b244f8af6ab31425e18185fac` |
| atx-equity-strategy | `f883f81f0fbb928cb8fe2e8b1931841e0eef058e7b6398810e10de68cb0495a4` | `47d341c2541e792873ec5d3db0b739af6de61e9962ba46667c755a1ca56ca9a4` |
| atx-equity-strategy-risk | `0c110534dd2df43e995040a07e5368eac788166b5e55cc2e0e2fa5e088be586b` | `6696da719374777aefb49ce9e6b4707187549dff9b838e0c2338d21e852b47a1` |
| atx-equity-strategy-mine | `daf889437983628ccbc375e76a078d3decf079a9d3a79b6cca79cbcc4a063eba` | `4dee20571d4c4481bcf175d8bb688574ba95c60ae29d08b4bb474849f7945412` |
| atx-impl | `71f0c6311aaffffaa48f4c221c18c94e009a3b7ebc5a45798baff009d75bbdb9` | `7095d4477dd7fee80ead2d9bbdb516a726ed3ab943beed95ac1dc5335af63e55` |
| atx-impl-strategy-ic-tests | `9e979065f2b56f135c819baab9da80c89e5745b6ba249c284c428d3497d0fb5b` | `55db8666c03533439ecadd10d9412f4c65167581e0a11a806de2572ea69ebd6b` |
| atx-impl-strategy-target-tests | `5e781705c1fab53c42cb49d0777753a1f792ff022847923f4dce901411cd6db5` | `18f3855468ca76c083eae9fabdb8d0cd1a237996ac8bba3136ed06782eb74318` |
| atx-impl-strategy-mine-tests | `2ae349c689f9423f2847ffbb63a3ad9598b4c26aab0ca63136dc16a83019fa68` | `7502d0fb39e125a9cce9feabab4c35a8da5681a37eacad974b793c555822abb0` |
| atx-impl-strategy-tests | `0ae6ab3f4a09af6a33d4dec3f6eba8fe8324e7c3ea85aaae6f745cb5e03712e1` | `6dc6d4426370e256dc1e1331292e84da180a292cb72c86456c70eb9255246fba` |
| atx-impl-tests | `9f4cf8076d17b93891a70fb5944e3f43975230a84bc0a82a1e8fb46d90e0b545` | `20f3e2378ea4373e4812ba6c1d5bf1257945b2c5a9488cabeb6df04071f523db` |
| atx-engine-alpha-tests | `534efe4928e84ae3f9fcc9f0ed79b7e823b51689987edd84c513b6ba7917bf56` | `3f8f1c7ce851998d3b2e515cdcbc9ae1634a61550ac2d975a3828850356c9637` |
| atx-engine-factory-tests | `6bf91318724f77c0413dfaada365708d4360fbc754320f05a9e074655575483c` | `a9d538ae4d922afa26c78da60e7114b6cabcf78dc32c3274d7f698cf339354d6` |
| atx-engine-book-tests | `88f195dba54faa428eed45b4d53cf64d7e8f8a28702c7f3b3d68dd80ab4af0ac` | `6977a5a75b386d283c81e148c0fec31a2da1f95536dc98cab2366d424d1f8360` |
| atx-engine-risk-tests | `b654425d22fc671b3b24a7dbbb35ccad4288b655afb8d1925853362ca624e99d` | `0008934517be7b3a36eede7eae65fde92cab50dab10b9835af585186e26bbbb1` |
| atx-engine-research-fields-tests | `165ebeea3d7f01fb330856240c12d125b18c0a491bac66183ab9a229c49b3651` | `a037964fc902f143541d204624e9bd31e0249c7e3a216bd9dac325ab1d47ef7d` |
| atx-engine-research-admission-tests | `39c4a81bda54df9712e2e818684cb78293486e7d664dec8b308883c505da1581` | `310aa78298b1bbb75c5d6104f1af581aa962c909cccfdd846d5b6628e8200a62` |
| atx-engine-research-store-tests | `bf9c312941fc5d8668bb7024c476f915138679fb26f3f3fa21fd88e94d953d26` | `d2330e4b387b832545ada353706c62cb41c02981bff61728365ac7329713661f` |
| atx-engine-research-catalog-tests | `c996b733b5b92f58bee1760a0c7505ec6349c3847bb392d5014e762b58dfefc5` | `b73b2199db7fd1bc451bd12872a5eb0851453db4299ed29e812dbbd151d9835a` |
| atx-engine-research-composition-tests | `17cee392663a5df0432e3293bec06bb5126f6931672b0d827cdaa0e336a731f6` | `76b664b7483a77a717015a3fdebb38f97dc39fbcfecd04a98e52d3cd016023a9` |
| atx-engine-research-ledger-tests | `bfe2afc39770a316810ea5f33650e6d15f66ad013374b123ea16e42f8865bf94` | `8c5a6065f31f061fe6a06b474b0bd9987f45a640c5f59a9ade4b61b25fe322cd` |
| atx-engine-research-eval-tests | `2b9910b3665798c6d93cca37998580db5942cc7284b88cfef4b9830b946a4a02` | `58b5681bc8373e9a1ddf0a84602d5502acc01070708a09ddc52d747106ec3557` |

Several Debug exes have the same bytes as their last root build (the IC exe `adffe742` = p9-3e, the eval verb
`4791019` = p9-2z, composition / admission = p9-3a, book tests = p9-2y): ninja relinked nothing whose inputs did not
change; the receipts name them under p9-3g regardless.

## Step 2: suites (PARTIAL at OWNER STOP 12; plan 4.2 "suites", GATE-COVDIFF, GATE-PAR)

### Release gtests, whole targets (p9-3f; `gate-logs/tools/suite_gt.sh rel`; ENV-1 0 hits; `gate-logs/gt-exits.txt`, `gt-rel-<target>.log`)

| target | ran | passed | skipped (names) | failed | disabled |
|---|---|---|---|---|---|
| atx-impl-strategy-ic-tests | 261 | 261 | 0 | 0 | 0 |
| atx-impl-strategy-target-tests (incl. the `Cov*`, `MvAimRule.*`, `InfoClock*` suites) | 421 | 421 | 0 | 0 | 1 |
| atx-impl-strategy-tests | 46 | 46 | 0 | 0 | 0 |
| atx-impl-strategy-mine-tests | 44 | 44 | 0 | 0 | 0 |
| atx-impl-tests | 1,272 | 1,267 | 5: `Alpha101Orats.RankByOptimizerSharpe`, `Alpha101Orats.RankBySharpeRiskParity`, `AtxImplDiscover.W6_RediscoverLowVolCapacityAlpha`, `FundamentalZoo.RealDataIcReport`, `SingleAlphaCapacity.SweepAndVerify` (the same five as M2j's Release / Debug whole runs) | 0 | 0 |
| atx-engine-alpha-tests | 782 | 780 | 2: **`AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`** (OD5-2 named skip), `AlphaVm_ZeroAlloc.WarmDispatchLoop_BoundedByOutputsAndSizeIndependent` (Release-only skip, the same as S2R's Release run `m2f-logs/s2r-rel-alpha-whole.log`) | 0 | 1 |
| atx-engine-factory-tests | 396 | 396 | 0 | 0 | 0 |
| atx-engine-book-tests | 197 | 197 | 0 | 0 | 0 |
| atx-engine-risk-tests `Risk*:-RiskQpAugment.MatchesDenseOracleAcrossBattery` (incl. `RiskCovContainer.*`) | 432 | 430 | 2: `RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery`, `RiskSectorColumnsById.CheckedIndexingCatchesThePreW0Read` (the same two as M2i's Release run) | 0 | 0 |
| atx-engine-research-fields-tests | 110 | 110 | 0 | 0 | 0 |
| atx-engine-research-admission-tests | 16 | 16 | 0 | 0 | 0 |
| atx-engine-research-store-tests | 27 | 27 | 0 | 0 | 0 |
| atx-engine-research-catalog-tests | 30 | 30 | 0 | 0 | 0 |
| atx-engine-research-composition-tests | 21 | 21 | 0 | 0 | 0 |
| atx-engine-research-ledger-tests | 16 | 16 | 0 | 0 | 0 |
| atx-engine-research-eval-tests | 27 | 27 | 0 | 0 | 0 |

**0 failed in 4,098 Release tests; every skip a pre-existing named one.** Golden `0x889874a3b9b29c55` (step 3 item,
recorded here): Release `SignalFitnessDefaults.ImplicitDefaultsKeepTheGoldenDigest` and
`...ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount` (workers 1 and 4; mine-tests) and factory-tests
`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `NsgaSearch.ScalarRaw_DigestInvariantAcrossWorkers` (1, 2, 4, 8),
`NsgaSearch.DeflateSelection_DefaultIsOff_ByteIdentical` all OK (the M1d form). Debug side not run yet.

### Pytest (Release exes, `gate-logs/tools/pyt.sh` with `GATE_BUILD=rel`)

| invocation | seed | result | log |
|---|---|---|---|
| `scripts/tests` (whole) | 0 | **exit 1**: 1 failed, 702 passed, 3 skipped; wall 36,486 s from 2026-10-15T20:20:25Z (spans OWNER STOP 12); ENV-1 0 hits. The one red: `test_cycle_e2e.py::test_cycle_e2e_goldens_ic_reference_redundant_copy_and_idempotent_rerun`, `research_cycle.py` exit 2 at plan time: "IC cache path too long (ruling W1-I1): ... up to 327 characters (> 250)" under the tool's TMP root in the session scratchpad = GATE-TMP's cause; re-run pending (GATE-TMP) | `gate-logs/pyt-scripts-tests-rel-s0.log`, `pyt-exits.txt` |

### GATE-COVDIFF

- S-7 hand check (read-only, `gate-logs/tools/store_files_check.py`, `gate-logs/covdiff-s7-files.txt`): **holds** on
  all three container stores: `p9-m2i-dcov-v11` (manifest `397366473b3a...` = the M2i pin; container
  `model.atxcov` `cd48d574...` root `b39a1b77...`), `p9-m2i-dcov-covv1` (`ea58691fc407...` = pin; `0f90bf7c...` root
  `20919f7b...`), `p9-m2d-cov-risk-container` (`9c12852ea0cd...` = the pin the M2i Y-1 NAV receipt names;
  `0a04b233...` root `5462e923...`): every one of the 11 listed files and the container equal the manifest's bytes
  and SHA-256; no unlisted file.
- The three `cov-diff --legacy` bounded runs: **not run** (OWNER STOP 12 came first). `v8-risk-lo3-v10`'s identity
  with its container is cited, not re-run (as ruled): `handoff-root-m2d.md:23`, `m2d-logs/cov-diff-legacy.log`.

### Not run (OWNER STOP 12)

Debug gtests whole; `ctest -L atx_research` / `-L atx_equity_strategy` on both trees; `scripts/tests` seed 1; the stage
chain; `atx-engine/tools`; `atx-impl/tools`; `atx-impl/strategies`; the fields fixture pytests; the mirror guard alone;
the three cov-diff runs. Commands: `handoff-root-gate-1.md` "Exact next step".

---

## Chain agent 2 (RESUME 13, 2026-10-16; `r13-prompts/gate-prompt-2.md`; rulings R13-RESUME, GATE-TMP, GATE-RELCACHE, COMPOSE-1a, HOST-6)

Started from `handoff-root-gate-1.md`. First act (R13-RESUME, DIRTY-1 form): agent 1's five placeholders (and the
sixth, `@@PY_S0_LOG@@` in its `integration-log.md` row) filled from `gate-logs/gt-exits.txt` / `pyt-exits.txt` only;
agent 1's records and the PM's files committed as they stood: docs commit `0c8bc8da`.

### Step 0 re-verification (agent 2; `gate-logs/start-state-2.txt`, 2026-10-16T12:36:59Z)

- HEAD `0c8bc8da` (docs); code head `06dc9e5a`: `git diff --name-only 06dc9e5a..HEAD` outside `.superpowers/` = 0
  files. `git status --porcelain` = the five untracked PNGs only. First-parent merges down from `06dc9e5a`: SQL4,
  SQL3 item 5, AL-CLOCK fix 1, A4 pre-flip, SIGN x3, D3W-PB x3, OD3P follow-up, B2 fix 2, COV-MV, AL-CLOCK (14 listed).
- Trial ledger 133 lines, sha256 `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`.
- `build-equity/research-store/` absent (SQL3-2).
- GATE-IDX file `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` **still present** (73,728 bytes, sha256
  `3a177dfd...c6f0`).
- `ucrtbase.dll` 10.0.26100.7623 `2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF` (= CRT-2 record,
  unchanged); `ucrtbased.dll` 10.0.22000.832 `19E79E91...F48D` (unchanged).
- Host: free RAM 4,965 of 16,069 MB, commit free 9,034 MB, C: 67.8 GB free; our build processes 0; foreign: the
  atx-vol sessions of pools 33 / 34 (HOST-6: a `cmake --build C:\atx-wt\pool-34\build-vol-rel-avx2` and a pool-33
  quote benchmark started during the suites; never touched).
- SPDLOG-RELINK re-hash (`gate-logs/tools/exe_sha_end.py`): **27 / 27 Release (p9-3f) and 27 / 27 Debug (p9-3g)
  `disk = receipt`**: the exe table of step 1 stands, no re-receipt needed; next free tag `p9-3h` unused.

### GATE-TMP: the tool change and where the short root sits (`gate-logs/tools/CHANGES.txt`)

`gate-logs/tools/pyt.sh` now takes a short invocation name and puts the basetemp (`<root>/b`) AND TMP / TEMP of every
invocation under ONE fresh short root, deleted by literal name after its record (the one logged change). The ruled
root `C:/atx-wt/pool-2/build-equity/pt/<invocation>` lies **inside pool-2's git work tree**, where the code under test
refuses a tiny-world root by design: `scripts/tests/test_cycle_e2e.py:212-217` (`fresh_root`: "temporary root ... lies
inside the repository") and `scripts/research_tree.py:75-81` (`no_git_refusal`: `research_cycle.py --no-git`, contract
K3, only for a root outside any git repository; the `--no-git` tests root tiny worlds under the basetemp too). Probe on
the ruled root, the one e2e node (`GATE_PT=ruled`): **1 failed**, `RuntimeError: temporary root
C:\atx-wt\pool-2\build-equity\pt\pr\t\tiny-world-luarx0j_ lies inside the repository`
(`gate-logs/pyt-tmp-probe-ruled-root-s0.log`; probe root deleted by literal name). Decision within GATE-TMP's purpose
(a short TMP root so W1-I1 does not refuse; the slip is the tool's, not the code's): every run of record uses the
short root `C:/Users/natha/AppData/Local/Temp/g2<inv>` (the OS temp dir, outside any work tree, where pytest and
`tempfile` put these tiny worlds when nothing overrides TMP, as at every ROOT-1 run before agent 1): deepest tiny-world
IC cache path 247 <= 250 (agent 1's refusal: 327 with a 138-character root; the tail is 189).

### Step 2, agent 2: `scripts/tests` whole, seeds 0 (again) and 1 (GATE-TMP; Release exes, `GATE_BUILD=rel`)

Run side by side (GATE-PAR: separate roots `g2s0` / `g2s1`), beside the Debug gtests; head `0c8bc8da` (code
`06dc9e5a`), code-dirty none, ENV-1 0 hits each (`gate-logs/pyt-exits.txt`).

| invocation | seed | result | wall | log |
|---|---|---|---|---|
| `scripts/tests` (whole), agent 1's run (TMP under the session scratchpad) | 0 | exit 1: 1 failed (GATE-TMP's cause), 702 passed, 3 skipped | 36,486 s (spans OWNER STOP 12; not a timing finding) | `pyt-scripts-tests-rel-s0.log` |
| probe: the e2e node alone on the ruled root inside the work tree | 0 | 1 failed: `fresh_root` "lies inside the repository" (the test's own guard; see GATE-TMP above) | 1 s | `pyt-tmp-probe-ruled-root-s0.log` |
| `scripts/tests` (whole), re-run on the short root `g2s0` | 0 | **exit 0: 703 passed, 3 skipped, 0 failed**; `test_cycle_e2e.py` 15 / 15 incl. the GATE-TMP case | 1,098 s | `pyt-scripts-tests-rel-s0-rerun.log` |
| `scripts/tests` (whole), short root `g2s1` | 1 | **exit 0: 703 passed, 3 skipped, 0 failed** | 1,096 s | `pyt-scripts-tests-rel-s1.log` |

GATE-TMP closes: the one red of agent 1's run was the tool's TMP root; on the short root the case passes at both
seeds. No red to attribute. The 3 skips are all in `scripts/tests/test_research_cycle.py`, opt-in live-root cases
(a `-rs` run of that file, 93 passed / 3 skipped, `gate-logs/pyt-research-cycle-rs-rel-s0.log`): `:56`, `:706`, `:946`
"set RESEARCH_CYCLE_LIVE_ROOT to hash the real pool-2 files" (the same class as M2j's 4 skips; named, pre-existing).

### Step 2, agent 2: the other pytest invocations (each its own; Release exes; seed 0; `gate-logs/pyt-exits.txt`)

Two chains side by side (`tools/pyt_batch.sh A|B`, GATE-PAR), beside the Debug gtests; head `aef2e033` (the PRE
agent's pathspec commit of `p9-prereg.md` above root's `0c8bc8da`; code `06dc9e5a`), code-dirty none, ENV-1 0 hits.

| invocation | result | log |
|---|---|---|
| `atx-engine/tools/test_stage_chain.py` alone | **10 passed** | `pyt-stage-chain-rel-s0.log` |
| `atx-engine/tools` (the other 42 `test_*.py`, explicit paths; without the stage chain and the two fields files below) | **391 passed** | `pyt-engine-tools-rel-s0.log` |
| `scripts/tests/test_no_python_mirror.py` alone (IC review item 2) | **7 passed**; "G-P5: 0 mirrored rule function(s) in 0 row(s) left for P9 lanes" | `pyt-mirror-guard-rel-s0.log` |
| `atx-impl/strategies` | **14 passed** | `pyt-impl-strategies-rel-s0.log` |
| fields: the holdings pair (`research_fields_holdings/test_holdings_{engine_path,fixture}.py`) | **4 passed** | `pyt-fields-holdings-pair-rel-s0.log` |
| fields: the four `research_fields/test_*.py` | **26 passed** | `pyt-fields-four-rel-s0.log` |
| fields: `atx-engine/tools/test_research_fields_holdings.py` alone | **16 passed** | `pyt-fields-tools-holdings-rel-s0.log` |
| fields: `atx-engine/tools/test_field_registry.py` alone | **37 passed** | `pyt-field-registry-rel-s0.log` |
| `atx-impl/tools` (g2 TMP) | exit 1: **1 failed**, 769 passed: `test_mega_report_v8.py::test_v7_config_renders_the_pre_v8_bytes`, `assert 109873 == 109868` | `pyt-impl-tools-rel-s0.log` |

The `atx-impl/tools` red is the gate tool's again, not the code's: that test asserts the rendered html's LENGTH
(`V7_GOLDEN_BYTES` 109868) before it normalises the `tempfile.mkdtemp` root out of the html, so it holds only for a TMP
of exactly 33 characters (the OS default `C:\Users\natha\AppData\Local\Temp`); the g2 TMP is 38 characters: +5, the
difference. The node alone (no load): on the g2 TMP 1 failed (109873, deterministic); on the OS TMP 1 passed
(`pyt-mega-v7-node-{g2tmp,systmp}-s0.log`; `tools/CHANGES.txt` second entry, mode `GATE_PT=systmp`). Not attributed
to a merge; P10 note (the test should normalise before it measures). The whole `atx-impl/tools` run of record is the
re-run alone under `GATE_PT=systmp`:

| invocation | result | log |
|---|---|---|
| `atx-impl/tools` (whole), alone, basetemp `g2i2`, TMP = the OS temp dir | **exit 0: 770 passed, 0 skipped, 0 failed** (the fitter `RealExecutables` and engine-fit real-exe cases ran on the Release admission / composition exes: 0 skipped; the n37 test on the Release verb passed, B2-N37 item 4) | `pyt-impl-tools-systmp-rel-s0.log` |

### Step 2, agent 2: Debug gtests, whole targets (p9-3g; `tools/suite_gt.sh dbg`; `gate-logs/gt-exits.txt`, `gt-dbg-<target>.log`)

Every exe re-hashed `disk = receipt` before the run (step 0 above); ENV-1 0 hits.

| target | ran | passed | skipped (names) | failed | disabled |
|---|---|---|---|---|---|
| atx-impl-strategy-ic-tests | 261 | 261 | 0 | 0 | 0 |
| atx-impl-strategy-target-tests | 421 | 421 | 0 | 0 | 1 |
| atx-impl-strategy-tests | 46 | 46 | 0 | 0 | 0 |
| atx-impl-strategy-mine-tests | 44 | 44 | 0 | 0 | 0 |
| atx-impl-tests | 1,272 | 1,267 | 5 (the same five real-data cases as Release) | 0 | 0 |
| atx-engine-alpha-tests | 783 | 783 | 0 (Debug runs `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`, the Debug-CRT golden, and `AlphaVm_ZeroAlloc.*`: both OK) | 0 | 1 |
| atx-engine-factory-tests | 396 | 396 | 0 | 0 | 0 |
| atx-engine-book-tests | 197 | 197 | 0 | 0 | 0 |
| atx-engine-risk-tests `Risk*:-RiskQpAugment.MatchesDenseOracleAcrossBattery` | 432 | 431 | 1: `RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery` | 0 | 0 |
| atx-engine-research-fields / admission / store / catalog / composition / ledger / eval tests | 110 / 16 / 27 / 30 / 21 / 16 / 27 | all | 0 | 0 | 0 |

**Debug: 16 / 16 targets exit 0; 4,099 run, 0 failed, 6 skipped (all named, pre-existing).** Golden
`0x889874a3b9b29c55` (`kGoldenDigest`, `atx-engine/tests/factory/factory_nsga_search_test.cpp:77`,
`factory_signal_fitness_test.cpp:79`): `SignalFitnessDefaults.ImplicitDefaultsKeepTheGoldenDigest`,
`...ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount` (1 and 4 workers), `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
`NsgaSearch.ScalarRaw_DigestInvariantAcrossWorkers`, `NsgaSearch.DeflateSelection_DefaultIsOff_ByteIdentical`: **OK
on Debug and on Release** (step 3's golden item holds).

### Step 2, agent 2: `ctest -L` through the wrapper (`tools/ctest_label.sh`; `scripts/atx-build.ps1 -Ctest -Jobs 1`; `gate-logs/ctest-exits.txt`)

| tree | label | result | log |
|---|---|---|---|
| Release `build-equity-rel` | `atx_research` | 100% passed, 0 failed out of 291 | `ctest-rel-atx_research.log` |
| Release | `atx_equity_strategy` | 100% passed, 0 failed out of 728; 1 "Not Run (Disabled)": `atx-impl-strategy-target-tests.CovStageDigest.DumpPerStageOnTheLaneFixture` (the target's one DISABLED test) | `ctest-rel-atx_equity_strategy.log` |
| Debug `build-equity` | `atx_research` | 100% passed, 0 failed out of 291 | `ctest-dbg-atx_research.log` |
| Debug | `atx_equity_strategy` | 100% passed, 0 failed out of 728; the same 1 disabled | `ctest-dbg-atx_equity_strategy.log` |

G-P4 (CTest registration): both labels register and run on both trees (291 / 728 tests).

### TEST-2 (owner 2026-10-16; ledger :972-973; the PM's message to this agent) and what it changes here

TEST-2 cuts every remaining whole-suite item of step 2. It reached this agent AFTER every item of its cut list had
already completed and been logged (Debug gtests whole, the four `ctest -L` runs, `scripts/tests` seed 1 and the
seed-0 re-run, the stage chain, `atx-engine/tools`, `atx-impl/tools` (and its re-run), `atx-impl/strategies`, the
fields pytests, the mirror guard alone; the times are in `gt-exits.txt`, `ctest-exits.txt`, `pyt-exits.txt`, last
line 13:25Z). Per the PM's message ("anything of step 2 you have ALREADY completed and logged before reading this:
keep it in the report as run; stop nothing mid-invocation, start NO new whole-suite invocation") they stand above as
run; no whole-suite invocation was started after it. Cut list items not run at the gate: **none left** (all had run).

Focused check (a), GATE-TMP's one red as the SINGLE test, Release exes, seed 0:

| root | basetemp / TMP | result | log |
|---|---|---|---|
| the ruled short root `C:/atx-wt/pool-2/build-equity/pt/pr` (inside pool-2's work tree) | `pt/pr/b` / `pt/pr/t` | 1 failed at the test's own setup: `fresh_root` "temporary root ... lies inside the repository" | `pyt-tmp-probe-ruled-root-s0.log` |
| the short root outside the work tree `C:/Users/natha/AppData/Local/Temp/g2e1` | `g2e1/b` / `g2e1` | **1 passed** (5.9 s) | `pyt-gatetmp-single-g2-s0.log` |

ROOT-1 attribution of the ruled-root failure: the refusing lines are `scripts/tests/test_cycle_e2e.py:212-217`
(`fresh_root`, commit `d6083b33`, 2026-09-29) and `scripts/research_tree.py:75-81` (`no_git_refusal`, `fa61c67f`,
2026-09-29), both ancestors of the wave-1 gate head `3c9eeb01`: no wave-2 / wave-3 merge commit touches them, so no
lane is attributable; it is the test's v8-era precondition (contract K3: a tiny-world root outside any git
repository), which no root inside `C:/atx-wt/pool-2/` can meet. **Verdict (a): the seed-0 record is green with the
cause stated** (the GATE-TMP red was the tool's TMP root; the case passes alone on a short root outside the work
tree and inside both whole runs above). Open question for the PM: GATE-TMP / TEST-2 name a root inside the work tree;
this agent used the nearest root that the test admits (stated, not a code change).

Focused check (b), GATE-COVDIFF: the COV-9 role check first (`gate-logs/cov9-role-check.txt`, 13:26:52Z: role
manifest `e1c67101...95f4`, `universe.delisting.returns_applied = false`, 0 of 990 events true: **S9 HOLDS**), then
`atx-equity-strategy-risk cov-diff --legacy <store>` on the step-1 Release exe (p9-3f `0c110534...586b`, = receipt
before and after) through the bounded runner (`--seconds 600 --max-rss-mib 2048 --build-type Release`; alone on the
host for our side; `tools/covdiff_run.sh`; `gate-logs/covdiff-runs.txt`, `covdiff-<name>-stdout.log`,
`receipt-p9-gate-covdiff-<name>.json`):

| store | runner | verb exit | the verb's line | `store` block (SQL3-2) |
|---|---|---|---|---|
| `build-equity/p9-m2i-dcov-v11` | completed, 1.3 s, peak 501 MiB | **0** | `legacy equal: 1405 sessions factor_covariance.f64 specific_variance.f64 style_exposures.f32 industry_slot.u8` | none (receipt, start) |
| `build-equity/p9-m2i-dcov-covv1` | completed, 1.3 s, peak 501 MiB | **0** | the same line | none |
| `build-equity/p9-m2d-cov-risk-container` | completed, 1.6 s, peak 501 MiB | **0** | the same line | none |

With agent 1's S-7 hand check (every manifest `files` entry and the container equal the bytes on disk; manifests =
pins) and `v8-risk-lo3-v10` cited from M2d (`handoff-root-m2d.md:23`, `m2d-logs/cov-diff-legacy.log`: "legacy equal:
1405 sessions ...", exit 0; not re-run, as ruled), **GATE-COVDIFF holds**. Focused check (c): D3-Q1a's and
COMPOSE-1a's own evidence is their runs (step 3); no extra gtest filter was needed.

**Step 2 state: HOLDS.** 0 failed in every suite of record (Release and Debug gtests whole, both ctest labels on both
trees, `scripts/tests` seeds 0 and 1, the stage chain, `atx-engine/tools`, `atx-impl/tools`, `atx-impl/strategies`, the
fields pytests, the mirror guard), named skips only; the two reds seen were the gate tool's TMP root (GATE-TMP), each
shown so; GATE-COVDIFF holds. Known-red set EMPTY.

### Step 3, agent 2: identity (SHA-256 and JSON path only; no statistic read)

**Reference inputs missing on disk (a finding for the owner / PM; `gate-logs/inputs-check.txt`, `tools/inputs_check.py`,
names only):** the fields directories `build-equity/train-2020-2023-lo3-fields-v10/` and `-fields-v13/` no longer
exist (their builders' receipts `-v10-run/`, `-v13-run/` remain; `-v9`, `-v14`, `-v15a` are gone too; `-v11` and `-v15`
remain). `disk-audit.md:135` (2026-10-03) lists v9 / v10 / v13 / v14 / v15 / v15a as one hard-linked family ("v13 is the
P6 pin `e5f7f28c...` that X-5 and D-1 read"); M6b's X-5 runs read v13 on 2026-10-15 ~19:30Z, so the directories went
after that; nothing in this agent's or agent 1's records deletes them, and no ledger line records it. Missing inputs of
gate runs: X-5's NAV reference argv (`p9-c1-s2-x5-nav-run`: `--fields .../fields-v13/manifest.json`), X-5's u / w
(`p9-m6b-x5-{u,w}-post-run`: `--train-fields .../fields-v13`), **R-2's NAV argv** (`mega-nav-v8-r1-std-...-L1.1474-v80-run`:
`--fields .../fields-v10/manifest.json`), and the build argv of the three container stores (`p9-m2i-dcov-v11`,
`-covv1`, `p9-m2d-cov-risk-container`: `--fields .../fields-v10/manifest.json`; the stores themselves are pinned bytes
and read fine: GATE-COVDIFF above; B8A-PROD's step 2 in P9-V's cell re-runs that build argv, so it needs v10). Every
Y-F0 / Y-1 / X-10 input (fields v15) is present. Not recreated (a fields build reads atx-db inputs: A4-DB / OD3-DB).

| item | runs (bounded runner, `--seconds 600 --max-rss-mib 2048`, Release p9-3f exes; `gate-logs/nav-runs.txt`, `d3q1a.txt`) | result | evidence |
|---|---|---|---|
| X-5 NAV flag-absent identity, argv and `--spec` (C3-6 / M2J-2 reference `p9-c1-s2-x5-nav`) | `p9-gate-x5-nav-{argv,spec}` | **not run: refused before any work**, exit 1 `InvalidArgument: nav replay: fields manifest missing/oversized` (fields v13 absent); nothing written | `nav-runs.txt`, `receipt-p9-gate-x5-nav-*.json` |
| Y-F0 NAV flag-absent identity, argv (reference `p9-b0-yf0-nav`: the wave-1 P9-B0's Debug exe `407c34ad...`) | `p9-gate-yf0-nav-argv`, 22 s, peak 583 MiB, no `store` block | **holds**: 26 / 27 files byte-identical; `summary.json` differs only at `/producer/build_type`, `/producer/engine_git_sha` | `nav-identity-yf0.txt` |
| Y-F0 NAV flag-absent identity, `--spec` (NavSpec printed by the Release exe: `yf0.navspec.json` `a793e183...`) | `p9-gate-yf0-nav-spec` | **holds**: the same 26 / 27, the same two `/producer` leaves | `nav-identity-yf0.txt` |
| ALCLOCK-1 opt-in (`--info-clock-print` on Y-F0's argv) | `p9-gate-yf0-nav-clock` | **holds**: same 27 files; only `summary.json` differs, 70 leaves all under `/scenarios/<i>/info_clock` (5 of 5 scenarios), opt-in side only; minus those blocks = the flag-absent document | `nav-identity-yf0.txt`, `tools/clock_optin_cmp.py` |
| Y-F0 fit flag-absent identity (reference `p9-b0-yf0-fit`; the fit receipt's argv, `--output` only changed) | `p9-gate-yf0-fit-py`, 1.6 s | **holds outside the code-identity stamps**: `admission.csv` byte-identical; `admission.json` only `/inputs/script_sha256` (reference only) -> `/inputs/producer_fingerprint` (new only); `composition_weights.json` only `/provenance/{admission_sha256, script_sha256 -> producer_fingerprint, std/module_sha256 -> std/producer_fingerprint, theme_erc/module_sha256 -> theme_erc/producer_fingerprint}`; attributed by merge commit to `61378337` (D2: `d61f73c2` replaced the script SHA stamps, CM-5, as M2d's D2 root item (iii) recorded) | `fit-identity-yf0.txt` |
| D3-Q1a engine-fit identity (D3-Q1, D3-Q1a) | signals index `p9-gate-yf0-sig` (67 candidates, roster order, payloads of the real cache's Debug entries; `signals.json` `410900f5...`), Release `factors` -> `p9-gate-yf0-factors` (1004 decisions x 67, 0 refused; manifest `21b9f198...41e1`), `--engine-fit --engine-factors <it> --admission-exe <Release f53d9277...> --composition-exe <Release 04db0d67...>` -> `p9-gate-yf0-fit-eng` | **DIFFERENT**: `admission.json` = (`2d5abfd7...`) and `admission.csv` = (`c3933116...`) byte-identical; `composition_weights.json` minus `$.provenance.engine_fit` (canonical JSON): Python `f2454589...b876` vs engine `decc2f8c...55a8`: 694 float leaves differ (0 non-float; no key on one side only): `/weights/*` (47), `/provenance/candidates/*/weight`, `/provenance/themes/*`, `/provenance/theme_erc/*` (covariance, ERC shares / risk contributions, sleeve vols / correlations, weights before cap), `/theme_standardise/theme_erc/covariance/matrix/*/*`, `/provenance/blend_in_sample_TRAIN_diagnostic/factor_mean`, `/provenance/weighted_standalone_turnover`; `provenance.engine_fit.status` = "agree", `fit_max_rel_float_diff` **non-zero** (within P12; value not read) | `d3q1a.txt`, `receipt-p9-gate-yf0-*.json`, `tools/d3q1a.py` |

**D3-Q1a outcome (ruled before the run, ledger :726, :898): P9-W LAPSES at 0 trials (PRE-35); P9-F0h is produced
flag-absent (the Python fit) at P9-B0.** The FINDING for the freeze report: on the real Y-F0 TRAIN fit the C++
theme-ERC fit agrees with numpy within ruling P12 but not bit for bit (the risk `wave-review-ic.md` section 4 stated
before the run: `fit.hpp:23-31`, BLAS `np.dot` vs the C++ sums), and `--engine-fit` writes the C++ values, so every
weight moves in its last bits; no re-tuning, no second attempt. The admission screen itself is identical.

Other step-3 items: COMPOSE-1a ACCEPT (above); D3-Q2 / D3-S1 greps 0 / 0 (`gate-logs/d3-greps.txt`); golden at 1 and
4 workers OK on both builds; canaries 15 / 15 on both builds (step 1). **X-5 u / w identity and every other run naming
`--candidate-cache build-equity/mega-candidate-cache-v8-lo3`: not run** (GATE-IDX: `index.sqlite` still present,
`gate-logs/cache-lo3-listing-agent2-start.txt`: 352 files, `fb444f7d316a1a4c`; without that row 351 files =
`155872de50833fca`); X-5's u / w would in any case need the absent fields v13. **Step 3 state: PARTIAL** (Y-F0 identity,
ALCLOCK-1, D3-Q1a, COMPOSE-1a, greps, golden, canaries done; X-5 identity blocked by missing inputs; u / w identity
blocked by GATE-IDX).

R-2 in step 3 (agent 4, ruling R2-IDENTITY; GATE-REV S-1): step 3's R-2 identity is its NAV only (the brief's step 4
re-bases "R-2 (NAV)"; no P9 cell reads R-2's u / fit / w; G-B6 and G-B8 read its NAV), met by step 4 row 1 under
FIELDS-SUB-A (`p9-b0-r2-nav`). R-2's u / fit / w: **CUT**, "not run in P9: NAV-only reference; its v8-era u / fit / w inputs include fields v10, absent (FIELDS-GONE)".

### Steps 4-6: not started (GATE-IDX; missing R-2 input)

Immediately before step 4 (13:40:01Z) `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` was STILL present, so
no run naming `--candidate-cache build-equity/mega-candidate-cache-v8-lo3` was started (Y-F0's re-based u / w are P9-B0
runs): **BLOCKED-OWNER** (GATE-IDX). P9-B0 is one run set with its substitution list committed first; it was not
started piecemeal (its R-2 NAV would in any case refuse: fields v10 absent, above). No substitution list was written
(it depends on the PM's ruling on R-2's missing input). Steps 5 (rho) and 6 (re-pin; EXE-FREEZE) follow step 4.

### Step 7, agent 2: the parts that name neither the cache nor a P9-B0 output

| item | result | evidence |
|---|---|---|
| `research_cycle.py scoreboard --timings` | exit 0; ledger `build-equity/trials.jsonl` 133 lines, head `5a3ef9d9...7503`, N 62 (chain verified); lineage `(parent) v8x3b -> y-s v8ysb ACCEPTED`, 0 branches; waves y-s ACCEPTED, y-2 / y-3 / y-5 NOT ACCEPTED; checks 4 / 4 (each ledgered, `s2_net_sr` equal): **reproduces the accepted lineage** (= the wave-1 record); timings table below (wall seconds; v8's waves read through the P9 scoreboard; no P9 wave exists yet) | `gate-logs/scoreboard-gate.txt` (non-statistic fields only; the console and JSON were deleted from the scratchpad unread beyond those fields) |
| PY-S S-6: the Python `ledger_n` vs the eval verb's N, by hand | **equal**: unscored py 63 = `house_v1.n` 63 = `tot.v8.n` 63 (`n_rule` "+ 1"); scored (`--trial c0=0ad7ea4c5122ebe3`) py 62 = 62 = 62 (no "+ 1"); Release eval verb p9-3f `e2b48c46...962c`; synthetic net series (300 placeholder rows), no dsr / sr0 / variance field read | `gate-logs/pys-s6-ledger-n.txt` |
| CRT record (CRT-2) | the two D-COV stores: receipts completed exit 0, Release, exe `ccc73cd1...d811` (p9-2o), manifests = the M2i pins; `ucrtbase.dll` unchanged since their build; never rebuilt | `gate-logs/crt-record.txt` |
| catalog (M2A-4), `verify --pins`, render identity, second catalog, X-5 opt-in cache identity, G-P ticks | **not run**: the catalog walks `build-equity/` (it would index the GATE-IDX file and could not be reproduced after its deletion) and belongs on the post-re-pin tree; the X-5 opt-in cache identity names the cache (GATE-IDX) and needs the absent fields v13 | - |

`scoreboard --timings` (v8 waves, wall seconds by phase; runs), as read at the gate:

| wave | card | fit | monitor | nav | ref | summ | u | w | total |
|---|---|---|---|---|---|---|---|---|---|
| y-2 | 18.2 (1) | 0.8 (1) | 1.6 (1) | 90.9 (2) | - | 34.4 (1) | 12.7 (1) | 42.2 (1) | 200.7 |
| y-3 | 24.5 (1) | 1.9 (1) | 1.6 (1) | 105.5 (2) | - | 37.2 (1) | 12.7 (1) | 52.8 (1) | 236.2 |
| y-5 | 18.8 (1) | 1.0 (1) | 1.6 (1) | 161.6 (2) | - | 37.7 (1) | 12.7 (1) | 62.2 (1) | 295.5 |
| y-s | 24.5 (1) | 1.9 (1) | 1.6 (1) | 115.3 (2) | 61.9 (1) | 40.0 (1) | 12.7 (1) | 52.8 (1) | 310.7 |

Gate walls of this agent beside them (Release p9-3f, our host quiet but for the HOST-6 vol sessions): Y-F0 NAV 21-22 s
(the wave-1 P9-B0 Debug NAV: 79-90 s); Y-F0 `factors` 24.8 s; the fitter 1.6 / 1.9 s (cached WorkStore records,
67 reused).

### Step 3, agent 2: COMPOSE-1a, the store-backed parse check (`gate-logs/compose-1a.txt`, `tools/compose_1a.py`)

Synthetic only (no product code, no real data): the tiny world (`scripts/tests/fixtures/tiny_world.py`, seed 7) built
into `build-equity/p9-gate-compose-tw` (role `2b34283c...6c31`, fields `395f82f7...a1ac`, = the template's locked
pins); the step-1 Release `atx-equity-strategy-risk` (p9-3f `0c110534...586b`) `risk ... --emit-exposures all
--emit-container --bias-families extended --recipe atx-cov-v1` on it: **exit 0**, a store
`build-equity/p9-gate-compose-store` (manifest `5a4b7b9b...1555`, container `model.atxcov` written; 656 sessions x 64
names, 0 fitted / 0 forecast sessions on so short a world). Then the Release NAV exe (p9-3f `255b58ac...3565`) on the
tiny world's NAV argv (`tiny.json` nav block; `--combined` names a placeholder file: every check below comes before the
blend load), one not-yet-existing `--output` each:

| case | flags added | exit | the refusal line | stage reached |
|---|---|---|---|---|
| COMPOSE-1 | `--rate info-clock-rate-v1 --risk-model <tiny store> --risk-model-sha256 5a4b7b9b... --target-rule mv-aim-v1` | 1 | `InvalidArgument: nav replay: fields lack ea_days_since` | v7 parse, the store open (role pin checked), the base parse and the run checks pass; refused at the fields load: the tiny world has no `ea_days_since` (the rate's clock) |
| control: `mv-aim-v1` alone | the store flags + `--target-rule mv-aim-v1` | 1 | `InvalidArgument: target replay: manifest missing/oversized` | past the fields load; stops at the placeholder blend |
| control: the rate alone | `--rate info-clock-rate-v1` | 1 | `InvalidArgument: nav replay: fields lack ea_days_since` | the same stop as COMPOSE-1 |
| control: two stores | COMPOSE-1's flags + a second `--risk-model` | 2 | `InvalidArgument: nav v7: duplicate/missing value: --risk-model` | refused at the v7 parse (the M-1 refusal) |

No output directory was created by any case. **COMPOSE-1a outcome: ACCEPT at the gate's store-backed parse check**
(the two rules together pass the parse and the store open with a real store; the one refusal is the tiny world's
missing clock field, identical with the rate alone, i.e. a data refusal, not a flag conflict). So P9-MV does NOT lapse
under COMPOSE-1 at the gate. Stated limit: `validate_nav_config` / `validate_nav_input` run after the fields and blend
loads (`book/nav_lockstep.cpp:238-250`), which the tiny world cannot reach with the rate on; those checks of the two
rules together are P9-MV's own preflight (COMPOSE-PRE: a refusal there lapses P9-MV at 0 before any read). The tiny
world and its store are deleted by literal name after this record.

---

## Chain agent 3 (RESUME 13, 2026-10-16; `r13-prompts/gate-prompt-3.md`; rulings GATE-3-ORDER, FIELDS-SUB, FIELDS-X5, GATE-MYPY, LAPSE-TABLE, TEST-2)

Started from `handoff-root-gate-2.md`. Order of work = GATE-3-ORDER (ledger :982): every item that names no candidate
cache first, then the GATE-IDX check. Every commit of this agent: explicit pathspecs plus
`:(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache`, then `git show --stat HEAD` checked for
`mypy_cache` (0) (GATE-MYPY; the 133 staged editor-side paths are the owner's to unstage, never touched here).

### Step 0 re-verification (agent 3, 2026-10-16T14:04Z)

- HEAD `2e08fa1c` (docs); code head `06dc9e5a`: `git diff --name-only 06dc9e5a..HEAD` outside `.superpowers/` = 0 files.
  `git status --porcelain` (outside the staged `.mypy_cache`): the PM's `progress.md` edit and the five untracked PNGs.
- Trial ledger 133 lines, sha256 `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`.
  `build-equity/research-store/` absent (SQL3-2).
- GATE-IDX file `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` **still present** (72 KiB) at start; not touched.
- SPDLOG-RELINK re-hash (`tools/exe_sha_end.py`): **27 / 27 Release (p9-3f) and 27 / 27 Debug (p9-3g) `disk = receipt`**
  (`gate-logs/exe-sha-rel-3.txt`, `exe-sha-dbg-3.txt`); next free tag `p9-3h` unused.
- Host: free RAM 3,490 of 16,069 MB; C: 66.6 GB free; no build running (ours or foreign); editor-side python only.

### FIELDS-SUB (1): the witness (`gate-logs/tools/fields_witness.py`, `gate-logs/fields-witness.txt`; commit `e5a2fe68`)

For each of the 70 names of `scripts/specs/v8/lib-v80.json` `fields.list` (R-2's spec, sha256 `306a070b...3a21`;
`fields.manifest_sha256` = v10's pin `a4a060ae...0809`): (a) the v11 manifest (`5826c02a...5f69`) has the entry, file
`<f>.f64`, `reused_from.dir` ending `fields-v10`, `reused_from.manifest_sha256` `a4a060ae...0809` and a `payload_sha256`;
(b) v15's manifest (`26fee5ce...3b09`, re-hashed) `files[<f>.f64].sha256` equals it; (c) the bytes on disk
`train-2020-2023-lo3-fields-v15/<f>.f64` hash to it (every one of the 70 hashed in full). **Result: 70 / 70 hold, no
miss.** Beside it, the two fields R-2's NAV of record says it read (`summary.json` `/financing_fields/fields_used`:
`shares_out` `1cff53f8...05bc`, `si_shares` `78036207...7186`) equal v15's `files` entries and the bytes on disk.

### Step 4, agent 3: the P9-B0 substitution list (written and committed BEFORE any P9-B0 run)

Reading of GATE-3-ORDER's items (1) and (2): the witness (1) runs first; the substitution list (2) is committed before
any P9-B0 run (the brief's step 4 and FIELDS-SUB: "listed FIRST in the substitution list"), so R-2's NAV run (1) starts
only after this list's commit. The runs, their tool and the checks are fixed here; nothing below is decided after a run.

Common to every run below:
- **Exe** (M2c-RED-2 (e), OD5-2: Release is the producer of record): token 0 of every receipt of record (a Debug exe in
  `build-equity/bin`) -> the Release exe of step 1 in `build-equity-rel/bin`, tag p9-3f, source = code head `06dc9e5a`
  (receipt Source `5f48a25d`, a docs commit: `git diff 5f48a25d..HEAD` outside `.superpowers/` empty, EXE-CUR): NAV
  `atx-equity-strategy-targets` `255b58ac...3565`, IC `atx-equity-strategy-ic` `ccd54db6...d705`; re-hashed against the
  receipt before and after each run (SPDLOG-RELINK).
- **Runner** (RUN-1, RUN-2, COVR-1): `scripts/run_bounded_research.py --output build-equity/<out>-run --seconds 600
  --max-rss-mib 2048 --build-type Release --admission-wait-seconds 1800 -- <exe> <argv>`; u / w at the registered phase
  caps of prereg C.14 (u 2,560, w 3,072 MiB; `lib-v8ysb-gm.json:10-28`), free RAM stated; alone on our side of the host
  (GATE-PAR); one per pool.
- **Argv** = the receipt of record's `command` with ONLY the tokens named per run changed (the tool asserts each named
  flag occurs exactly once, that no added flag is already there, and re-hashes every substituted pin from its file).
  `--ann-mean-gross` appended on every NAV reference (PRE-7a, C3-9); `--info-clock-print` appended on Y-F0's NAV only
  (ALCLOCK-1: the P9-B0 names it on the Y-F0 reference; held at agent 2's step 3; LAPSE-TABLE P9-C RUNS). Fit mode
  flag-absent (LAPSE-TABLE; D3-Q1a). No `--candidate-cache` on any NAV argv (checked on each receipt of record before
  its run: none names one), no cache-index flag anywhere (SQL-11, prereg C.13). Timings off: no timing flag on any argv
  (C.5).
- **`--seal-allow`** (SEAL-ALLOW-3): a flag of the wave driver `research_wave.py` (its seal scan over the wave's logs;
  `scripts/wave_seal.py:18`); a hand run through the bounded runner has no seal scan and the exes take no such flag, so
  it is named on no argv here; should any run stop on a seal token, `tools/sealsrc.py` runs first and the stop is
  recorded (none expected).
- **No statistic is read.** Outputs are compared by file SHA-256 and JSON leaf path (`m2g-logs/tools/nav_cmp.py`: CSVs by
  header, row count and the count of differing cells per column; no value printed). Every output file's SHA-256 is
  listed (`gate-logs/b0-files-<out>.txt`). A run's receipt must carry no `store` block (SQL3-2).
- **Tool:** `gate-logs/tools/b0_nav.py <cell>` (NAV runs; `--dry` prints the argv and its token diff, run before this
  commit for all four NAV cells); the u / w runs use the same form (token 0 and `--output` only).

| # | reference replaced | receipt of record (argv source) | tokens changed (besides token 0) | new dir | compared against / expected | why |
|---|---|---|---|---|---|---|
| 1 | **R-2** (A.4; no P9-B0 reference yet) | `mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80-run` (v8 Debug exe `5497c89d`, source `39f87cd6`, 2026-10-02) | `--fields build-equity/train-2020-2023-lo3-fields-v15/manifest.json --fields-sha256 26fee5ce...3b09` (the v10 pair: FIELDS-GONE, FIELDS-SUB, witness above), `--output build-equity/p9-b0-r2-nav`; + `--ann-mean-gross` | `p9-b0-r2-nav` | the v8 NAV of record `mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80` by SHA-256 per file; allowed: `summary.json` `/producer/*` and the leaves `--ann-mean-gross` adds (`/scenarios/<i>/ann_mean_gross`, opt-in side only, the M2j item-4 form); identical outside them = path A (R-2 re-based = `p9-b0-r2-nav`, PRE-22 met by substitution); any other difference, a witness miss or a refusal of the 84-field manifest = **path B** (R-2 NOT re-based; G-B6 / OD-3 lines as FIELDS-SUB words them). Facts stated before the run (not a ruling): that reference was written before P9 by the v8 exe, i.e. before C1's NAV change (M1d's P9-B0 list: L1-L4 / capacity files "may differ (sqrt)" against a v8-era reference); and its `summary.json`, `recipe.json`, `capacity/summary.json`, `capacity/recipe.json` carry the fields manifest's SHA-256 (`/financing_fields/manifest_sha256`, `summary.json` `/source_bindings/research_fields_manifest_sha256`). The verdict is the ruling's letter; every differing path is ALSO classified (fields-manifest leaf / M1d's C1 classes / other) for the PM, deciding nothing. **Ruled path A (FIELDS-SUB-A)** (agent 4's amendment): the re-based R-2 of record is `p9-b0-r2-nav` | PRE-22; FIELDS-SUB; FIELDS-SUB-A |
| 2 | **X-10** (`p9-b0-x10-nav`, M1d, Debug `407c34ad`) | `p9-b0-x10-nav-run` | `--output build-equity/p9-b0g-x10-nav`; + `--ann-mean-gross` | `p9-b0g-x10-nav` | `p9-b0-x10-nav`: 27 / 27 files, 26 byte-identical; `summary.json` only `/producer/build_type`, `/producer/engine_git_sha` and `/scenarios/<i>/ann_mean_gross` (the form of agent 2's Y-F0 identity on the same exe, `nav-identity-yf0.txt`); anything else = an identity miss, attributed by merge commit (ROOT-1) | B2-13 (the leverage-only pair's reference); GATE-3-ORDER |
| 3 | **Y-1** (`p9-b0-y1-nav`, M1d, Debug `407c34ad`, store `v8-risk-lo3-v10`) | `p9-b0-y1-nav-run` | `--risk-model build-equity/p9-m2i-dcov-v11 --risk-model-sha256 397366473b3a...23c7` (the Release `atx-risk-v1.1` container store by its pin, PRE-29; `handoff-root-m2i.md` "Store pins"), `--output build-equity/p9-b0g-y1-nav`; + `--ann-mean-gross` | `p9-b0g-y1-nav` | no identity is claimed (PRE-29: another store, not byte-comparable with v8's Y-1): every file's SHA-256 recorded; compared with `p9-b0-y1-nav` and, PRE-34's form, with v8's ledgered Y-1 NAV `mega-nav-v8y-vol-target-L2.0`, each reported as "agree / differ at <paths>" by SHA-256 and JSON path only; PRE-34's printed-precision agreement of the figures is a read of figures (B.7: made before the adoption print) and is NOT run here (READ NO STATISTIC). Before the run: COV-9 role check (`m2g-logs/tools/s9_check.py`), COVR-1 | PRE-29, PRE-30, PRE-34 |
| 4 | **Y-F0 u** (`p9-b0-yf0-u`, M1d, Debug IC `393183c0`) | `p9-b0-yf0-u-run` | `--output build-equity/p9-b0g-yf0-u` (the argv keeps `--candidate-cache build-equity/mega-candidate-cache-v8-lo3`, GATE-RELCACHE) | `p9-b0g-yf0-u` | after GATE-IDX only. `p9-b0-yf0-u`: payloads (`orientations.json`, `recipe.json`, `train_daily_ic.csv`) byte-identical; `train_candidates.jsonl` equal outside `/stage_seconds/*`, `/wall_seconds`; `summary.json` differs by design only at timing leaves, the Release VM identity strings and cache-root paths, the cache counters (cold Release pass vs warm Debug reference: M1d's Release IC adoption list, `integration-log.md:304-327`) and the cold-vs-warm field counters `/roles/*/research_fields/{field_loads,loaded_bytes,peak_resident_fields}` (GATE-RELCACHE), each listed by path; anything else = an identity miss (ROOT-1). Cache: GATE-RELCACHE's checks (>= 15 GB free before; after, the 351 original files byte-unchanged, `155872de50833fca`, and the new whole-dir listing recorded with its cause) | PRE-22 |
| 5 | **Y-F0 w** (`p9-b0-yf0-w`, M1d) | `p9-b0-yf0-w-run` | `--output build-equity/p9-b0g-yf0-w` (cache as row 4; `--composition-weights` the v8 fit's, as the receipt) | `p9-b0g-yf0-w` | after GATE-IDX only. `p9-b0-yf0-w`: the 10 payloads byte-identical (`orientations.json`, `recipe.json`, `train_combined.{f64,json}`, `train_combined_{finite.u8,ids.u64,member.u8,sessions.i64}`, `train_daily_ic.csv`, `train_planned_targets.csv`); `summary.json` / `train_candidates.jsonl` as row 4 | PRE-22 |
| 6 | **Y-F0 NAV** (`p9-b0-yf0-nav`, M1d) | `p9-b0-yf0-nav-run` | `--output build-equity/p9-b0g-yf0-nav`; + `--ann-mean-gross --info-clock-print` | `p9-b0g-yf0-nav` | after GATE-IDX (GATE-3-ORDER). `p9-b0-yf0-nav`: 27 / 27, 26 byte-identical; `summary.json` only `/producer/build_type`, `/producer/engine_git_sha`, `/scenarios/<i>/ann_mean_gross` and `/scenarios/<i>/info_clock/**` (opt-in side only; ALCLOCK-1 as held at step 3) | PRE-22, ALCLOCK-1, LAPSE-TABLE |
| - | **X-5** (`p9-c1-s2-x5-nav`; u / w) | - | - | - | **CUT** (FIELDS-X5): "not run at the gate: fields v13 absent (FIELDS-GONE)"; X-5 is no P9 cell's parent; its opt-in cache identity of record is M6b's items 5 / 6 (`handoff-root-m6b.md`), cited, not re-run | FIELDS-X5 |

Not re-based: the Y-F0 fit (the brief's step 4 names u, w and NAV; the NAV references read the v8 w's
`train_combined.json`, the w reads the v8 fit's weights, each stage on its reference's own inputs, the M1d form); the
gate's flag-absent fit identity of record is agent 2's (`fit-identity-yf0.txt`: holds outside D2's code-identity
stamps). The D-COV stores are pinned bytes, never rebuilt (CRT-2). rho (step 5) is taken on the new NAV dirs above
(B2-13; value and the "1 - rho < 1e-6" flag only).

Committed as `778d34b8` (with `tools/b0_nav.py`, the ledger and `gate-prompt-3.md`), before the first P9-B0 run.

### Step 4, agent 3: the cache-free P9-B0 runs (rows 1-3; `gate-logs/b0-runs.txt`, `b0-files-<out>.txt`, `receipt-<out>.json`)

Every run: Release NAV exe p9-3f `255b58ac...3565` (= receipt before and after), bounded runner `--seconds 600
--max-rss-mib 2048 --build-type Release`, alone on our side, receipt "clean in the code pathspec", **no `store` block**
(receipt and start), argv = the receipt of record's with exactly the substitution list's tokens changed (the tool's token
diff in `b0-runs.txt`). `ucrtbase.dll` at these runs: 10.0.26100.7623 `2C08E77F...65EF` (= CRT-2; `gate-logs/ucrt-b0-3.txt`).

| # | run | started (UTC) | free RAM before | outcome | wall / peak | files | listing SHA-256 (`b0-files-<out>.txt`) | `summary.json` SHA-256 |
|---|---|---|---|---|---|---|---|---|
| 1 | `p9-b0-r2-nav` (R-2) | 14:15:27 | 3,350 MiB | completed, exit 0 (the 84-field v15 manifest accepted) | 25.1 s / 583 MiB | 27 | `04bb8060...b1bd` | `48a52f6a...c1b9` |
| 2 | `p9-b0g-x10-nav` (X-10) | 14:18:45 | 3,171 MiB | completed, exit 0 | 25.6 s / 583 MiB | 27 | `6c87c8d5...f31d` | `deabe71b...0bd` |
| 3 | `p9-b0g-y1-nav` (Y-1, store `p9-m2i-dcov-v11`) | 14:19:44 | 3,109 MiB | completed, exit 0 | 25.8 s / 1,080 MiB | 28 | `cac882e1...b813` | `b0d4dcbf...c028` (`vol_target.csv` `8e614ea7...67a7`) |

Identity-bridge flag line (OD5-1 (i), prereg C.1; added by agent 4 under R1-FIX, GATE-REV R-1), one per reference:
- Row 1, R-2 `build-equity/p9-b0-r2-nav`: "universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured".
- Row 2, X-10 `build-equity/p9-b0g-x10-nav`: "universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured".
- Row 3, Y-1 `build-equity/p9-b0g-y1-nav`: "universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured".

Before row 3: COV-9 role check (`gate-logs/cov9-role-check-3.txt`, 14:19:24Z): role `e1c67101...95f4`,
`universe.delisting.returns_applied = false`, 0 of 990 events true: **S9 HOLDS**; and the v1.1 store re-checked
read-only against its pin (`tools/store_files_check.py`, `gate-logs/v11-store-check-3.txt`: manifest `39736647...23c7`,
11 listed files + `model.atxcov` (root `b39a1b77...2e1d`) = the manifest, none unlisted: HOLDS).

**Row 1, R-2: FIELDS-SUB step (3) and its verdict.** `m2g-logs/tools/nav_cmp.py` against the v8 NAV of record
(`gate-logs/r2-cmp-raw.txt`): 27 / 27 files, **13 byte-identical, 14 differing; VERDICT outside the allowed list**
(`summary.json` `/producer/*`, `/scenarios/<i>/ann_mean_gross`). Every differing path classified
(`gate-logs/tools/r2_classify.py`, `gate-logs/r2-classify.txt`; kinds of values only, no value printed): **no path
outside these classes**:

| class | where | count |
|---|---|---|
| ALLOWED | `summary.json` `/producer/{build_type, definition, engine_git_sha}` (absent -> string), `/scenarios/0..4/ann_mean_gross` (absent -> number, opt-in side only) | 8 leaves |
| FIELDS (the substitution itself) | `/financing_fields/manifest_sha256` in `summary.json`, `recipe.json`, `capacity/summary.json`, `capacity/recipe.json` (v10's `a4a060ae` -> v15's `26fee5ce`) | 4 leaves |
| DERIVED (a digest of another output file, each = that file's digest on its own side) | `/recipe_sha256` (compact-JSON digest of `recipe.json`, in `summary.json` and `capacity/summary.json`); `summary.json` `/scenarios/1..4/daily_csv_sha256`; `capacity/summary.json` `/scenarios/0..4/daily_csv_sha256`; `summary.json` `/v7/files/{capacity/summary.json, v7_extras.json}` and `v7_extras.json` `/files/capacity/summary.json` (added) | 14 leaves |
| M1D-C1 (named by M1d's written P9-B0 list for a v8-era reference vs a P9 exe, `integration-log.md:251-259`) | `summary.json` `/v7/extras` (string) | 1 leaf |
| C1-COST (M1d's class, `integration-log.md:345`: cost columns only, a few rows) | the 4 daily CSVs of L1-L4 and the 5 `capacity/daily_K`: only `impact_cost_dollars`, `trade_cost_dollars`, `unrationed_cost_dollars`, `trade_cost_return`, 2-8 rows of 1,006 per file | 9 files |

Byte-identical: every `events_*.csv` (5) and `capacity/events_*.csv` (5), the L0 `daily_linear-6bps-stale5-v1+swap-fin-v1.csv`,
`capacity_curve.csv`, `v7_transfer_coefficient.csv`; no `summary.json` leaf differs other than those listed above.

**Verdict: PATH A, ruled FIELDS-SUB-A** (agent 3 recorded path B by FIELDS-SUB's letter, "any other difference", and
asked; the PM ruled that the letter meant a difference the fields input could cause). The witness held (70 / 70) and
the exe accepted the 84-field manifest; the outputs differ from the v8 NAV of record outside `/producer` + the opt-in
only at the FIELDS, DERIVED, M1D-C1 and C1-COST paths above, which stay beside the reference as its disclosure. So:
**the re-based R-2 of record is `build-equity/p9-b0-r2-nav`** (PRE-22 met by the fields substitution; Appendix IV item
1); G-B6 and the OD-3 R-2 line read it, and the OD-3 R-2 history line is readable (its TRAIN NAV = `p9-b0-r2-nav`).
`p9-b0-r2-nav` is kept (a P9-B0 reference dir; never deleted) with its SHA-256 listing. Agent 3's note for the PM, on
which the ruling rests: the comparison base was written by the v8 exe (`5497c89d`, source `39f87cd6`, 2026-10-02),
before P9's NAV change, so the allowed list (`/producer/*` + the opt-in) cannot hold against it whatever the fields
input: the same exe change moved every v8-era reference at M1d's P9-B0 (Y-F0, X-10, Y-1: 1-8 cost-column rows per L1-L4
file, `/v7/extras`, `/v7/files/*`, all inside M1d's written list), and the FIELDS leaves are written by the substitution
by construction (FIELDS-SUB's own trade-off clause names "its sha" among the differences); no position (events file),
no L0 file and no statistic leaf of `summary.json` moved. (Agent 3's open question 1; answered by FIELDS-SUB-A.)

**Row 2, X-10: holds** (`gate-logs/x10-cmp.txt`): 27 / 27 files, 26 byte-identical; `summary.json` differs only at
`/producer/build_type`, `/producer/engine_git_sha` and `/scenarios/0..4/ann_mean_gross` (absent in the reference, present
in all 5 new scenarios); VERDICT inside the list. The re-based X-10 reference is `p9-b0g-x10-nav`.

**Row 3, Y-1: recorded, no identity claimed (PRE-29).** Against M1d's `p9-b0-y1-nav` (`gate-logs/y1-cmp-b0.txt`) and,
PRE-34's form, against v8's ledgered Y-1 NAV `mega-nav-v8y-vol-target-L2.0` (`gate-logs/y1-cmp-v8.txt`): **differ at**
26 of 28 files each (every `daily_*` / `events_*` CSV and `capacity/*` CSV, `capacity/summary.json` 855 leaves,
`capacity_curve.csv`, `summary.json` 824 / 826 leaves incl. `/scenarios/*` subtrees and the opt-in / producer leaves,
`v7_extras.json` 77 leaves, `v7_transfer_coefficient.csv`, `vol_target.csv` (`sigma_hat`, `sigma_ref`, `raw`, `L_t`,
`multiplier`, `gross`, `priced_share`)); byte-identical: `recipe.json`, `capacity/recipe.json`. This is the store
substitution PRE-29 ruled (the Release-built v1.1 container store; "not byte-comparable with v8's Y-1"). PRE-34's own
criterion (agreement of the figures at the ledger's printed precision) is a read of figures, made before the adoption
print by the cells root (B.7, PRE30-MVX): not run at the gate (READ NO STATISTIC). The re-based Y-1 reference is
`p9-b0g-y1-nav`.

### Step 5, agent 3: rho of the pair that names no cache (`gate-logs/rho-pairs-1.txt`, `tools/rho_pairs.py`)

Run before the GATE-IDX check because it names no candidate cache and reads only rows 2 and 3 (GATE-3-ORDER: everything
that names no cache first); the pairs that need the re-based Y-F0 wait for GATE-IDX. Bounded runner
(`build-equity/p9-b0g-rho-1-run`: completed exit 0, 0.5 s, peak 41 MiB, no `store` block), the M2g tool's form, the
Release eval verb p9-3f `e2b48c46...962c`; only `paired.{sessions, rho, one_minus_rho, degenerate}` read:

| pair (B2-13) | T | rho | "1 - rho < 1e-6" |
|---|---|---|---|
| Y-1 vs X-10 (`p9-b0g-y1-nav` vs `p9-b0g-x10-nav`) | 1005 | 0.9947113980219978 | **false** (paired test defined) |

Not yet measured (need the re-based Y-F0 NAV, GATE-IDX): the P9-X pair (re-based Y-1 vs re-based Y-F0) and X-10 vs
Y-F0.

### GATE-IDX check (agent 3), after every cache-free item

2026-10-16T14:21:49Z: `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` **STILL PRESENT** (72 KiB); the cache
dir lists 352 files (`gate-logs/cache-lo3-listing-agent3-idx.txt`, `fb444f7d316a1a4c`; without that row = agent 2's
`155872de50833fca`). So no run naming `--candidate-cache build-equity/mega-candidate-cache-v8-lo3` was started:
**BLOCKED-OWNER** (GATE-IDX). Not run: rows 4-6 (Y-F0 u / w / NAV; GATE-3-ORDER puts the NAV after the check too), the
rest of step 5, step 6 (re-pin; EXE-FREEZE not started), step 7's rest, step 9's final records. Re-checked at the
hand-back (14:29:49Z): still present. Trial ledger 133 lines `27e40f9f` at the hand-back; C: 67 GB free. Prepared for
the next agent (not run): `gate-logs/tools/b0_ic.py` (rows 4-5 with GATE-IDX and GATE-RELCACHE built in; its `--dry`
refuses while the file exists, tested). Hand-off: `handoff-root-gate-3.md`.

---

## Chain agent 4 (RESUME 13, 2026-10-16; `r13-prompts/gate-prompt-4.md` over `gate-prompt-3.md`; rulings FIELDS-SUB-A, RELCACHE-ALLOW, PRE34-WHO, YF0-NAV-ORDER, PRE-2BII-B2A)

Started from `handoff-root-gate-3.md` "Exact next step". The owner deleted the GATE-IDX file and unstaged the editor-side
`.mypy_cache` by hand (ledger "OWNER ACTIONS observed"); every commit of this agent still names explicit pathspecs plus
`:(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache` and is checked for 0 `mypy_cache` paths.
The PRE agent's `p9-prereg.md` is never added here (PRE-2BII-B2A).

### Step 0 re-verification (agent 4, 2026-10-16T15:10:30Z)

- HEAD `1e2dbf3c` (the PRE agent's B2a pathspec commit above agent 3's `65ce28f0`); code head `06dc9e5a`:
  `git diff --name-only 06dc9e5a..HEAD` outside `.superpowers/` = 0 files; index: nothing staged (the 133
  `.mypy_cache` paths are no longer staged). `git status --porcelain`: the PM's `progress.md` edit and the five PNGs.
- Trial ledger 133 lines, sha256 `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`;
  `build-equity/research-store/` absent (SQL3-2).
- SPDLOG-RELINK (`tools/exe_sha_end.py`): **27 / 27 Release (p9-3f) and 27 / 27 Debug (p9-3g) `disk = receipt`**
  (`gate-logs/exe-sha-rel-4.txt`, `exe-sha-dbg-4.txt`); next free tag `p9-3h` unused.
- `ucrtbase.dll` 10.0.26100.7623 `2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF` (= CRT-2).
- Host: free RAM 4,671 of 16,069 MB; C: 60.4 GB free; no build of ours; foreign: the editor's python daemons and an
  atx-vol quote benchmark under `C:\atx-cache\quote-benchmark` (HOST-5 / HOST-6: never touched).

### GATE-IDX check (agent 4; first act after step 0)

2026-10-16T15:11:39Z: `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` **ABSENT**; the cache dir lists exactly the
351 original files, `tools/dir_listing.py` digest **`155872de50833fca`** (= agent 2's start listing minus its
`index.sqlite` row, row for row; `gate-logs/cache-lo3-listing-agent4-idx.txt`; 7,799,539,072 bytes). GATE-IDX cleared.
`tools/b0_ic.py yf0u --dry` and `yf0w --dry` (GATE-IDX + GATE-RELCACHE dry check): index absent, the argv names the real
cache exactly once and no index flag, the 351 originals `155872de50833fca` with 0 files beyond them, C: 60.4 GB free
(>= 15), the Release IC exe `ccd54db6...d705` = its p9-3f receipt; the argv differs from the receipt of record at token
0 (the Release exe) and `--output` only.

### Step 4, agent 4: substitution-list amendments (committed BEFORE rows 4-6 ran)

1. Row 1 (R-2): **ruled path A (FIELDS-SUB-A)**: the re-based R-2 of record is `build-equity/p9-b0-r2-nav` (the row and
   the agent-3 verdict paragraph above amended; the classification table stays as the disclosure).
2. Rows 4 / 5 (Y-F0 u / w), RELCACHE-ALLOW: compared with M1d's Debug references `p9-b0-yf0-u` / `p9-b0-yf0-w` by
   `gate-logs/tools/b0_cmp.py --mode ic`, which lists every differing leaf by JSON path with its class. Allowed classes
   (exactly the ruling's list, fixed before the runs): TIMING (`.../wall_seconds`, `.../hash_seconds`,
   `.../stage_seconds/<stage>`); COUNTERS (`/roles/<i>/candidate_cache/{hits,misses,legacy_hits,vm_evaluations}`,
   `vm_evaluations` being the miss count by definition, `atx-impl/src/strategy_ic_runner.cpp:735`;
   `/roles/<i>/candidate_cache/ic_results/{hits,misses}`; the per-candidate `signal_cache` / `ic_result_cache` hit | miss
   stamps, `:641-642`; `/roles/<i>/research_fields/{field_loads,loaded_bytes,peak_resident_fields}`); IDENTITY
   (`/roles/<i>/candidate_cache/vm_identity`, and any string leaf equal to the reference's with the reference VM identity
   string replaced by the Release one: the `DIR/<identity>/` cache-root paths; M1d's adoption list, `integration-log.md:322-325`,
   names no other leaf). Every other file byte-identical (u: `orientations.json`, `recipe.json`, `train_daily_ic.csv`;
   w: those plus `train_combined.{f64,json}`, `train_combined_{finite.u8,ids.u64,member.u8,sessions.i64}`,
   `train_planned_targets.csv`). Anything else = an identity miss attributed by merge commit (ROOT-1), BLOCKED for that
   lane; the gate goes on with what the miss does not invalidate.
3. Row 6 (Y-F0 NAV), YF0-NAV-ORDER: runs after row 5 and reads the re-based w: `--combined
   build-equity/p9-b0g-yf0-w/train_combined.json` (`--combined-sha256 e13fbc4d...8c` unchanged: the reference w's
   `train_combined.json`, which = v8's `mega-v8xw-train-theme-erc-v8ysb-2/train_combined.json`; the tool re-hashes the new
   file and stops if it differs), so the receipt names that dir; with `--ann-mean-gross --info-clock-print` (PRE-7a,
   ALCLOCK-1), `--output build-equity/p9-b0g-yf0-nav`. No NAV output names the combined path (checked on
   `p9-b0-yf0-nav`: the path string occurs in no output file; `summary.json` carries the sha only), so the path token
   moves no output byte. Compared with `p9-b0-yf0-nav` by `b0_cmp.py --mode nav` (PRODUCER `/producer/{build_type,
   engine_git_sha}`; OPTIN `/scenarios/<i>/ann_mean_gross` and `/scenarios/<i>/info_clock/**` on the new side only).
   `ucrtbase.dll` re-recorded at this run (C.6). Tool changes: `gate-logs/tools/CHANGES.txt` (agent 4 entry).
4. (REPIN-REV addendum, after the runs; it states a stale pin and changes no run, PRE-40 form) Row 1, R-2: R-2 is read
   BY PATH (`build-equity/p9-b0-r2-nav`, FIELDS-SUB-A). Its v8 spec `scripts/specs/v8/lib-v80.json` (`306a070b...3a21`)
   is NOT re-pinned: its `fields` pin names the absent v10 (`a4a060ae`, FIELDS-GONE) and its `nav.output` the v8 NAV
   dir `mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80`: both stale, stated here; G-B6's bundle and any
   OD-3 era spec for R-2 read `p9-b0-r2-nav` (an OD-3 era spec, if OD-3 runs, is built from its recipe at that step).

### Step 4, agent 4: row 4, Y-F0 u on the Release IC exe (`gate-logs/b0-runs.txt`, `yf0u-cmp.txt`, `yf0u-classes.txt`)

`tools/b0_ic.py yf0u`, started 15:18:36Z, free RAM 3,633 MiB, runner cap 2,560 MiB (C.14): completed exit 0, wall
107.0 s, peak 1,613 MiB, Release IC exe `ccd54db6...d705` = receipt before and after, receipt source `bfbe64d2` "clean in
the code pathspec", no `store` block (receipt, start); argv = the receipt of record's with token 0 and `--output` only.
Outputs `build-equity/p9-b0g-yf0-u`: 5 files, listing `1cb087abaa50746392269b3452491be8fa2999b9a39e1212f01049acec2916aa`
(`gate-logs/b0-files-p9-b0g-yf0-u.txt`), `summary.json` `acbf61cef24e909bb8975668b1b2206708b5c96cf03194a0728564445fae12b8`.
GATE-RELCACHE after the pass: the 351 original files' listing still **`155872de50833fca`** (byte-unchanged by size and
mtime); 201 files / 4,466,406,032 bytes added under `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0/`; whole listing 552 files
`c9ee9291f5ddc0c2`, cause "Release identity subtree added at step 4 (p9-b0g-yf0-u)"
(`gate-logs/cache-lo3-listing-after-p9-b0g-yf0-u.txt`); C: 53.0 GB free after (>= 15).

Identity-bridge flag line (OD5-1 (i), prereg C.1; R1-FIX), row 4, Y-F0 u `build-equity/p9-b0g-yf0-u`: "universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured".

**Identity vs M1d's `p9-b0-yf0-u` (RELCACHE-ALLOW): holds.** Payloads byte-identical: `orientations.json` `d0d16f30...`,
`recipe.json` `b9b4029c...`, `train_daily_ic.csv` `c8714175...`. `summary.json` 629 differing leaves and
`train_candidates.jsonl` 402, every one in an allowed class, each listed by JSON path in `yf0u-cmp.txt` (collapsed by
class in `yf0u-classes.txt`): TIMING 545 (`wall_seconds`, `hash_seconds`, `stage_seconds/*`), COUNTERS 276 (cache
`hits` / `misses` / `vm_evaluations`, `ic_results` hits / misses, the per-candidate `signal_cache` / `ic_result_cache`
stamps 67 + 67 in each file, `research_fields` `field_loads` / `loaded_bytes` / `peak_resident_fields`), IDENTITY 1
(`vm_identity` `dslvm1_clang18.1` -> `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0`), M1D-LIST 209 (the cache-root paths
`directory`, `fields_directory`, 67 `payload` + 67 `sidecar` now under `DIR/<Release identity>/` with the same
remainder; `ic_results` `identity` / `key/ic_identity` + the Release suffix; `ic_results/subdirectory`; 67
`signal_key_sha256`; `verify_bytes`, `ic_cache_bytes`, `ic_scratch_bytes`). Outside: **0**.

Disclosure (the comparison tool, not the run): the first pass of `tools/b0_cmp.py` (`yf0u-cmp-pass1.txt`, kept) put the
209 M1D-LIST leaves OUTSIDE: its IDENTITY rule was a string substitution, and the Debug reference's cache-root paths
sit at `DIR/<role>/...` without an identity directory; and the tool had not encoded RELCACHE-ALLOW's fourth class
("M1d's written Release IC adoption list where it names a leaf"), whose written u / w row is
`root-wave1-merge-report.md:890-891` (my amendment above cited the pre-run expectation `integration-log.md:322-325`
instead). That row names exactly these leaves (identity strings incl. `ic_results` identity / key / subdirectory and
`signal_key_sha256`; cache-root paths `directory`, `fields_directory`, payload + sidecar "now under `DIR/<Release
root>/`, same file names"; counters incl. `verify_bytes`, `ic_cache_bytes`, `ic_scratch_bytes`). The tool now encodes
that list with a strict check per leaf (`tools/CHANGES.txt` (3)); no payload and no other leaf is involved.

### Step 4, agent 4: row 5, Y-F0 w on the Release IC exe (`gate-logs/b0-runs.txt`, `yf0w-cmp.txt`, `yf0w-classes.txt`)

`tools/b0_ic.py yf0w`, after row 4, free RAM 5,210 MiB, runner cap 3,072 MiB (C.14): completed exit 0, wall 30.6 s, peak
1,427 MiB, exe = receipt before and after, receipt source `c0112afc` clean in the code pathspec, no `store` block; argv =
the receipt of record's with token 0 and `--output` only (the cache and `--composition-weights` of the v8 fit as the
receipt). Outputs `build-equity/p9-b0g-yf0-w`: 12 files, listing
`ec69344005107bf0e2ab0bb859583db083889521a652dcfe134b2304e27c8ae1`, `summary.json`
`f772b394a6885b5965d0c5ad93e4f023515a74ed7e02064a772814357ee904f2`. Cache after: the 351 originals still
`155872de50833fca`; the whole listing `c9ee9291f5ddc0c2` (552 files) is byte-for-byte the after-u listing (the warm
Release w added no file; the tool's cause line repeats "added at step 4" because it counts every file beyond the 351);
C: 51.8 GB free.

Identity-bridge flag line (OD5-1 (i), prereg C.1; R1-FIX), row 5, Y-F0 w `build-equity/p9-b0g-yf0-w`: "universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured".

**Identity vs M1d's `p9-b0-yf0-w` (RELCACHE-ALLOW): holds.** All 10 payloads byte-identical: `orientations.json`,
`recipe.json`, `train_combined.f64` `70d863f9...`, `train_combined.json` **`e13fbc4d...8c`** (= the v8 w's, the pin the
NAV row 6 reads), `train_combined_{finite.u8,ids.u64,member.u8,sessions.i64}`, `train_daily_ic.csv` `45261f20...`,
`train_planned_targets.csv` `cb744a74...`. `summary.json` 484 differing leaves, `train_candidates.jsonl` 266: TIMING 541,
IDENTITY 1 (`vm_identity`), M1D-LIST 208 (the same cache-root paths, identity strings, `signal_key_sha256`,
`ic_cache_bytes`, `ic_scratch_bytes`; both sides warm, so no hit / miss counter moved); outside **0**.

### Step 4, agent 4: row 6, Y-F0 NAV of record on the Release NAV exe (`gate-logs/b0-runs.txt`, `yf0nav-cmp.txt`, `yf0nav-classes.txt`)

YF0-NAV-ORDER: after row 5; `tools/b0_nav.py yf0` (dry first: the argv differs from `p9-b0-yf0-nav-run`'s at token 0,
`--combined build-equity/p9-b0g-yf0-w/train_combined.json` (its SHA-256 re-hashed = the pinned `e13fbc4d...8c`) and
`--output`, plus `--ann-mean-gross --info-clock-print` appended). Started 15:39:01Z, free RAM 5,539 MiB: completed exit 0,
wall 34.4 s, peak 646 MiB, Release NAV exe `255b58ac...3565` = receipt before and after, receipt source `a24a77bd` clean
in the code pathspec, no `store` block; the receipt's argv names `build-equity/p9-b0g-yf0-w/train_combined.json`.
Outputs `build-equity/p9-b0g-yf0-nav`: 27 files, listing
`e205d6ecb16e75cba02194b90b313849404c2052b3e1738c4bf020e486fcf797`, `summary.json`
`826d5ec65329c24c4c0c1b72ee1f57dcdbdb136cf00d4113c6463310e79b8f38`.

Identity-bridge flag line (OD5-1 (i), prereg C.1; R1-FIX), row 6, Y-F0 NAV `build-equity/p9-b0g-yf0-nav`: "universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured".

**Identity vs M1d's `p9-b0-yf0-nav`: holds.** 26 / 27 files byte-identical; `summary.json` differs at 77 leaves, all
allowed: PRODUCER 2 (`/producer/build_type`, `/producer/engine_git_sha`), OPTIN 75 on the new side only
(`/scenarios/0..4/ann_mean_gross` 5; `/scenarios/0..4/info_clock/*` 70 = 14 leaves x 5 scenarios, the ALCLOCK-1 form of
agent 2's `nav-identity-yf0.txt`); outside **0**. ALCLOCK-1 holds at the NAV of record (LAPSE-TABLE: P9-C RUNS).

`ucrtbase.dll` re-recorded at the Y-F0 runs (C.6, `gate-logs/ucrt-b0-4.txt`, 15:39:47Z): 10.0.26100.7623
`2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF` (= CRT-2, = agent 3's cache-free runs).

**Step 4 state: HOLDS.** Re-based references of record: R-2 `p9-b0-r2-nav` (FIELDS-SUB-A), X-10 `p9-b0g-x10-nav`, Y-1
`p9-b0g-y1-nav` (PRE-29, no identity claimed), Y-F0 `p9-b0g-yf0-u` / `p9-b0g-yf0-w` / `p9-b0g-yf0-nav`; X-5 cut
(FIELDS-X5).

### Step 5, agent 4: rho of the two pairs that need Y-F0 (`gate-logs/rho-pairs-2.txt`, `receipt-p9-b0g-rho-2.json`)

`scripts/run_bounded_research.py --output build-equity/p9-b0g-rho-2-run --seconds 600 --max-rss-mib 2048 -- <python>
gate-logs/tools/rho_pairs.py build-equity-rel/bin/atx-research-eval.exe <pairs>`: completed exit 0, 1.4 s, peak 51 MiB, no
`store` block; the Release eval verb p9-3f `e2b48c46...962c`; only `paired.{sessions, rho, one_minus_rho, degenerate}`
read (B2-13):

| pair | T | rho | "1 - rho < 1e-6" |
|---|---|---|---|
| P9-X pair: re-based Y-1 vs re-based Y-F0 (`p9-b0g-y1-nav` vs `p9-b0g-yf0-nav`) | 1005 | 0.9947035686613187 | **false** (paired test defined) |
| X-10 vs Y-F0 (`p9-b0g-x10-nav` vs `p9-b0g-yf0-nav`) | 1005 | 0.9994863034956717 | **false** |
| Y-1 vs X-10 (agent 3, `rho-pairs-1.txt`) | 1005 | 0.9947113980219978 | **false** |

**Step 5 state: HOLDS** (every pair's value and flag logged; the P9-V pair's references are P9-X's, PRE-MVX, measured at
the cells).

### Step 6, agent 4: the re-pin (W1-I2 (b), B2-10 (b), B2-11; ONE code commit **`2078f877`**; EXE-FREEZE starts here)

`git show --stat 2078f877`: 3 files, `scripts/specs/v8/{lib-v8ysb-gm,x-leverage-L2.0,y-vol-target-y-1}.json`, +17 / -11,
pins only (0 `mypy_cache`); `git diff --name-only 06dc9e5a..2078f877` outside `.superpowers/` = those 3 files.

| spec (role) | SHA-256 before -> after | pins moved |
|---|---|---|
| `lib-v8ysb-gm.json` (Y-F0, the lineage parent of every P9 cell) | `1e3ec118...8626` -> `3631679cd18f1c6a63a6a5dc96cdbe6ee7669b81b8979f529544f1b237d9edea` | `exes` -> `build-equity-rel/bin/atx-equity-strategy-ic.exe`, `.../atx-equity-strategy-targets.exe` and **`exes.eval`** `.../atx-research-eval.exe` (added); **`exes_sha256`** (added) eval `e2b48c46...962c`, ic `ccd54db6...d705`, nav `255b58ac...3565` (= p9-3f, = disk); `nav.output` `mega-nav-v8x-theme-erc-L1.1828-v8ysb` -> **`build-equity/p9-b0g-yf0-nav`** |
| `x-leverage-L2.0.json` (X-10) | `ebe68e6e...4975` -> `46e9d6c2fdbf42e362518118938bc89af038e3c09d7519d660aaa936554fd0bf` | `change.set.nav.output` -> **`build-equity/p9-b0g-x10-nav`**; `locked.reference_cell` -> `build-equity/p9-b0g-yf0-nav/summary.json` `826d5ec6...8f38` |
| `y-vol-target-y-1.json` (Y-1, P9-X's reference, PRE-30) | `faffee17...5f77` -> `f4186573a41de3b67cdd815976639ed2123e5c6f43353516c61150d92dbb50d6` | `change.set.nav.output` -> **`build-equity/p9-b0g-y1-nav`**; `change.flags.nav` `--risk-model` / `--risk-model-sha256` `v8-risk-lo3-v10` `862515d9...` -> `build-equity/p9-m2i-dcov-v11` `397366473b3a...23c7` (PRE-29); `locked.reference_cell` -> `build-equity/p9-b0g-x10-nav/summary.json` `deabe71b...0bd` |

The exes of the re-pin = step 1's table (Release p9-3f; templates inherit the parent's `exes_sha256`,
`scripts/cycle/lock.py:24-28`). Read, not edited: `info-clock-rate-v1.json` = the registration SHA **`7801c198...a152`**
(W1-I2; on disk = the LF bytes merged at `1d003241`); `mv-aim-v1.json` = `4b4af6e2...80b4` **NOT edited** (COVMV-PIN:
its `printed_only` / "ex-ante" sentences stay as they are, PRE-40 form, stated in B.5).

Decided within the rulings (each stated for the PM; open questions 1-2):
- **`ic.u_output` / `ic.w_output` stay the v8 dirs** (`mega-v8-b0b-train-u-v8ysb[-1]`, `mega-v8xw-train-theme-erc-v8ysb[-2]`).
  The cycle names an IC pass `<base>-<n>` with run dir `<base>-run<n>` (`scripts/cycle/phases.py:566-568`, `:596-598`),
  so a spec cannot name the exe-direct P9-B0 dirs `p9-b0g-yf0-u` / `-w` (run dirs `-run`): pointed there, the cycle
  resolves `p9-b0g-yf0-u-1` / `-w-1` "pending" and every NAV-only cell would re-run u and w (tried in the working tree,
  `repin-verify.txt` is the final state). The v8 u / w are byte-identical to the re-based u / w in every payload
  (u 3 / 3, w 10 / 10 incl. `train_combined.json` `e13fbc4d`, checked by SHA-256 at the re-pin) and their receipts
  predate K-P9-10, so the Release pins do not refuse them (`scripts/cycle_resume.py:141-155`); the P9-B0 u / w of record
  stay `p9-b0g-yf0-u` / `-w` (Appendix IV item 2; the NAV of record reads `p9-b0g-yf0-w`, YF0-NAV-ORDER).
- **R-2 (`lib-v80.json`) is not edited**: no lineage parent spec pins R-2's NAV (G-B6, G-B8 and the OD-3 R-2 line read
  `p9-b0-r2-nav` by path, prereg A.4); re-pinning `lib-v80.json`'s fields to v15 would need its 70-name `fields.list`
  changed (the cycle refuses a list that differs from the pinned manifest's 84 rows, `scripts/cycle/gate.py:22-28`): not
  a pin. `lib-v80.json` stays `306a070b...3a21` (the FIELDS-SUB witness's spec SHA).
- `env_path_prepend` not touched: the three pinned Release exes import no vcpkg DLL (PE import tables: system and CRT
  DLLs only), so the DLL path cannot change their bytes' behaviour.

**`verify` on the pinned lineage** (`gate-logs/tools/repin_verify.py`, `gate-logs/repin-verify.txt`; read-only; per
spec the Cycle the CLI builds and, per done step, the checks a `run` makes before reusing it: K-P9-10 argv,
E1-REUSE-a2 exe pin, W1-I2 (a)'s parent-NAV exe, B2-11's `exes.eval`): **X-10 HOLDS** and **Y-1 HOLDS** (every input
pin `[locked, verified]`, the three exe pins `[exes_sha256, verified]` = disk, `exes.eval` named and pinned; fields, u,
fit, card, w, monitor done and reusable (pre-K-P9-10 receipts); NAV `p9-b0g-x10-nav` / `p9-b0g-y1-nav` made by
`255b58ac` = the pin); the wave's view of the parents (`Cycle(..., verify=False)`, as `wave_context.Wave.outputs`):
Y-F0's NAV `p9-b0g-yf0-nav` and X-10's `p9-b0g-x10-nav` made by `255b58ac` = `exes_sha256.nav` (W1-I2 (a) no longer
refuses a Release rule cell over them). Before the re-pin the same tool found 4 problems on X-10 (no pins, no
`exes.eval`, the parent NAV by the v8 exe `72ff6d2d`). Stated, not counted: `lib-v8ysb-gm.json` alone cannot be built
by the cycle (its own `inputs.baseline_fields` is fields v13, FIELDS-GONE; the same before the re-pin; template
children resolve without it); the reference NAVs carry no C-13 binding to these specs (exe-direct runs with the
substitution list's opt-in flags), so a `run` of the reference spec itself would not score them: they are references,
not cells.

### Step 7, agent 4: the store step (M2A-4, SQL3-2, STORE-S S-4), G-P ticks, the G-P10 print (after the re-pin)

Every run on the gate head with the re-pin in (docs head `883be9f2`, code `2078f877`), the Release store exe p9-3f
`atx-research-store` `0eeac21bb95677d8da5268ca829b88dc668521a4c9f625e0fbbde92f96fb3818` (= receipt before and after),
every catalog DB **outside the root** (`C:/Users/natha/AppData/Local/Temp/g4cat/`, `g4fx/`), so
`build-equity/research-store/` stayed absent and the record hook off (SQL3-2); the trial ledger read only (133 /
`27e40f9f` before and after).

| item | result | evidence (`gate-logs/`) |
|---|---|---|
| fixture chain digest (step 6's "fixture chain digest"; the fixture digest of record, M6b SQL4-S S-1) | the store fixture tree `atx-engine/tests/fixtures/research_store/tree`, two fresh catalogs: both `files_seen 28 verified 25 declared 0 skipped 4`, **`catalog_digest a50fdcf9c9c83bdac0d41ff368666da31578ebb4407bbc4498175a63014f100c`** twice (= the record); `verify --pins` exit 0; render identity "total checked 12 mismatches 0" | `fixture-chain-4.txt` |
| real-tree catalog of the gate head, bounded root run (M2A-4) | `run_bounded_research.py --output build-equity/p9-b0g-cat-2-run --seconds 600 --max-rss-mib 2048 -- python tools/gate_cat.py <store exe> C:/atx-wt/pool-2 <A> <B>`: completed exit 0, 26.4 s, peak 53 MiB, no `store` block; catalog A: init 0, catalog 0, `files_seen 33699 verified 4190 declared 84 skipped 29468`, **`catalog_digest adbcd348b5d82a1fd4f357274a34bbf387391cf82154a961e701bad39a0dd474`** (argv as M6B-FINISH item 7: `--root <pool> --ledger build-equity/trials.jsonl --classes atx-engine/schemas/research_store/classes.json`) | `realcat-gate.txt`, `tools/gate_cat.py` |
| a second from-scratch catalog reproduces the digest | catalog B (a fresh DB, in the same bounded run, so both walks see the same tree: the run's own dir holds only its `start.json` during both, and bounded-run receipts / starts are catalogued classes in `classes.json`): the same counts and **`adbcd348...d474`: reproduced** | `realcat-gate.txt` |
| first attempt (void) | `build-equity/p9-b0g-cat-1-run`: process-error exit 1 in 0.3 s; the tool passed a relative exe path that `CreateProcess` refused (FileNotFoundError; nothing catalogued); fixed (absolute path), re-run as above; kept as its record | `realcat-gate.txt`, `realcat-gate-runner.log` |
| `verify --pins` counts by state (G-P10 (a)) on catalog A | bounded run `build-equity/p9-b0g-catv-1-run` (completed exit 0, 0.8 s, peak 40 MiB, no `store` block; `tools/gate_catv.py`): artifacts verified **4,190**, declared **84**; pins **ok 1,620, declared 169, stale 220, missing 630, unresolved 1,512**; by kind: exe ok 1 / stale 84 / unresolved 83; field-payload declared 168 / missing 516; field-source declared 1 / ok 276 / unresolved 1,371; fields-manifest missing 24 / ok 11; input missing 20 / ok 247 / unresolved 6; ledger-head unresolved 17; locked ok 74; receipt-binding missing 70 / ok 630 / stale 136; receipt-log ok 336; rule-template ok 8; spec-digest unresolved 35; stage-receipt ok 33 (every stale / missing row is listed by path in the file: v8-era receipt bindings and the fields dirs gone under FIELDS-GONE) | `realcat-gate-verify.txt` |
| render-identity mismatch count (G-P10 (b)) | `research_store_identity.py --catalog A --root <pool>`: run-receipt 168, run-start 169, stage-receipt 37, cycle-binding 23, cycle-verdict 17, wave-result 4, wave-candidate 16, trial-ledger 1: **total checked 435, mismatches 0** | `realcat-gate-verify.txt` |
| `verify --catalog A --ledger build-equity/trials.jsonl` | exit 0: 133 lines, head `5a3ef9d9...7503`, rule `atx-ledger-chain-fold/v1`, index ok | `realcat-gate-verify-ledger.txt` |
| `classes.json` SHA by hand (STORE-S S-4) | `atx-engine/schemas/research_store/classes.json` **`5bc27ba489f24a0d1c8c42c8c29927c8d1bde6bf63cd5993f285a5b084086557`** (the catalog itself records no `classes.json` SHA: P10) | this row |
| X-5 opt-in cache identity | **CUT** (FIELDS-X5): M6b items 5 / 6 cited, not re-run (a `cache init` on the real cache would re-create the GATE-IDX file) | `handoff-root-m6b.md` |
| scoreboard, `scoreboard --timings`, PY-S S-6, CRT record | agent 2's records stand (`scoreboard-gate.txt`, `pys-s6-ledger-n.txt`, `crt-record.txt`); not re-run: the scoreboard reads wave results and the ledger only (`scripts/wave_scoreboard.py:71-100`), which the re-pin does not touch; the ledger is unchanged (133 / `27e40f9f`, head `5a3ef9d9`) | - |

Cache listing (GATE-RELCACHE) at the end: `build-equity/mega-candidate-cache-v8-lo3` 552 files, 12,265,945,104 bytes,
listing `c9ee9291f5ddc0c2` = the 351 originals `155872de50833fca` (unchanged by size and mtime) + 201 files /
4,466,406,032 bytes of the Release identity subtree `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0/` (written by row 4;
`gate-logs/cache-lo3-listing-agent4-final.txt`); no `index.sqlite`. C: 50 GB free at the end.

#### G-P ticks at the gate (plan section 0.4; G-P1 .. G-P9 + G-P11 gate the freeze, G-P10 is printed, SQL-8)

| row | state at the gate | evidence |
|---|---|---|
| G-P1 one-command wave | **measured at P9-H by the cells root** (GP-TICKS: the v2 wave route end to end on a real cell, ticked at the first P9 v2 wave with its evidence row); no P9 wave has run at the gate (0 trials); the v8 timings baseline is agent 2's `scoreboard --timings` | `scoreboard-gate.txt` |
| G-P2 spec-only experiments | **ticked**: the 10 kind fixtures under `scripts/specs/p9/kinds/`; the W3-8a tick held at M5 (`root-wave3-merge-report.md:444`, 12 passed); `scripts/tests` whole green at seeds 0 and 1 at step 2 (incl. `test_wave_kinds.py`) | `pyt-scripts-tests-rel-s0-rerun.log`, `pyt-scripts-tests-rel-s1.log` |
| G-P3 Release adopted | **ticked**: IC half at M1d (u / w / marginal); NAV half shown at M2j item 5 (Release = Debug on X-5); at the gate every P9-B0 reference (R-2, X-10, Y-1, Y-F0 u / w / NAV) was produced on the Release p9-3f exes, the Y-F0 u / w / NAV hold against M1d's Debug references, and the re-pin pins the Release exes; canaries 15 / 15 on p9-3f | this report, steps 1, 4, 6 |
| G-P4 CTest | **ticked**: `atx_research` 291 and `atx_equity_strategy` 728 register and run on both trees, 0 failed | `ctest-exits.txt` |
| G-P5 no Python mirror | **PARTIAL** (IC-S at GATE-0: four pairs unlisted, P10 retirement; stays PARTIAL in the freeze print, GP-TICKS); the guard green, "G-P5: 0 mirrored rule function(s) in 0 row(s) left for P9 lanes" | `pyt-mirror-guard-rel-s0.log`, `handoff-root-gate0.md` |
| G-P6 no versioned script | **ticked** (M1d); `scripts/tests/test_no_versioned_scripts.py` green inside both whole `scripts/tests` runs | `pyt-scripts-tests-rel-s0-rerun.log`, `pyt-scripts-tests-rel-s1.log` |
| G-P7 statistics of record | **ticked**: B2 merged (the engine `eval` verb is the only producer of the statistics, tied to the frozen eval_tie oracle, `root-wave2-merge-report.md:1583-1602`), B2 fix 2 merged (known-red set EMPTY); at the gate the n37 legacy test passed on the Release verb (inside agent 2's whole `atx-impl/tools` run, 770 passed, 0 skipped; B2-N37 item 4), PY-S S-6 is equal, and step 5's rho came from the Release verb `exes.eval` now pins | `pyt-impl-tools-systmp-rel-s0.log`, `pys-s6-ledger-n.txt`, `rho-pairs-1.txt`, `rho-pairs-2.txt` |
| G-P8 canary | **ticked**: 15 / 15 on Release p9-3f and on Debug p9-3g | `mega-p9-3f-canary.log`, `mega-p9-3g-canary.log` |
| G-P9 seal | **ticked**: T2 merged (`atx-engine/tools/conftest.py` binds no window globally; three modules take the superseded window by name, T2-GOLD); the seal consumer tests green at step 2 (`atx-engine/tools` 391 passed incl. `test_prepare_research_fields_seal.py`, `test_seal_partitions.py`; fields gtests 110 / 110 on both builds) | `pyt-engine-tools-rel-s0.log`, `gt-rel-atx-engine-research-fields-tests.log`, `gt-dbg-atx-engine-research-fields-tests.log` |
| G-P11 risk model PIT + container | **ticked**: COV-6 answered at the COV review (the as-of trace holds); COV-9 S9 holds at the gate (role `e1c67101`, `universe.delisting.returns_applied = false`, 0 of 990 events); GATE-COVDIFF holds (`cov-diff --legacy` exit 0 on the three container stores); `RiskCovContainer.*` green in both builds' risk tests | `cov9-role-check.txt`, `cov9-role-check-3.txt`, `covdiff-runs.txt`, `gt-rel-atx-engine-risk-tests.log`, `gt-dbg-atx-engine-risk-tests.log` |
| G-P10 artifact store (print, gates nothing) | (a) **landed** (the counts above); (b) **landed** at the gate (435 checked, 0 mismatches, 8 classes); (c) **landed** (two from-scratch digests, both `adbcd348...d474`); (d) **PARTIAL** as M6 states: "three writers hooked, the stage record's side files not" (SQL3-S2 S-2); the X-5 cache identities are M6b items 5 / 6 (cited, FIELDS-X5); SQL-11 holds; (e) **landed** for `atx.run-store/v1` (the extended guard `test_research_store_classes.py` green inside `atx-engine/tools` at step 2) | this table, `pyt-engine-tools-rel-s0.log` |

**Step 7 state: HOLDS** (the X-5 opt-in cache identity cut by FIELDS-X5).

### Rulings acted on by agent 4

| ruling | state | evidence (`gate-logs/`) |
|---|---|---|
| GATE-IDX | cleared by the owner; checked first after step 0 (absent; the 351 originals `155872de50833fca`) and again by `b0_ic.py` before rows 4 and 5 | `cache-lo3-listing-agent4-idx.txt`, `b0-runs.txt` |
| GATE-RELCACHE | C: 60.4 GB free before; after the cold Release u the 351 originals unchanged, the new 552-file listing recorded with its cause; C: 50 GB free at the end | `cache-lo3-listing-after-p9-b0g-yf0-{u,w}.txt`, `cache-lo3-listing-agent4-final.txt` |
| RELCACHE-ALLOW | u and w: every payload byte-identical, every differing leaf listed by JSON path in an allowed class (TIMING, COUNTERS, IDENTITY, M1D-LIST), 0 outside; the tool's first pass under-encoded the ruling's fourth class (disclosed, `yf0u-cmp-pass1.txt`, `tools/CHANGES.txt` (3)) | `yf0{u,w}-cmp.txt`, `yf0{u,w}-classes.txt` |
| FIELDS-SUB-A | applied: step-4 row 1, the rulings table, round-2b-ii item 1 and the substitution list row 1 say path A; "cross-build reference" removed; the classification table kept as the disclosure | this file |
| YF0-NAV-ORDER | row 6 after row 5, reading `p9-b0g-yf0-w` (the receipt names it); `--info-clock-print`, `--ann-mean-gross`; `ucrtbase.dll` re-recorded | `b0-runs.txt`, `ucrt-b0-4.txt` |
| PRE34-WHO | nothing run (the cells root's); the path-level diff of agent 3 stands | `y1-cmp-v8.txt` |
| PRE-2BII-B2A / PAR-1 | `p9-prereg.md` never added or committed by this agent | git |
| GATE-MYPY | nothing staged at start; every commit by explicit pathspec (the exclude carried on the record commits), `git show --stat` 0 `mypy_cache` each | git |
| B2-13 | rho value and flag only, three pairs | `rho-pairs-{1,2}.txt` |
| W1-I2 (a) / (b), B2-10 (b), B2-11 | the re-pin `2078f877`; `verify` holds; `exes.eval` named and pinned | `repin-verify.txt` |
| COVMV-PIN | `mv-aim-v1.json` not edited (`4b4af6e2`) | step 6 |
| EXE-FREEZE | starts at `2078f877`; no exe rebuilt (no build tag used); 27 / 27 + 27 / 27 `disk = receipt` at the end | `exe-sha-{rel,dbg}-4-end.txt` |
| SQL3-2 / SQL-11 | no `store` block in any receipt of this agent; `build-equity/research-store/` absent throughout; no cache-index flag anywhere | `b0-runs.txt`, `realcat-gate.txt` |
| RUN-1 / RUN-2 / COVR-1 | every real-data run bounded (`--seconds 600`; NAV / rho / catalog `--max-rss-mib 2048`, u 2,560, w 3,072 per C.14); free RAM stated per run | `b0-runs.txt` |
| TEST-2 | no suite run by agent 4 | - |
| READ NO STATISTIC | no return / IC / Sharpe / turnover / NAV statistic read or recorded; outputs compared by SHA-256 and JSON path; the catalog's and verify's outputs hold paths, digests and counts only | - |
| LAPSE-TABLE | fit mode flag-absent; P9-W LAPSED (0); P9-X pending PRE-30 / PRE30-MVX | round-2b-ii table |

### Kept and deleted outputs (agent 4)

Kept (P9-B0 reference dirs, never deleted): `build-equity/p9-b0g-yf0-u`, `p9-b0g-yf0-w`, `p9-b0g-yf0-nav` (each with its
`-run` dir); the run dirs `build-equity/p9-b0g-rho-2-run`, `p9-b0g-cat-1-run` (the void attempt), `p9-b0g-cat-2-run`,
`p9-b0g-catv-1-run`. Deleted by literal name (regenerable scratch outside the tree): the catalog DBs
`C:/Users/natha/AppData/Local/Temp/g4cat/` (catA / catB 17.4 MB each, the verify outputs) and `g4fx/` (the two fixture
catalogs), after their records. Disk: the cache grew by 4,466,406,032 bytes (GATE-RELCACHE, accepted); C: 50 GB free.

### Step 6, agent 4, follow-up: the second re-pin commit **`3a1a4759`** (rulings REPIN-UW, REPIN-R2; ledger :999-1000) and its verify: **REFUSES, STOPPED**

ONE code commit, pins only: `git show --stat 3a1a4759` = `scripts/specs/v8/lib-v8ysb-gm.json` (+2 / -2) and
`scripts/specs/v8/lib-v80.json` (+3 / -3); 0 `mypy_cache`; no exe touched (EXE-FREEZE unaffected, no build). The re-pin
of record is now **`2078f877` + `3a1a4759`**.

| spec | SHA-256 before -> after | pins moved |
|---|---|---|
| `lib-v8ysb-gm.json` (Y-F0) | `3631679c...edea` -> `4081f473173cd0068bb173a2cb3b2b761592b791705cf7be33bc95fe3f06e2ee` | `ic.u_output` -> `build-equity/p9-b0g-yf0-u`; `ic.w_output` -> `build-equity/p9-b0g-yf0-w` (REPIN-UW) |
| `lib-v80.json` (R-2) | `306a070b...3a21` -> `7f6e45ee53c1854f7a3147f0a1b6e1021d67bf854f0ae9c57c17c37e1fb1bb89` | `fields.output` -> `build-equity/train-2020-2023-lo3-fields-v15`; `fields.manifest_sha256` -> `26fee5ce...3b09` (re-hashed from the file); the 70-name `list` unchanged; `nav.output` -> `build-equity/p9-b0-r2-nav` (REPIN-R2); nothing else |

**`verify` on the pinned lineage after `3a1a4759`** (`gate-logs/repin-verify-2.txt`; `tools/repin_verify.py`, now
also with `--fields=`: the as-built fields check a `run` makes on a done fields step, `scripts/cycle/gate.py:20-28`, on
the wave's `verify=False` view; informational before, recorded now):

1. **u / w not reusable (REFUSES REPIN-UW's premise).** For X-10 and Y-1 (the templates over Y-F0) every pin still holds
   (inputs `[locked, verified]`, the three exe pins = disk, `exes.eval` pinned, NAV made by `255b58ac` = the pin), but the
   inherited u and w now resolve as **`build-equity/p9-b0g-yf0-u-1` and `p9-b0g-yf0-w-1`, state `pending`**: the cycle
   names an IC pass `<u_output>-<n>` with run dir `<u_output>-run<n>` (`scripts/cycle/phases.py:566-568`, `:596-598`),
   and the P9-B0 dirs are exe-direct runs named `p9-b0g-yf0-u` / `-run` (no `-<n>`). So a NAV-only cell over Y-F0
   would NOT resume from the parent's u_output: its driver would run u and w again (Release, into `p9-b0g-yf0-{u,w}-1`)
   and its NAV would read `p9-b0g-yf0-w-1/train_combined.json`. (The same state was seen in the working tree before
   `2078f877`, recorded in that step's text above as the reason the outputs were left at the v8 dirs.)
2. **R-2's fields check REFUSES.** `lib-v80.json` on the wave's view: fields `train-2020-2023-lo3-fields-v15` pin
   `26fee5ce` = disk, but "84 rows DIFFER from the spec list" -> `fields check FAILED (the pinned manifest's field rows
   differ from fields.list)`: a `run` of the R-2 spec (OD-3's roles cycle reads it) stops at its done fields step
   because the 70-name `list` is unchanged (as ruled) while v15 has 84 rows. Its NAV output `p9-b0-r2-nav` was made by
   the Release NAV exe `255b58ac`; the spec pins no `exes_sha256` (none was ruled for it).
3. Unchanged and pre-existing: `lib-v8ysb-gm.json` and `lib-v80.json` alone cannot be built by the cycle (their own
   `inputs.baseline_fields` are fields v13 / v9, FIELDS-GONE); Y-F0's as-built fields check holds (84 rows = its list);
   the parents' NAVs (`p9-b0g-yf0-nav`, `p9-b0g-x10-nav`) made by the pinned Release NAV exe.

**Fixture chain digest after `3a1a4759`** (`gate-logs/fixture-chain-5.txt`): two fresh catalogs of the store fixture
tree, both `catalog_digest a50fdcf9c9c83bdac0d41ff368666da31578ebb4407bbc4498175a63014f100c`; `verify --pins` exit 0;
render identity 12 / 0: **holds**.

**STOP (the PM's instruction: a verify refusal is recorded and handed back, no second attempt).** Nothing reverted;
`3a1a4759` stays as committed; the reference dirs are untouched. What would close each refusal is the PM's ruling
(not tried here): item 1 needs the u / w references under the cycle's IC-pass names or the outputs left at the v8 dirs
(`2078f877`'s state); item 2 needs `lib-v80.json`'s `fields.list` set to v15's 84 rows (not a pin) or its fields pin
left at v10.

### Step 6, agent 4: REPIN-REV (ledger :1003; supersedes REPIN-UW and REPIN-R2): the revert **`f8a85ad4`**

`git revert --no-commit 3a1a4759`, then one commit by pathspec (`chore(p9): revert 3a1a4759: the re-pin of record is
2078f877 (REPIN-REV)`): 2 files (`lib-v8ysb-gm.json` +2 / -2, `lib-v80.json` +3 / -3), 0 `mypy_cache`; **`git diff
2078f877 f8a85ad4 -- scripts/specs/v8/` is EMPTY** (0 bytes): both specs are again their bytes at `2078f877`
(`lib-v8ysb-gm.json` `3631679c...edea`, `lib-v80.json` `306a070b...3a21`). No uncommitted work touched (the PM's
`progress.md` and `r12-prompts/cells-prompt.md` edits and the five PNGs left as they are). **The re-pin of record is
`2078f877`**; EXE-FREEZE runs from it; 27 / 27 Release exes `disk = receipt` after the revert.

The two refusals of `3a1a4759`, quoted from `gate-logs/repin-verify-2.txt`:
1. `u       pending  build-equity/p9-b0g-yf0-u-1` and `w       pending  build-equity/p9-b0g-yf0-w-1` (X-10 and Y-1 alike):
   the cycle resolves the u / w pins as attempt roots of exe-direct dirs that carry no cycle state, so every NAV-only
   cell would re-run u and w.
2. `fields check: pinned build-equity/train-2020-2023-lo3-fields-v15/manifest.json sha256 26fee5ce...: 84 rows DIFFER
   from the spec list` -> `fields check REFUSES: fields check FAILED (the pinned manifest's field rows differ from
   fields.list)` (`lib-v80.json`).

**`verify` on the pinned lineage after the revert: HOLDS** (`gate-logs/repin-verify-3.txt`): X-10 HOLDS, Y-1 HOLDS (u
`mega-v8-b0b-train-u-v8ysb-1` and w `mega-v8xw-train-theme-erc-v8ysb-2` done and reusable; NAV made by `255b58ac` = the
pin; `exes.eval` pinned); the parents' NAVs `p9-b0g-yf0-nav` / `p9-b0g-x10-nav` made by the pinned Release NAV exe; Y-F0's
as-built fields check holds (84 rows = its list). The verify section is line-for-line identical to the `883be9f2`
record `repin-verify.txt` (66 / 66 lines); the one REFUSED line is the pre-existing standalone build of
`lib-v8ysb-gm.json` (its own `inputs.baseline_fields` is fields v13, FIELDS-GONE), the same as at `883be9f2`; that line
alone makes the file end "tool exit 1" (`tools/repin_verify.py` returns 1 on a cycle that cannot be built; GATE-REV
S-2), as `repin-verify.txt` did: every spec verified ends "HOLDS".
**Fixture chain digest after the revert** (`gate-logs/fixture-chain-6.txt`): `a50fdcf9...100c` twice, `verify --pins`
exit 0, render identity 12 / 0: holds.

The two roles of Y-F0's u / w (REPIN-REV): **`build-equity/p9-b0g-yf0-u` / `p9-b0g-yf0-w` are the references of record
for provenance** (the Release IC exe p9-3f, their receipts, the RELCACHE-ALLOW identity with M1d's references; the NAV
of record `p9-b0g-yf0-nav` reads `p9-b0g-yf0-w`); **the v8-era cycle dirs `mega-v8-b0b-train-u-v8ysb-1` /
`mega-v8xw-train-theme-erc-v8ysb-2` stay the spec's `ic.u_output` / `ic.w_output`: the cycle's resume roots (completed
state)**, byte-identical to the Release references in every payload (u 3 / 3, w 10 / 10; checked at the re-pin); no
cell re-runs u or w. R-2 is read by path (`p9-b0-r2-nav`); `lib-v80.json` untouched, its stale pins stated in the
substitution list (amendment 4, PRE-40 form).

## FLAGS-ROUTE (RQ-1 close), root FLAGS agent

RESUME 14, 2026-10-16; dispatch `r14-prompts/root-flags-prompt.md`; rulings FLAGS-ROUTE, FLAGS-ROUTE-2, FLAGS-SEQ (1)
(ledger, the block from "- RESUME 14"); GATE-REV part 2's one Required finding RQ-1 (`prereg-2bii-review.md`), route
(b). Pool `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. No build, no suite (TEST-2), no statistic read
(READ NO STATISTIC: SHA-256 and JSON path only).

**Step 0 (start state, DISPATCH-COMMIT).** `gate-logs/flags-start.txt`: code head `f8a85ad4` = the re-pin of record
`2078f877` outside `.superpowers/` (diff empty); docs head at start **`cb051169`**, not the dispatch's `6c608ff3`: the
two commits above it are the PRE docs agent's pathspec commits beside me (FLAGS-SEQ (2), PAR-1: `bc034c4a` SG-1 in
`p9-prereg.md`, `cb051169` SG-2 in `p9-od3-runbook.md`; nothing else), expected, not a stop. Trial ledger 133 lines
`27e40f9f`; `research-store/` and `index.sqlite` absent; the six P9-B0 reference dirs present; the NAV exe
`255b58ac...3565` and 27 / 27 Release exes = their p9-3f receipt; `ucrtbase.dll` 10.0.26100.7623 `2C08E77F...65EF`;
free RAM 3.39 of 15.69 GB, C: 40.7 GB free. DISPATCH-COMMIT **`a70f406d`** (11 paths by explicit pathspec: the ledger
and `next-goal-prompt.md` as they stand, `status-14.md`, `r13-prompts/{cells-prompt-1,gaterev-prompt-1}.md`, the five
`r14-prompts/` files, `gate-logs/flags-start.txt`; 0 `mypy_cache`, 0 PNG).

### Route facts (read-only, at code head `f8a85ad4`, BEFORE the spec edit)

| # | fact | source (quoted) | holds |
|---|---|---|---|
| 1 | The lineage parent's NAV flags carry neither flag | `scripts/specs/v8/lib-v8ysb-gm.json` (sha256 `3631679cd18f1c6a63a6a5dc96cdbe6ee7669b81b8979f529544f1b237d9edea`, LF, 2-space indent) `:250-282`: `"nav": {"output": "build-equity/p9-b0g-yf0-nav", "rule": "aim-partial-v5", "leverage": "1.1828", "flags": ["--cadence", "1", "--trade-fraction", ".05", "--dust-multiple", ".1", "--aim-leverage", "{leverage}", "--daily-turnover-mean-max", ".20", "--daily-turnover-p95-max", ".30", "--neutralize", "price-risk-v1", "--max-bytes", "1073741824", "--order-basis", "delta", "--exit-rate", ".05", "--locate-in-aim", "--liquidity-cache", "--warm-start-sessions", "60", "--capacity-curve"]}` (25 tokens) | yes: = the dispatch's list; no `--ann-mean-gross`, no `--info-clock-print` |
| 2 | X-10 inherits the parent's list | `scripts/specs/v8/x-leverage-L2.0.json`: `"parent": "lib-v8ysb-gm.json"`, `"change": {"set": {"nav.leverage": "2.0", "nav.output": "build-equity/p9-b0g-x10-nav"}}`; no `nav.flags` key, no `change.flags`, no `change.unset` | yes |
| 3 | Y-1 adds to the inherited list, replaces nothing | `scripts/specs/v8/y-vol-target-y-1.json`: `"parent": "x-leverage-L2.0.json"`, `"change": {"set": {"nav.output": "build-equity/p9-b0g-y1-nav", "nav.leverage": "2.0"}, "flags": {"nav": {"--vol-target": "vol-target-v1", "--risk-model": "build-equity/p9-m2i-dcov-v11", "--risk-model-sha256": "397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7"}}}`; `change.flags` semantics `scripts/research_spec.py:12-17`, `apply_flags` `:128-163` ("set the option's value (appended when absent)"): three options absent from the parent's list, so six tokens appended; no `nav.flags` override | yes |
| 4 | The four P9 templates have no parent of their own and set outputs only | `scripts/specs/p9/templates/info-clock.json`, `mom-volman.json`, `mv-aim.json`, `theme-cov-hedge.json`: each `"parent": null`; `change` keys `["set"]` only; `change.set` keys: info-clock / mv-aim `nav.output`, `monitor.output`; mom-volman / theme-cov-hedge `fit.output`, `card.output`, `ic.w_output`, `nav.output`, `monitor.output` (`nominal_parent` of info-clock / mv-aim `../../v8/lib-v8ysb-gm.json`); no `nav.flags`, no `change.flags` | yes: once the cells root sets `parent` to the FILE at registration, each inherits the parent's list |
| 5 | Every other spec over the parent | `git grep lib-v8ysb-gm.json -- scripts atx-impl atx-engine`: the v8 cells `y-norm-score-y-3{,-gm}.json`, `y-theme-tsmom-y-2{,-gm}.json`, `y-two-speed-y-5{,-gm}.json` (`"parent": "lib-v8ysb-gm.json"`; `change.set` `nav.output` (+ `nav.leverage` in `-gm`); `change.flags.nav` adds `--rank-shape` / `--two-speed` or nothing; no `nav.flags` override) and the v8 waves `y-2` / `y-3` / `y-5` naming it as their spec | they inherit too; v8 cells (y-2 / y-3 / y-5 NOT ACCEPTED), not run in P9 |
| 6 | The driver builds the NAV argv from the resolved spec's `nav.flags` | `scripts/cycle/phases.py:824` (`nav_step`): `flags = [str(nav.get("leverage")) if f == "{leverage}" else f for f in nav["flags"]]`; `:829-832`: `argv = self.runner(run_dir, binds, phase, k) + [s["exes"]["nav"], "nav", "--combined", comb, "--combined-sha256", comb_sha, "--role", role_m, "--role-sha256", role_sha, "--fields", fdm, "--fields-sha256", self.rt_sha(fdm), "--output", n_out, "--rule", nav["rule"], *flags, *label]` with `label = ["--label-role", ..., "--label-role-sha256", ...]` (`:828`) | yes; and by construction the label pair follows `*flags`: tokens appended at the END of `nav.flags` reach the argv BEFORE `--label-role`, while the reference receipt carries them after it (an order difference, listed at step 4) |
| 7 | The lane's own test shows a parent `--info-clock-print` reaching the cell exactly once | `scripts/tests/test_info_clock_rule.py:217`: `assert flags.count("--info-clock-print") == (1 if parent_flags else 0)`; `:222`: `assert argv.count("--info-clock-print") == (1 if parent_flags else 0)` (the "printed" world's parent flags carry it, `:203-204` of `test_kind_rule_wave_puts_one_rate_and_one_rule`, `:201`) | yes (read; the test file not run: not needed as evidence) |

**Consequence (FLAGS-ROUTE-2, stated before the spec commit; the ruling's clauses, not its reasons).** (a) After
FLAGS-ROUTE every spec built over `lib-v8ysb-gm.json` inherits `--ann-mean-gross` and `--info-clock-print` on its NAV
argv (facts 2-6), so B.0's void rule (2) "alike" holds for `--ann-mean-gross` on every cell / reference pair and for
`--info-clock-print` on every v2 cell against `p9-b0g-yf0-nav` or an accepted v2 parent, P9-C included. (b) A hand cell
written from P9-F0h's NAV argv (P9-X, P9-V, if they run) against `p9-b0g-y1-nav` or P9-X may carry `--info-clock-print`
on the cell side only: NOT a void; the one-sided `/scenarios/*/info_clock` block is disclosed in the cell record (the
form of GATE-REV S-5); the void rule (2) wording stays as written, restated beside by the PRE docs agent (PRE-40 form).
(c) The edit moves `lib-v8ysb-gm.json`'s SHA-256 off `3631679c...`: every citation of that value is restated beside
(PRE-40 form); the cells root sets each template's `parent` to the FILE and `lock --exes --write` pins what it
inherits; the re-pin of record stays `2078f877` (no pin moves), EXE-FREEZE is unaffected, and the gate's catalog
digest `adbcd348...` is not re-run. Stated, not decided (fact 3 with the edit): the X-10 and Y-1 reference SPECS will
also plan `--info-clock-print`, while their reference DIRS `p9-b0g-x10-nav` / `p9-b0g-y1-nav` were exe-direct runs with
`--ann-mean-gross` only; those dirs are read by path as references (never re-run by a cell), and the Y-1 side is the
case (b) names.

### Step 2: the spec commit **`f8d852f3`** (ONE code commit, ONE file)

`spec(p9): lib-v8ysb-gm.json nav.flags += --ann-mean-gross --info-clock-print (FLAGS-ROUTE; RQ-1)`, by pathspec
`scripts/specs/v8/lib-v8ysb-gm.json` only (Edit tool; no re-serialisation). `git diff --stat` = 1 file, +3 / -1; the
diff is exactly the `nav.flags` array's tail: `"--capacity-curve"` -> `"--capacity-curve",` (the JSON comma) and the two
added lines `"--ann-mean-gross",` / `"--info-clock-print"` (the reference receipt's order); no key reordered, LF and the
2-space indent kept (0 CR bytes before and after). JSON path `/nav/flags`: 25 -> 27 tokens. **SHA-256
`3631679cd18f1c6a63a6a5dc96cdbe6ee7669b81b8979f529544f1b237d9edea` -> `a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a`**
(blob `9dd63a00` -> `f0c3acd2`). Not a pin move: the re-pin of record stays `2078f877`; no exe touched (EXE-FREEZE);
`git diff 2078f877 f8d852f3 -- . ':(exclude).superpowers'` = this one file. Not edited: `x-leverage-L2.0.json`,
`y-vol-target-y-1.json`, `lib-v80.json`, any template, rule, registration or fixture.

### Step 3: `verify` on the pinned lineage and the fixture chain digest (after `f8d852f3`)

**`verify`** (`gate-logs/tools/repin_verify.py`, the arguments of `repin-verify-3.txt`: X-10, Y-1, `lib-v8ysb-gm.json`,
`--parent=` Y-F0 and X-10, `--fields=` Y-F0) -> **`gate-logs/flags-verify.txt`: holds.** Every pin `[locked,
verified]` (library, recipe, role, label_role, reference_cell, reference_admission) and the three exe pins
`[exes_sha256, verified]` = disk; `exes.eval` named and pinned (B2-11); **X-10 HOLDS**, **Y-1 HOLDS**; fields, u, fit,
card, w, monitor done, u / w = the v8-era resume roots `mega-v8-b0b-train-u-v8ysb-1` / `mega-v8xw-train-theme-erc-v8ysb-2`;
NAV made by `255b58ac` = `exes_sha256.nav` (W1-I2 (a)); the parents' NAVs `p9-b0g-yf0-nav` / `p9-b0g-x10-nav` made by
the pinned Release NAV exe; Y-F0's as-built fields check holds (84 rows = its list). The ONE REFUSED line is the
pre-existing standalone build of `lib-v8ysb-gm.json` ("REFUSED (the cycle cannot be built): input baseline_fields
missing: build-equity/train-2020-2023-lo3-fields-v13/manifest.json"; FIELDS-RISK, FIELDS-GONE): expected, not a stop;
that line alone makes the file end "tool exit 1" (`repin_verify.py` returns 1 on a cycle that cannot be built; GATE-REV
S-2), exactly as `repin-verify-3.txt`.

`diff repin-verify-3.txt flags-verify.txt` (72 / 72 lines): **5 differing lines, each expected**:

| line | what differs | cause |
|---|---|---|
| 1 | the header (head, timestamp) | this record |
| 4 | X-10 `spec digest over the template chain` `8127dbd8...d140` -> `ea5c4578ac6b851f81052e76121eb2bc8d4d145f5e78cbbca4863821df872ed8` (the file's own sha256 `46e9d6c2...` unchanged) | the edited parent is in X-10's chain |
| 27 | X-10 `nav` line, C-13 binding note: "this spec's NAV command is argv sha256" `108e1c2d...58a8` -> `40ff035d18ef419751c5bea138a71117df47a79dd82c93d5fc6c9e64c5e832fb` (the reference dir's own argv sha256 `b9b0decb...` unchanged; informational, not counted, as before) | the planned NAV argv now carries the two tokens (the planned flag lists themselves: step 4's `flags-plan.txt`) |
| 33 | Y-1 spec digest over the template chain `b0c7ad1c...656f` -> `41360ef2af5c8c39d3840f6f3fdbfa5b6cc3ada6457d7f8eb4a28486222858be` (file sha256 `f4186573...` unchanged) | the edited parent is in Y-1's chain |
| 57 | Y-1 `nav` line, "this spec's NAV command is argv sha256" `61de47f4...824f` -> `9dd016ffef87c58f3a800b7cfb6893ef5ce3a2bd246919dfc2f72a46e9c1b859` (reference `badecf1f...` unchanged) | as line 27 |

Nothing else differs (no pin line, no state, no exe, no REFUSED line other than the FIELDS-RISK one).

**Fixture chain digest** (`gate-logs/tools/fr_fixture.sh`, the form of `fixture-chain-6.txt`; Release store exe p9-3f
`0eeac21b...`; DBs outside the tree under `C:/Users/natha/AppData/Local/Temp/f1fx/`) -> **`gate-logs/fixture-chain-7.txt`:
`catalog_digest a50fdcf9c9c83bdac0d41ff368666da31578ebb4407bbc4498175a63014f100c` twice** (both `files_seen 28 verified
25 declared 0 skipped 4`), `verify --pins` exit 0, identity "total checked 12 mismatches 0": holds (= the record).
