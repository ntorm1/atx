# GATE-REV part 1: review of the gate report (GATE-1), P9, 2026-10-16

Reviewer: one fresh GATE-REV reviewer (Opus; rulings GATE-REV, GATE-REV-PAR, REV-FRESH). Dispatch
`r13-prompts/gaterev-prompt-1.md`; task text `r12-post-gate.md` "GATE-REV" Part 1. Pool `C:/atx-wt/pool-2`, branch
`feat/platform-p9-20261003`. Part 2 (`prereg-2bii-review.md`) follows on the PM's message.

**Verdict: RETURN** for step 4, records only. No lane is implicated and no merge commit; root redoes no run and no
code. One finding is Required (R-1): the brief's per-reference record item "the identity-bridge flag line (OD5-1)"
is missing from every P9-B0 reference record. Every measured claim of steps 0-9 holds as recorded and checked below.
Once R-1 is closed (one root docs commit), the gate is PROCEED.

## State at review and what was read

- Code head `f8a85ad4`: `git diff 2078f877 HEAD` outside `.superpowers/` is 0 bytes, so the code is the re-pin of
  record. Docs head `d22246f6`. Working tree: the PM's `progress.md` (ledger to :1006), the PRE agent's uncommitted
  `p9-prereg.md` (B2b in progress; part 1 read only B.0's void-rule wording at :576-582), and the five untracked PNGs
  (never opened).
- Read: `resume12-addendum.md`; `r13-prompts/gate-prompt-{2,3,4}.md`; `r12-post-gate.md` (whole); `root-gate-brief.md`;
  `r11-dispatch.md` "ROOT GATE" + "ROOT GATE: rulings from the whole-wave review"; `r12-dispatch.md` "ROOT GATE";
  `root-gate-report.md` (whole, 1,119 lines); `handoff-root-gate.md` and `handoff-root-gate-{1,2,3}.md` (whole);
  `integration-log.md:799-928` (the gate rows); `handoff-root-m6b.md:77-105`; the ledger rulings the dispatch names
  (full text) and the RESUME 13 block :952-1006 whole.
- Blind: I read and wrote no 2020-2023 return / IC / Sharpe / turnover / NAV statistic. I read evidence files by
  structure only (paths, SHA-256, counts, exit codes, verdict lines). I hashed output directories but never parsed
  them. The rho values the report prints (B2-13: value and flag) are not reprinted here.
- Trial ledger `build-equity/trials.jsonl`: 133 lines, sha256 `27e40f9f6314585e...` (read by `wc -l` / `sha256sum`
  only).
- Permission refusals: none.

## Independent checks run by GATE-REV (read-only; no build; no real-data run)

1. **Re-pin diff.** I read `git show 2078f877` whole. It changes only exe paths, adds `exes.eval`, adds `exes_sha256`
   {eval, ic, nav}, and moves three `nav.output` pins, two `locked.reference_cell` path + SHA pairs and Y-1's
   `--risk-model` path + SHA: pins only. `git diff --name-only 06dc9e5a 2078f877` outside `.superpowers/` lists the
   three specs. `3a1a4759` and `f8a85ad4` each touch `lib-v80.json` + `lib-v8ysb-gm.json`, +5 / -5, and mirror each
   other. `git diff 2078f877 f8a85ad4 -- scripts/specs/v8/` is 0 bytes.
2. **Re-hash, all equal to the records:**
   - The 10 Release exes of the frozen set (NAV `255b58ac`, IC `ccd54db6`, eval `e2b48c46`, composition `04db0d67`,
     admission `f53d9277`, store `0eeac21b`, risk `0c110534`, fields `883c5b74`) equal the handoff table.
   - The specs and pins: `lib-v8ysb-gm.json` `3631679c`, `x-leverage-L2.0.json` `46e9d6c2`, `y-vol-target-y-1.json`
     `f4186573`, `lib-v80.json` `306a070b`, `mv-aim-v1.json` `4b4af6e2`, `info-clock-rate-v1.json` `7801c198...a152`,
     the v1.1 store manifest `39736647...23c7`, fields v15 manifest `26fee5ce`, `classes.json`
     `5bc27ba489f24a0d1c8c42c8c29927c8d1bde6bf63cd5993f285a5b084086557`.
   - `ucrtbase.dll` 10.0.26100.7623 `2C08E77F...65EF`.
3. **The six P9-B0 reference dirs**, re-hashed file by file with my scratch tool. Each dir's on-disk listing equals
   the committed `gate-logs/b0-files-<dir>.txt` byte for byte. Each listing's SHA-256 and each `summary.json` SHA-256
   equal the report's and the handoff's: `p9-b0-r2-nav` 27 files `04bb8060`, `p9-b0g-x10-nav` 27 `6c87c8d5`,
   `p9-b0g-y1-nav` 28 `cac882e1`, `p9-b0g-yf0-u` 5 `1cb087ab`, `p9-b0g-yf0-w` 12 `ec693440`, `p9-b0g-yf0-nav` 27
   `e205d6ec`.
4. **REPIN-REV premise.** The v8 resume roots `mega-v8-b0b-train-u-v8ysb-1` (3 / 3 payloads) and
   `mega-v8xw-train-theme-erc-v8ysb-2` (10 / 10) hash-equal `p9-b0g-yf0-u` / `-w`.
5. **YF0-NAV-ORDER's "the path token moves no output byte".** `grep -rlF train_combined.json` over `p9-b0g-yf0-nav`
   and `p9-b0-yf0-nav` matches no file.
6. **The real cache, re-listed** with `gate-logs/tools/dir_listing.py` (stat only): 552 files, `c9ee9291f5ddc0c2`,
   byte for byte `cache-lo3-listing-agent4-final.txt`. Restricted to the non-Release rows: 351 files,
   `155872de50833fca`. `index.sqlite` is absent and `build-equity/research-store/` is absent.
7. **Suite counts.** I summed `gt-exits.txt` (Release 4,098 run / 0 failed / 9 skipped; Debug 4,099 / 0 / 6). The
   SKIPPED names grepped from every `gt-*.log` are exactly the report's named skips, and no log has a
   `[  FAILED  ]` line. The ctest logs say "0 tests failed out of 291 / 728" on both trees. The `pyt-exits.txt` summary
   lines match the report. The three `scripts/tests` skips are `test_research_cycle.py:56`, `:706`, `:946` (live-root
   opt-ins). ENV-1 has 0 hits everywhere.
8. **Ancestry.** A4's `69141218`, `59d807ea`, `fc3e414e`, `916f837f` are not ancestors of HEAD. SQL4 `44a76937` and
   GATE-0's `7320e1b9` / `22f99018` are.
9. **The 20 commits `7890883e..HEAD`.** Every one has the trailer; 0 `mypy_cache` / `__pycache__` paths in any of them;
   `p9-prereg.md` is touched only by the PRE agent's `aef2e033` / `1e2dbf3c` (PAR-1).
10. **Re-run.** I checked the host first (no build, HOST-4) and re-ran `scripts/tests/test_no_python_mirror.py` once
    (explicit path, `-p no:cacheprovider`, own basetemp, `PYTHONDONTWRITEBYTECODE=1`): 7 passed, "G-P5: 0 mirrored
    rule function(s) in 0 row(s) left for P9 lanes".
11. **Statistic scan.** I listed every decimal figure in `root-gate-report.md` and `handoff-root-gate*.md`. The only
    ones are the three rho values (value and flag, B2-13) and the `scoreboard --timings` wall seconds. **No statistic
    is printed** (no Required finding from the blind check).
12. **Committed records.** All 223 files under `gate-logs/` are tracked; nothing is untracked except the git-ignored
    editor cache. No `@@` placeholder is left in any gate record.

## A. The brief's steps 0-9 (`root-gate-brief.md`)

| step | claimed | evidence | checked | verdict |
|---|---|---|---|---|
| 0 start state | Each of the four agents re-verified the start: heads, tree, ledger 133 / `27e40f9f`, `research-store/` absent, `ucrtbase.dll`, RAM / disk, exes `disk = receipt` | `start-state.txt`, `start-state-2.txt`, `exe-sha-{rel,dbg}{,-3,-4,-4-end}.txt`; report "Step 0" sections | The files exist. `exe-sha-*-4-end.txt` 27 / 27 each "disk = receipt". The ledger, `ucrtbase.dll` and research-store absence hold today (checks 2, 6) | **holds** |
| 1 exes of record | Release p9-3f and Debug p9-3g; 27 targets each; 0 warnings; canaries 15 / 15; SPDLOG-RELINK 27 / 27 + 27 / 27 | `builds.txt`, `mega-p9-3{f,g}-receipt.json`, `-canary.log`, `exe-sha-{rel,dbg}.txt` | `builds.txt`: exit 0 and warning-lines 0 for both. Receipts: ExitCode 0, Source `5f48a25d`, 244 / 34 and 68 / 19, 27 executables each. Both canary logs "15 passed". My re-hash of the frozen Release exes equals the table (check 2) | **holds** |
| 2 suites | Release / Debug gtests 0 failed, named skips; ctest 291 + 728 on both trees; `scripts/tests` seed 0 re-run and seed 1 703 / 0 / 3; stage chain 10; `atx-engine/tools` 391; `atx-impl/tools` 770; strategies 14; fields 4 / 26 / 16 / 37; mirror 7; cov-diff 3 x exit 0; the two reds were the gate tool's TMP root | `gt-exits.txt`, `gt-*.log`, `ctest-exits.txt`, `ctest-*.log`, `pyt-exits.txt`, `pyt-*.log`, `covdiff-runs.txt`, `cov9-role-check.txt` | Check 7: the counts and every skip name match. The two reds are each shown as the tool's: the W1-I1 refusal under the long TMP root (single test 1 passed on the short root), and `test_v7_config_renders_the_pre_v8_bytes` failing only on the 38-character TMP (`tools/CHANGES.txt`). Mirror file re-run green (check 10) | **holds** |
| 3 identity, canaries | Y-F0 NAV identity, argv and `--spec`; ALCLOCK-1; Y-F0 fit (D2 stamps); D3-Q1a DIFFERENT; COMPOSE-1a ACCEPT; D3-Q2 / D3-S1 0 / 0; golden at 1 / 4 workers; canaries; Y-F0 u / w (rows 4-5); X-5 CUT | `nav-identity-yf0.txt`, `fit-identity-yf0.txt`, `d3q1a.txt`, `compose-1a.txt`, `d3-greps.txt`, `yf0{u,w}-cmp.txt` | **NAV:** 26 / 27 identical; 2 `/producer` leaves; VERDICT inside. **ALCLOCK-1:** 70 leaves, all under `info_clock`; the document minus those blocks equals flag-absent: True. **Fit:** the tool's VERDICT is OUTSIDE its list exactly at the CM-5 stamp leaves (`script_sha256` -> `producer_fingerprint`, `module_sha256` -> `producer_fingerprint`, `admission_sha256`). M2d's written P9-B0 list names exactly these leaves (`root-wave2-merge-report.md:668-686`, merge `61378337`), and the PM ledgered "holds" at :974. **D3-Q1a:** the admission json / csv SHA pairs are equal; the stripped weights pair is `f2454589...b876` vs `decc2f8c...55a8`, unequal at 694 / 2,853 leaves, 0 non-float. **COMPOSE-1a:** exit codes and refusal lines as reported; `nav_lockstep.cpp:225-250` confirms `validate_nav_config` / `validate_nav_input` run after the loads | **holds** (Suggested S-1: R-2's line) |
| 4 P9-B0 | Substitution list `778d34b8` committed first, amendments `bfbe64d2` before rows 4-6; witness 70 / 70; R-2 path A; X-10 holds; Y-1 recorded (PRE-29); Y-F0 u / w / NAV hold; `ucrtbase.dll` at both run groups; no `store` block; READ NO STATISTIC | `b0-runs.txt`, `b0-files-*.txt`, `receipt-*.json`, `fields-witness.txt`, `r2-classify.txt`, `x10-cmp.txt`, `y1-cmp-{b0,v8}.txt`, `yf0{u,w,nav}-cmp.txt`, `ucrt-b0-{3,4}.txt` | **Order (UTC):** `778d34b8` committed 14:15:10 before R-2 started at 14:15:27 (its receipt source is `778d34b8`); `bfbe64d2` committed 15:18:22 before row 4 at 15:18:36. **Receipts:** all six completed exit 0, Release, exe = pin, no `store` block in receipt or start, no eval-mode flag; `--ann-mean-gross` on the four NAV references; `--info-clock-print` on Y-F0's only. **Dirs:** re-hashed equal (check 3). **No statistic** in the report (check 11). **Missing:** the per-reference "identity-bridge flag line (OD5-1)" (`root-gate-brief.md:53-54`) has 0 occurrences in the report, the four handoffs and the integration log | **RETURN, records only (R-1)** |
| 5 rho | P9-X pair, X-10 vs Y-F0, Y-1 vs X-10: value and flag; every flag false; P9-V's references are P9-X's | `rho-pairs-{1,2}.txt`, `receipt-p9-b0g-rho-{1,2}.json`, `tools/rho_pairs.py` | The tool keeps only `paired.{sessions, rho, one_minus_rho, degenerate}` (`:29`). T is 1005 for each pair. The daily-file SHA prefixes are each reference's primary-scenario CSV in its listing. All three flags false. The report prints value and flag only | **holds** |
| 6 re-pin | `2078f877` is pins only, with `exes.eval`; EXE-FREEZE from it; `verify` holds; fixture digest; `3a1a4759` refused at `verify`, reverted by `f8a85ad4`; `verify` holds again | git, `repin-verify{,-2,-3}.txt`, `fixture-chain-{4,5,6}.txt` | **Diff:** check 1. **`repin-verify-3.txt`:** its body equals `repin-verify.txt` line for line (plus one leading blank line), followed by the `--fields=` view (Y-F0 84 rows = list: holds). X-10 HOLDS and Y-1 HOLDS; the exe pins equal disk; u / w resolve to the v8 resume roots, done. Its one REFUSED line is `lib-v8ysb-gm.json` built alone (`baseline_fields` v13; FIELDS-RISK; also at `883be9f2`), and that line causes the file's "tool exit 1" (`repin_verify.py:35-36`). **`repin-verify-2.txt`:** both refusals as the report quotes them (`:21`, `:25`, `:78-79`). **Fixtures 4 / 5 / 6:** `a50fdcf9...100c` twice each, `verify --pins` 0, identity 12 / 0 | **holds** (Suggested S-2) |
| 7 store step, scoreboard, G-P, CRT | Catalog `adbcd348...d474` reproduced from scratch; `verify --pins` counts; render identity 435 / 0; ledger verify; `classes.json` by hand; scoreboard lineage; PY-S S-6 "equal"; CRT; G-P ticks; X-5 opt-in identity cut | `realcat-gate.txt`, `realcat-gate-verify.txt`, `realcat-gate-verify-ledger.txt`, `scoreboard-gate.txt`, `pys-s6-ledger-n.txt`, `crt-record.txt`; report G-P table | **Catalog:** A and B have equal counts and digest ("reproduces: True"); research-store absent before; DBs outside the root (SQL3-2); `verify --pins` lines 2-31 equal the report; `classes.json` re-hashed equal. **PY-S S-6:** VERDICT equal (63 / 62, three ways). **Scoreboard:** checks 4 / 4 true. **CRT:** both stores = pins. **G-P rows:** the named evidence tests appear in the logs (`test_research_store_classes.py`, `test_prepare_research_fields_seal.py`, `test_seal_partitions.py`, `test_no_versioned_scripts.py`, `test_wave_kinds.py`). The catalog ran at docs head `883be9f2` (code `2078f877`, byte-equal to `f8a85ad4`); REPIN-REV re-records only the fixture digest | **holds** (Suggested S-3) |
| 8 whole-wave review | DONE before the chain (WAVE-REV-EARLY, SQL4-ADD) | `wave-review-*.md`, `22f99018` | The commit holds the five files | **holds** |
| 9 records | Report, integration-log rows (agents 1-4, the follow-up, REPIN-REV), final `handoff-root-gate.md`, round-2b-ii facts by item; code and docs commits separate | the files, git | All committed (check 12). The handoff has code / docs head, the exe table, reference dirs with SHA-256, substitution list, `ucrtbase.dll`, cache listings, known-red set, next tag (`p9-3h`) and what the cells agent must know. The items are those of `gate-prompt-3.md`'s list. The three code commits touch specs only. Trailers are present | **holds** (Suggested S-4, S-5) |

## B. "ROOT GATE: rulings from the whole-wave review" (`r11-dispatch.md`)

| ruling | claimed | evidence | checked | verdict |
|---|---|---|---|---|
| STORE-M1 (+ STORE-S S-1) | DONE at GATE-0 in one slip commit | `handoff-root-gate0.md:7-11`, `:29-32`, `:74-75`; `7320e1b9` | `7320e1b9` is in the gate head; the hook file went 13 -> 14 passed with the S-1 case | **holds** |
| IC-S | No `FROZEN_PAIRS` edit (the mechanism is a name registration, not a byte freeze); G-P5 PARTIAL | `handoff-root-gate0.md:90-97`; report G-P5 row | Stated, and carried into GP-TICKS | **holds** |
| five `wave-review-<area>.md` committed | DONE at GATE-0 | `22f99018` | 5 files + ledger | **holds** |
| GATE-EXES | Composition and admission built on both trees, receipted, in the table, frozen | receipts (27 executables each); handoff exe table | Release pair re-hashed equal (check 2) | **holds** |
| GATE-COVDIFF | Three `cov-diff --legacy` bounded runs exit 0 after COV-9; S-7 hand check; `v8-risk-lo3-v10` cited from M2d | `covdiff-runs.txt`, `cov9-role-check.txt`, `covdiff-s7-files.txt` | Exit 0 x 3; "legacy equal: 1405 sessions"; no `store` block; S9 holds (0 of 990); S-7 VERDICT HOLDS x 3, 0 unlisted | **holds** |
| D3-Q1a | DIFFERENT; P9-W LAPSED (0); P9-F0h flag-absent; tool and output committed | `d3q1a.txt`, `tools/d3q1a.py` | Both tracked; SHA pair as in A step 3 | **holds** |
| COMPOSE-1 (by COMPOSE-1a) | ACCEPT at the store-backed parse stage; the limit is stated (validators after the loads = P9-MV's own preflight) | `compose-1a.txt`, `tools/compose_1a.py` | As in A step 3 | **holds** |
| PRE-MVX | The prereg lines are the PRE agent's; the gate states P9-V pairs with P9-X (item 22) | report round-2b-ii item 22 | Gate side stated; the prereg side is part 2's | **holds** (gate side) |
| COVMV-S2 S-b + NAV-S S-2 | `mv-aim-v1.json` not edited (COVMV-PIN, PRE-40 form) | report step 6; handoff | Not in any gate commit (`06dc9e5a..HEAD`); `4b4af6e2` re-hashed | **holds** |
| G-P10 (d) / (e), STORE-S S-4 | (e) landed (extended guard green); (d) PARTIAL; `classes.json` SHA by hand | report G-P10 row; `pyt-engine-tools-rel-s0.log:47` | Guard file passed; SHA equal (check 2) | **holds** |
| Fields area | APPROVE; nothing for root; its pytests ran | `pyt-fields-*.log`, `pyt-field-registry-rel-s0.log` | 4 / 26 / 16 / 37 passed | **holds** |
| Python area | Whole suites on the Release exes; PY-S S-6; SQL3-2 | `pyt-exits.txt` (`build rel`), `pys-s6-ledger-n.txt` | As in A steps 2 and 7 | **holds** |

## C. Rulings of RESUME 13 that bind the gate (ledger :952-1006)

| ruling | claimed | evidence | checked | verdict |
|---|---|---|---|---|
| R13-RESUME | Agent 1's placeholders filled from its logs; records + the PM's files committed | `0c8bc8da` | No `@@` left in any gate record | **holds** |
| TEST-2 | Every cut item had already run and been logged; it stands as run; focused checks (a) single test and (b) cov-diff done | `pyt-exits.txt`, `ctest-exits.txt`, `covdiff-runs.txt` | The last cut-list record ends about 13:25Z; (a) 1 passed (13:26:33Z); (b) 3 x exit 0; no whole suite after it (agents 3 / 4: none) | **holds** |
| GATE-TMP | The red was the gate tool's TMP root, not code; seed 0 re-run green; seed 1 run | `tools/CHANGES.txt`, `pyt-tmp-probe-ruled-root-s0.log`, `pyt-scripts-tests-rel-s{0-rerun,1}.log` | Change logged; the ruled in-tree root refused by K3 is shown | **holds** |
| GATE-TMP-2 | The outside-tree root `C:/Users/natha/AppData/Local/Temp/g2<inv>` is the TMP root of record; the `atx-impl/tools` golden is a P10 note | `pyt-exits.txt` roots | Every line of record names a `g2` root (or `systmp`) | **holds** |
| GATE-IDX | No run named the cache while `index.sqlite` existed; checked before rows 4-5; the owner deleted it | `cache-lo3-listing-agent{2-start,3-idx,4-idx}.txt`, `b0-runs.txt` | Absent now (check 6). Observation: D3-Q1a's signals step (`d3q1a.py signals`) read the cache's Debug payload files by path while the file existed. Its argv names no `--candidate-cache`, its receipt has no `store` block, and the 351 originals are unchanged since: within the ruling's letter | **holds** |
| GATE-RELCACHE | >= 15 GB free before; the 351 originals unchanged after; listing recorded with its cause | `b0-runs.txt`, `cache-lo3-listing-after-p9-b0g-yf0-{u,w}.txt`, `-agent4-final.txt` | C: 60.0 GB before, 53.0 GB after; re-listing equal (check 6) | **holds** |
| RELCACHE-ALLOW | u / w payloads byte-identical; every differing leaf classed by JSON path; 0 outside; tool's first pass disclosed | `yf0{u,w}-cmp.txt`, `yf0u-cmp-pass1.txt`, `yf0{u,w}-classes.txt`, `tools/CHANGES.txt` | Payload lines "same" (u 3, w 10). TOTAL outside 0 for u and w. Pass 1 is kept, with its 209 OUTSIDE leaves at exactly the M1D-LIST paths. Those leaves are the ones M1d's written list names (`root-wave1-merge-report.md:890-891`; the ruling cites `handoff-root-m1d.md`, root disclosed the citation) | **holds** |
| COMPOSE-1a | ACCEPT (store-backed); tiny world + store deleted | `compose-1a.txt` | As in A step 3; no `p9-gate-compose-*` dir remains | **holds** |
| HOST-6 | Foreign vol sessions untouched | report step 0 host lines | Records only; my process list shows a foreign quote benchmark under `C:\atx-cache` and no build | **holds** (as recorded) |
| FIELDS-GONE | Recorded as a finding; nothing recreated; atx-db not opened | `inputs-check.txt` | 7 missing inputs, all v10 / v13 | **holds** |
| FIELDS-SUB | Witness tool + output committed before the run (`e5a2fe68`); 70 / 70 | `fields-witness.txt`, `tools/fields_witness.py` | "VERDICT: 70 / 70 HOLD"; the two read fields equal | **holds** |
| FIELDS-SUB-A | Path A applied: step-4 row 1, the rulings table, item 1, substitution row 1; "cross-build reference" removed; classification kept | report `:51`, `:62`, `:166`, `:665`, `:716-728`, `:1022`; `r2-classify.txt` | The only "cross-build" left in the report is the row saying it was removed. Classes: C1-COST 9 / FIELDS 4 / DERIVED 14 / ALLOWED 8 / M1D-C1 1, "no OTHER" | **holds** |
| FIELDS-X5 | X-5 rows cut and recorded; M6b items 5 / 6 cited | report steps 3, 7 | Stated in the ruled words | **holds** |
| LAPSE-TABLE | The records agree: fit flag-absent, P9-W LAPSED (0), P9-X pending PRE-30 / PRE30-MVX, the rest run | handoff "What the cells agent must know" | Agrees with ledger :981 | **holds** |
| GATE-3-ORDER | Cache-free items first, then the GATE-IDX check | git times; `b0-runs.txt`; report | Witness committed 14:11:54; list 14:15:10; R-2 14:15:27; X-10 14:18:45; Y-1 14:19:44; GATE-IDX check 14:21:49 | **holds** |
| GATE-MYPY | Every commit by pathspec; 0 cache paths | git | Check 9 | **holds** |
| PRE34-WHO | Nothing run at the gate; path-level diff only | `y1-cmp-v8.txt` | "files byte-identical 2, differing 26"; no figure | **holds** |
| YF0-NAV-ORDER | Row 6 after row 5, reading `p9-b0g-yf0-w`; `--ann-mean-gross --info-clock-print`; `ucrtbase.dll` re-recorded | `b0-runs.txt`, `ucrt-b0-4.txt` | 15:39:01 > 15:37:08; the receipt's token 3 is `p9-b0g-yf0-w/train_combined.json`; check 5 | **holds** |
| GP-TICKS | G-P5 PARTIAL; G-P1 "measured at P9-H" | report G-P table, integration-log `:906-909` | As ruled | **holds** |
| REPIN-UW / REPIN-R2 (superseded) | Executed as `3a1a4759`; `verify` refused twice; stopped; disclosed | `repin-verify-2.txt`, report `:1045-1085` | Both refusals present and quoted | **holds** (as disclosure) |
| REPIN-REV | One revert commit; specs back to `2078f877` bytes; `verify` holds as at `883be9f2`; fixture digest re-recorded; both u / w roles in the handoff; R-2 read by path; amendment 4 | `f8a85ad4`, `repin-verify-3.txt`, `fixture-chain-6.txt`, handoff | Checks 1, 4; A step 6 | **holds** |
| FIELDS-RISK | The standalone REFUSED line is pre-existing; no gate action | `repin-verify{,-3}.txt` | Present in both at the same place | **holds** |
| SEAL-REF | `--seal-allow` on no exe-direct reference argv | `receipt-*.json` | No seal flag in any of the six argvs | **holds** |

## D. The other rulings the dispatch names (cross-cutting)

| ruling | checked | verdict |
|---|---|---|
| GATE-1, GATE-PAR | One gate, one P9-B0, one re-pin of record. Builds and bounded runs ran alone; at most two low-RAM suites at a time (agents 1-2) | holds |
| EXE-FREEZE, EXE-CUR, SPDLOG-RELINK, W1-I2 | No build after p9-3f / p9-3g; the exe receipt source `5f48a25d` differs from HEAD only in specs (no C++ / CMake); `disk = receipt` at the end; W1-I2 (a): the parent NAVs were made by the pinned NAV exe (`repin-verify-3.txt`); `7801c198` read, not edited | holds |
| PRE-22, PRE-29, PRE-30, PRE-34 | R-2 / Y-F0 re-based (NAV; Y-F0 also u / w); Y-1 on the v1.1 store by its pin, no identity claimed; P9-X pending; PRE-34's figure check is the cells root's | holds |
| B2-13 | Three pairs, value and flag | holds |
| M2A-4, SQL3-2, SQL-11 | Catalog once, outside the default path; no `store` block in any receipt; no index flag on any argv (receipts read) | holds |
| CRT-2, OD5-2 | Stores = pins, never rebuilt; the Release named skip stays | holds |
| OD5-1 | Not carried on any P9-B0 reference record | **R-1** |
| ROOT-1, TEST-1, ENV-1 | No red to attribute; ENV-1 0 hits | holds |
| SQL4-S, SQL4-CUTOFF-2 | SQL4 in the head (`06dc9e5a`), checks green, no red in its files; S-1's fixture digest of record reproduced | holds |
| PY-S, STORE-S, IC-S, NAV-S, COVMV-S2 | As in B | holds |
| PERM-1 | Agent 2's one refusal recorded with its exact command; not retried; the owner unstaged | holds |

## Required findings

- **R-1 (step 4; OD5-1).** `root-gate-brief.md:53-54` says "Record per reference: dir, SHA-256 of every output file,
  the exe table, `ucrtbase.dll` (C.6), the identity-bridge flag line (OD5-1)". `r11-dispatch.md:279-280` and
  `r12-dispatch.md:93` repeat it for step 4. The line appears in none of the gate's records: `root-gate-report.md`
  step 4 (`:630-743`, `:802-907`), `handoff-root-gate.md` "Reference dirs of record" (`:71`), `integration-log.md`
  `:858-927`, and the three chain handoffs. Four of the five per-reference record items are present; this one is not,
  so step 4 is half-recorded.
  - What root must redo, records only, as one docs commit: write the OD5-1 (i) line, verbatim from ledger :461, into
    the record of each of the six P9-B0 references (the report's step 4 and the handoff's reference-dir table).
  - Nothing to re-run. No code commit. EXE-FREEZE and the re-pin are untouched.

## Suggested findings

- **S-1 (step 3; the report's step-3 rows).** The brief's step 3 names R-2 among the parents whose flag-absent
  identity is checked. No step-3 row names R-2. Its NAV identity in substance is step 4 row 1 (the FIELDS-SUB
  comparison, FIELDS-SUB-A). Its u / w read fields v10 (`lib-v80.json` fields pin; FIELDS-GONE), and its fit is not a
  P9-B0 item (PRE-22: NAV only). Add a one-line step-3 pointer saying so (see Q-1).
- **S-2 (step 6; `root-gate-report.md:1104-1109`).** `repin-verify-3.txt` ends "tool exit 1", caused by the
  pre-existing standalone REFUSED line (`repin_verify.py:35-36`). The report discloses the line but not the exit
  code; name it beside "holds".
- **S-3 (step 7).** The brief says `scoreboard --timings` is "appended to `integration-log.md`". The table sits in
  `root-gate-report.md:561-568` and `scoreboard-gate.txt`, while the log row (`integration-log.md:850`) only points to
  it. Copy the table, or state where it lives.
- **S-4 (records outside the gate rows).** `integration-log.md:686` (the wave-3 SQL3 merge row) still carries
  `@@S0@@` / `@@S1@@`. This is for IDX-FINAL / the final report; it is not a gate item.
- **S-5 (handoff; for part 2 and the cells).** `handoff-root-gate.md` "What the cells agent must know" says every cell
  carries `--ann-mean-gross` and P9-C the clock flag, "so the paired opt-in leaves are alike on both sides". That is
  true only for P9-C. The parent reference `p9-b0g-yf0-nav` carries the `info_clock` block for every cell. B.0's void
  rule (2) (`p9-prereg.md:579-580` in the working tree at review) requires the flag alike only on P9-C and its
  reference, and ALCLOCK-1 showed that block to be the only difference. The sentence should say exactly that.

## Open questions for the PM (one line each)

- Q-1: Does the brief's step-3 "R-2" mean R-2's NAV only (PRE-22's NAV-only re-base; S-1 is then a statement), or
  u / fit / w too (then a FIELDS-X5-style cut line: its u / w read the absent fields v10)?
- Q-2: R-1's placement: the gate report's per-reference record (the brief's letter), or the prereg's "Round 2b-ii
  record" (PRE B2b), if the PM rules that it suffices?

## Verdict

**RETURN**, step 4, records only.

| step | lane | merge commit | what root must redo |
|---|---|---|---|
| 4 | none (root's own record) | none | Add the OD5-1 identity-bridge flag line to each of the six P9-B0 reference records (R-1); one docs commit; nothing re-run |

All other steps (0-3, 5-9) and every ruling row above hold. Once R-1 is closed, the cells may start (PROCEED). No
BLOCKED attribution exists, the SQL4 verdict is carried green, and the trial ledger is 133 / `27e40f9f`.
