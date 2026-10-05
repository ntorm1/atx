# Handoff: root after M2e (C2 merge, AL-COMB merge, pool-15 -> C3), 2026-10-04

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`.

- **Code head: `e1c16051`** = C2 merge `a25453be` + AL-COMB merge `9d518317` + eol `2ee62800` + SQL2-CLS `e1c16051`.
- **Docs head:** the commit that adds this file (on `5061579d`). Evidence: `m2e-logs/`; report:
  `root-wave2-merge-report.md` section M2e; rows: `integration-log.md` "M2e".

**Status: M2e DONE_WITH_CONCERNS.** Both merges verified from the committed head; no new red; every identity held.
Nothing half-merged (`MERGE_HEAD` absent); no build / test / bounded run of mine is running. The one concern is C2
item (ii) (below).

- **Next free build tag: `p9-2g`** (used: p9-2d equity-dev, p9-2e equity-rel, p9-2f equity-dev).
- **Trial ledger** `build-equity/trials.jsonl`: **133 lines, sha256 prefix `27e40f9f`** (checked at start and end; 0
  trials).
- **Trees:** `build-equity` (Debug) configured at `2ee62800` by p9-2f's reconfigure (ConfiguredProvenance
  `2ee62800...-dirty`); `build-equity-rel` (Release) at `a25453be` (p9-2e), bench OFF. bin now: targets `90cb34a7`
  (p9-2d), ic `c5015605` (p9-2f), target-tests `ecb229ca`, ic-tests `f535167a`, combine-tests `1ef0b46a`,
  impl-tests `6e14bb54`, eval-tests `65f78372`, book-tests `ff3ddfce`; Release: targets `cb2df40d`, target-tests
  `38bf32f6`, impl-tests `b38952e9`.

## Steps of the M2e brief

| step | state |
|---|---|
| 0 trial ledger 133 / `27e40f9f`; housekeeping docs commit | done, `20bc5f5f` |
| MERGE 1 C2 `4d77d3db` | merged `a25453be` (CMake tail keep-both) |
| C2 lane files (`task-C2-rereview-2.md`; the rest came with the merge) | copied, `de6d55ef` |
| C2 Debug p9-2d / Release p9-2e, 0 warnings | done |
| C2 gtests both builds (19 named; target-tests 377; impl `CalibrateGross.*` 7; eval 10; book 13) | all green; **M1c-RED cleared** |
| C2 pytest (`test_research_wave.py` 20; eight wave suites 77) | green |
| C2 (i) gm-pair list committed first (`4d0583cf`), X-5 run | **holds** (L' `1.1720` exact; `construction_reused` true) |
| C2 (ii) delete `wave_rules.py:110-119` (+ C2-9 refusal) | **NOT done: left to C3** (reason below) |
| C2 (iii) S13 | at E2's merge |
| C2 (iv) no P9 reference under `rel` | holds (receipt scan) |
| C2 (v) SQL2-CLS + fixture chain | no literal, no commit; chain `d32f7655...4e69` |
| MERGE 2 AL-COMB `efb7ab2e` | merged `9d518317` (CMake tail keep-both; `exe_capabilities` 8 sorted) |
| AL-COMB lane files (`task-AL-COMB-rereview-1.md`, `evidence-AL-COMB/`) | copied, `5061579d` |
| AL-COMB eol=lf for template pins | `2ee62800` (E2's R0-7-EOL lines verbatim); pins = disk = blob |
| AL-COMB build p9-2f, gtests, `--list-rules` = fixture, pytest 133 / 142 / 11 | green |
| AL-COMB SQL2-CLS (`atx.p9-rule-registration/v1`, `atx.p9-diagnostic-spec/v1`) | `e1c16051`; store 43 x2; chain `d32f7655...4e69` |
| AL-COMB W1-MIN M-2 theme seam | holds |
| AL-COMB flag-absent X-5 w and fitter identity (lists `789d7e91`, `60bd542b` first) | both hold |
| DISK-2 pool-15 / pool-19 | nothing to delete (lanes self-cleaned); mine 212 MB deleted |
| pool-15 -> C3 (W3-11) | released `p9-c1-20261003`, leased `p9-c3-20261004` at `e1c16051`, keeper alive |
| report sections, log rows, this handoff | done |

## Concern: C2 item (ii) is C3's

`matched_leverage` / `gross_matches` (`wave_rules.py:113-122` now) are the whole Python `pm6-6` match mode:
`wave_stage_cell.match` (`:241`, `:245`, `:262`; also the `gross_match: none` path), `FakeCycle.calibrate`
(`wave_fixture.py:343`), the fixture's default manifest `"gross_match": "pm6-6"` (`wave_fixture.py:180`), the direct
asserts `test_research_wave.py:77-84`, the engine-vs-Python test `:236-288`, the G-P5 row
`test_no_python_mirror.py:75-76`. Deleting the lines alone turns those suites red; retiring the mode is a designed
change in files lane E2 is rewriting (Ruling C2-10's principle). `wave3-carry.md:221` already lists the deletion for C3
"(root, if still present)"; C2-9's plan-time refusal and S14 go with it. The gm-pair identity it waited for now holds.

## Known-red set now

- pytest `scripts/tests/test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` (E2).
- Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (M2c-RED-2 / OD5-2; a visible Release skip once
  S2R's test split merges).
- (Release `BookNormalScore.*` x2: **cleared** by C2 at p9-2e.)

## Next root agent

1. Read `resume4-dispatch.md`, this file, the ledger from RESUME 5 on.
2. M2f as the PM dispatches (B2 identity part + TRAIN identity per the ledger).
3. At **E2's merge**: MR-4 (`COMPOSITION_FLAGS` += `"theme-cov-hedge-v1": {"fit": {"--theme-hedge":
   "theme-cov-hedge-v1"}}`, `"mom-volman-v1": {"fit": {"--mom-volman": "mom-volman-v1"}}`) in the same commit, then
   E2's `scripts/tests` on root's fixture; S13 (align the engine branch's exe-notes line in `wave_stage_cell.py`
   `run_stage` with E2's wording, per `task-C2-rereview-2.md` item 4); reconcile C2's `wave_result.py` edit and R4's
   two lines in `wave_stage_record.py` with E2's version; the positive live-table check (frozen `D1_SCHEMAS` = the
   merged exe's `--list-rules --json`). `.gitattributes` merges clean (root carries E2's three lines verbatim).
4. Wave-2 gate (after the last merge): as `root-wave2-merge-brief.md` "After the last merge"; plus AL-COMB's Q5
   zero-trial diagnostic before P9-H is registered; D-COV / COV-7.
5. C3's brief (PM): include C2 item (ii) + C2-9 + S14 (above).
6. pool-19 for AL-CLOCK (W3-11) when dispatched: `-Release pool-19 -RunId p9-t1-20261003` (record still T1's, tree
   clean at `098e6ccd`), then a new lease as in `pools.md` "Wave-3 re-lease".

## Pool states

| pool | holder | state |
|---|---|---|
| pool-2 | root (`feat/platform-p9-20261003`) | clean but for the PM / AL-DATA files below |
| pool-15 | **C3**, `feat/p9-c3-20261004` at `e1c16051`, run id `p9-c3-20261004`, heartbeat `p9-c3-hb` | leased, owner alive (keeper 24052); ignored incomplete `build/` (590 KB) from the script's `dev` configure |
| pool-19 | wave-1 T1 record (`p9-t1-20261003`, owner dead), branch `feat/p9-alcomb-20261003` at `098e6ccd` | clean; AL-COMB merged; free to re-lease |

## Uncommitted state at the end

- `progress.md` is committed as it stood (LEDGER-1) in the final docs commit.
- `al-data/{atx-db-request,builder-designs,task-AL-DATA-report,task-AL-DATA-review}.md`: modified by the AL-DATA
  doc agent while M2e ran (its round 2 was live); not committed by me after the housekeeping commit (a live agent's
  half-written files). The next root's housekeeping commit takes them as they stand.
- Four untracked `docs/plans/*.png` and `disk-audit.md`: not mine, left alone.

## Kept outputs

`build-equity/p9-m2e-c2-x5-engine` (11 MB, the first engine-calibrated X-5 NAV) and the run receipts of every M2e run
(`p9-m2e-*-run/`); receipts `mega-p9-2{d,e,f}-*`. M2d's kept outputs unchanged.
