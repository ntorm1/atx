# Handoff: root after M2f (S2R merge, B2 identity part + TRAIN identity), 2026-10-04

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`.

- **Code head: `42f42a07`** = S2R merge `0f7a84b1` + B2 identity-part merge `c2416d62` + SQL2-CLS `ba5e6e5d` + canary
  `42f42a07`.
- **Docs head:** the commit that adds this file (on `42f42a07`; earlier M2f docs commits `5e73a9cb`, `ea52989a`). Evidence: `m2f-logs/`; report:
  `root-wave2-merge-report.md` section M2f; rows: `integration-log.md` "M2f".

**Status: M2f DONE.** Both merges verified from the committed heads; no new red; the TRAIN identity holds with 0 B2-7
boundary lines. Nothing half-merged (`MERGE_HEAD` absent); no build / test / bounded run of mine is running.

- **Next free build tag: `p9-2k`** (used: p9-2g equity-dev, p9-2h equity-rel, p9-2i equity-dev, p9-2j equity-rel).
- **Trial ledger** `build-equity/trials.jsonl`: **133 lines, sha256 prefix `27e40f9f`** (checked at start and end; 0
  trials).
- **Trees:** `build-equity` (Debug) configured at `c2416d62` (p9-2i's reconfigure); `build-equity-rel` (Release)
  configured at `c2416d62` (p9-2j), bench OFF. Debug bin now: alpha-tests `8b767814` (p9-2g), ic-tests `f535167a`,
  research-ledger-tests `8c5a6065`, research-eval-tests `25c14b28`, `atx-research-eval` `bd8b5e08`, impl-tests
  `237e5826`, IC `6c2e2e2e` (p9-2i; relinked with the ledger library), targets `90cb34a7` (p9-2d). Release bin:
  alpha-tests `875ed9c7`, ic-tests `1e070302` (p9-2h), research-ledger-tests `bfe2afc3`, research-eval-tests
  `e0a51398`, `atx-research-eval` `a57a2090`, impl-tests `6a1eb722`, IC `fccaa07e` (p9-2j), targets `cb2df40d`
  (p9-2e).
- **UCRT** (M2c-RED-2 (d) record): `ucrtbase.dll` 10.0.26100.7623, SHA-256 `2c08e77f...65ef` (= S2R's measurement).

## Steps of the M2f brief

| step | state |
|---|---|
| 0 trial ledger 133 / `27e40f9f` at start and end | done |
| MERGE 1 S2R `048b01c6` | merged `0f7a84b1`, no conflict |
| S2R lane files (`task-S2R-review.md` from `d92a20ee`; report, handoff, `s2r-logs/` came with the merge) | copied, `5e73a9cb` |
| S2R Debug p9-2g / Release p9-2h, 0 warnings | done |
| S2R gtests: Debug alpha 783 / 0 / 0, Release 780 / 2 skip / 0 (the two named skips, M2c-RED-2 message), S2 filter 28 / 28, IC whole 226 / 226 (204 + AL-COMB's 22), both builds | reproduced |
| no golden digest changed (hex constants pre / post; only the canonical NaN added) | holds |
| S-3 slip (`hf_crowd` has a DSL string) | fixed in `task-S2R-report.md` 12.3, `5e73a9cb` |
| S2R-1 scan on the merged tree | 5 `stddev` book candidates, 0 P9 strings |
| MERGE 2 B2 identity part `1b25b9a4` (not `598d2199` / `8fb17ad6` / `26fe8d50`) | merged `c2416d62` (three CMake tails, keep-both) |
| B2 lane files (report, review, re-review 1, handoff, `b2-evidence/` from `4bed5c43` blobs) | copied, final docs commit |
| B2 Debug p9-2i / Release p9-2j, 0 warnings | done |
| B2 gtests eval 24, ledger 16, impl `TrialLedger*` 24, both builds | green |
| B2 pytests at the identity point 109 (+1 root-only skip) + 17, Debug and Release verb | green; the root-only `test_legacy_n37_numbers_reproduced` 1p |
| (a) rho of X-5 / Y-F0, logged FIRST (`ea52989a`) | X-5 vs X-3 0.9202746809672834 (1 - rho 0.0797); Y-F0 vs X-5 0.9556041703496023 (0.0444) |
| (b) expected list committed first (`ea52989a`), TRAIN identity (P / Release verb / Debug verb, bounded 2,048 MiB) | **HOLDS**, 0 B2-7 lines |
| (c) `atx-research-eval` into `$canaryExes` | `42f42a07` |
| (d) SQL2-CLS (both code literals + both fixture literals) and fixture chain | `ba5e6e5d`; chain `d32f7655...4e69` |
| re-review check 8 (ledger `prev_sha256: null` count) | 0 |
| DISK-2 pool-18 | nothing to delete (lane self-cleaned); pool-14 untouched; no lease released |
| report sections, log rows, this handoff | done |

## Known-red set now

- pytest `scripts/tests/test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` (E2).
- Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` is no longer a red: a **named Release skip, 0
  failed** (M2c-RED-2 (c) / OD5-2); the wave-2 gate report states it.

## Items for the E2 merge (next root step group that merges E2)

1. **MR-4** in the same commit: `COMPOSITION_FLAGS` += `"theme-cov-hedge-v1": {"fit": {"--theme-hedge":
   "theme-cov-hedge-v1"}}`, `"mom-volman-v1": {"fit": {"--mom-volman": "mom-volman-v1"}}`; then E2's `scripts/tests`
   on root's fixture.
2. **Positive live-table check:** E2's frozen `D1_SCHEMAS` = the merged IC exe's `--list-rules --json` (build the IC
   exe at the E2 merge; Debug bin's IC is p9-2i `6c2e2e2e`).
3. **C2's S13:** align the engine branch's exe-notes line in `wave_stage_cell.py` `run_stage` with E2's wording
   (`task-C2-rereview-2.md` item 4).
4. **`scripts/wave_stage_record.py`:** reconcile with E2's version (a) C2's R4 two lines (`record_spec_digest` pinned in
   `verify_inputs`) and (b) NEW from M2f: B2's ledger-head writer in `record()` (`LEDGER_HEAD`, `ledger_head_doc`, the
   head copy and the log line; schema `atx.ledger-head/v1`, now a class row); re-run the record-stage tests. Also C2's
   `wave_result.py` edit.
5. `.gitattributes` merges clean (root carries E2's three R0-7-EOL lines verbatim).

## After E2: B2 post-identity part (Ruling B2-12)

The lane (pool-14) merges root's head (with E2), redoes the deletion, B2-11 and re-review R1 / R2 on E2's split, then a
scoped re-review; root merges it then: re-review checks 3 (`test_cycle_scoring.py::test_the_summ_runs_the_named_verb_and_the_verdict_refuses_another`
with root's build) and 5 (hunks on E2's split); the canary from then on needs the verb (already in `$canaryExes`).
SQL3 waits for that merge (W3-12).

## Wave-2 gate carries from M2f

- S2R-DIAG (S2R-2): zero-trial count for `smax` / `smax5` (AuditExact NaN, ResearchFast finite), if under ~15 min.
- Optional S-5: Debug vs Release `DISABLED_PerRowDigestDump` diff; expect exactly rows 05 / 06 / 83.
- UCRT record at each P9-B0 and before each cell (M2c-RED-2 (d)); today's value above.

## Pool states (unchanged by M2f)

| pool | holder | state |
|---|---|---|
| pool-2 | root (`feat/platform-p9-20261003`) | clean but for the four untracked PNGs (not mine) |
| pool-14 | lane B2 (`feat/p9-b2-20261003` at `4bed5c43`; lease record still B1's `p9-b1-20261003`, owner dead) | not touched |
| pool-15 | C3 (`feat/p9-c3-20261004`, run id `p9-c3-20261004`, keeper 24052 alive) | not touched |
| pool-18 | S2R done (`feat/p9-s2-20261003` at `d92a20ee`; lease record S1's `p9-s1-20261003`, owner dead) | clean; S2R merged; no object tree left (0.5 MB lease `build/` stub, 3.2 MB receipts) |
| pool-19 | T1's record (`p9-t1-20261003`, owner dead), branch `feat/p9-alcomb-20261003` | free for AL-CLOCK (W3-11) |

## Uncommitted state at the end

- `progress.md` and the four `al-data/*.md` committed as they stood (LEDGER-1; `al-data/` was still being edited by
  the AL-DATA fix-round-3 agent, so the snapshot may be mid-edit; the next root's docs commit takes the rest).
- Four untracked `docs/plans/*.png`: not mine, left alone.

## Kept outputs

`build-equity/p9-m2f-b2-identity/` (the six identity JSONs, 52 KB) and the six run dirs
`build-equity/p9-m2f-b2-{x5,yf0}-{p,erel,edbg}-run` (receipts copied to `m2f-logs/`); receipts `mega-p9-2{g,h,i,j}-*`.
M2e's kept outputs unchanged.

## Open notes for the PM (not blocking)

- rho was measured only for the ruled pairs; a leverage-only P9 child (X-10 vs Y-F0, Y-1 vs X-10) is the pair most
  likely to approach the B2-R1 (c) 1e-6 bound: worth measuring at PRE before registration.
- nav_summ's `WARNING CSV tau mean != summary` appears for every NAV dir checked (ledgered v8 cells included):
  long-standing, unrelated to B2.
