# Handoff: root after M5 (RESUME 11: B2 fix 2, OD3P follow-up, D3W-PB, SIGN, A4 pre-flip), 2026-10-15

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Started from `handoff-root-m5.md` (the STOP 10 stub, which
stays as that record) and `handoff-root-m4.md`. Rows: `integration-log.md` "M5 (RESUME 11)"; detail:
`root-wave3-merge-report.md` "Step group M5 (RESUME 11)" and sections "B2 fix 2", "OD3P follow-up", "D3W-PB", "SIGN",
"A4"; evidence `m5-logs/` (tools `m5-logs/tools/`).

- **Status: DONE_WITH_CONCERNS.** Four lanes merged whole at their MERGE-SHAs (B2 fix 2, OD3P follow-up, D3W-PB,
  SIGN). **A4 stopped at a lane boundary after its pre-flip head**: A4-Q2's TRAIN identity needs the holdings stages
  under `C:/atx/atx-db/data/alpha_panel/v1/` (owner rule: never touch `atx-db/`), so it was not run and the flips and
  `fc3e414e` were not merged (BLOCKED for the flips; the merge of the pre-flip head is not reverted and moves no byte of
  a P9 run). No unattributed red, no identity miss.
- **Code head: `bba4c06f`.** Commits of the group, in order: step 0 `6cb1070f` (docs); B2 merge `6d47b28d` (at
  `101f695a`) + slip `138e1d2c` (B2-S3 S-1 docstring); OD3P merge `a2cf7e78` (at `4a7372e2`) + slip `b991c755`
  (OD3P-S2 S-4, `classes.json`); D3W-PB merges `6088bfe9` (`c044f5f6`), `175edd61` (`460f5ba2`), `22caee08`
  (`44a331e6`, the S-g conflict); SIGN merge `ad913bd5` (`c2010ed1`, two CMake-tail conflicts) + SQL2-CLS `1dd9b9b1`
  (`atx.sign-receipt/v1` in `ic-output`) + `338e6a7c` (`50a43599`) + `cdc753a8` (`e2844926`); A4 merge `bba4c06f`
  (`6dab6948`).
- **Docs head:** the commit that adds this file. It also commits `progress.md` exactly as the PM left it in the working
  tree (no ledger line edited by root), the merge report, the integration log and `m5-logs/`.
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at start and end (0 trials).
- **SQL3-2** holds: `build-equity/research-store/` absent at start and end.
- **Build tags used: `p9-2z`, `p9-3a`, `p9-3b`, `p9-3c`** (all Debug `equity-dev`, exit 0, 0 warning lines; after `p9-2z`
  the series continues `p9-3a` as `p9-1z` -> `p9-2a` did). **Next free root tag: `p9-3d`.**
- **Known-red set: EMPTY** (B2 fix 2 cleared `atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced`:
  10 passed on root's verb p9-2z with root's inputs and the blind flags). Release
  `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` stays a named skip. Named skip seen in M5:
  `test_wave_kinds.py:1406` (needs `ATX_EQUITY_BIN` naming a bin with both exes; as in M4).
- No lease touched. No permission refusal. No data dated 2024 or later opened; no file under `C:/atx/atx-db` opened;
  `holdout_gate.py` not run; no statistic recorded (one disclosed grep incident, see the merge report "Disk and status
  (M5)"). Host: no other build of `C:\atx-wt\` at any admission; free RAM 1.5-3.8 GB.

## Exes (EXE-CUR)

Debug `build-equity/bin`; on-disk SHA-256 = the newest receipt's for every exe at the end of the group
(`m5-logs/exe-sha-end.txt`; SPDLOG-RELINK: re-hash against these receipts before using one as evidence).

| tag | source | receipt | exe | SHA-256 | current by source at `bba4c06f` |
|---|---|---|---|---|---|
| **p9-2z** | `138e1d2c` | `build-equity/mega-p9-2z-receipt.json` (copy in `m5-logs/`) | atx-research-eval | `4791019131c0d74603b83cb772ef027ec8641167fd95844c4674e75bdf39893b` | yes |
| **p9-3a** | `6088bfe9` | `build-equity/mega-p9-3a-receipt.json` | atx-engine-research-composition-tests | `76b664b7483a77a717015a3fdebb38f97dc39fbcfecd04a98e52d3cd016023a9` | yes |
| **p9-3a** | `6088bfe9` | same | atx-research-composition | `212f8b9941f0be705ebac138c03e83279fc863ab12051f00dd09501db7eed98e` | yes |
| **p9-3a** | `6088bfe9` | same | atx-research-admission | `411af33a0def5a6c28983dbf1ffc2fbc324694e8ac2e6b24401b27b693c9fa26` (bytes = p9-2v) | yes |
| p9-3a | `6088bfe9` | same | atx-impl-strategy-ic-tests | `b43d7a3ebc082f9d97df4055a09dbf8c502b4cb1202d2b113f05d88b376b97ca` (overwritten by p9-3b) | - |
| **p9-3b** | `1dd9b9b1` | `build-equity/mega-p9-3b-receipt.json` | atx-equity-strategy-ic (IC exe) | `ef3974cfa82d1cc86d6a8079394e5d0964e8abcea8fd36a068b12f8b8bc99b38` | yes |
| **p9-3b** | `1dd9b9b1` | same | atx-impl-strategy-ic-tests | `9cd667c75c08a9197f4267c7dc1e407630beb84f9163739227e73f8d28eedf56` | yes |
| **p9-3c** | `bba4c06f` | `build-equity/mega-p9-3c-receipt.json` | atx-engine-research-fields-tests | `39be2b3d995b11873b5d667c04419b5b063f1f2f56729bce1816eed1b30febdf` | yes |
| **p9-3c** | `bba4c06f` | same | atx-research-fields | `d14346e836923cc8dea8ebd05b9769f0449b44e0f9f9150866f31d5a2e875cde` | yes |
| p9-2y | `5c811b56` | `build-equity/mega-p9-2y-receipt.json` | atx-engine-book-tests | `6977a5a75b386d283c81e148c0fec31a2da1f95536dc98cab2366d424d1f8360` | yes (`atx::engine` unchanged in M5) |
| p9-2y | `5c811b56` | same | atx-equity-strategy-targets (NAV) | `e6b21410987a8946dcd9a5d0b5631a15fee76d682d2be987c46f1d05be4bd219` | **no**: links `atx-impl-strategy` (D3W-PB, SIGN) |
| p9-2y | `5c811b56` | same | atx-impl-strategy-target-tests | `ee69e2fb06db6702b78a9e8cbb68a9a13ca25ccc5fa578ada0c3d90b9afd8b8e` | **no** (same) |
| p9-2r | `fd084e78` | `build-equity/mega-p9-2r-receipt.json` | atx-research-store / catalog tests | `d8a500d8...` / `f1134aaa...` | yes |
| p9-2v | `08f04e07` | `build-equity/mega-p9-2v-receipt.json` | atx-engine-research-eval-tests | `dd8a3103...` | **no** (`atx::engine` gained `mv_target.cpp` in M4) |

**Stale by source after M5** (rebuilt by the first step that needs them, EXE-CUR; the gate's builds otherwise): the
Debug NAV exe and target-tests, the eval tests, `atx-impl-tests`, every other Debug exe linking `atx-impl-strategy`
(`atx-equity-strategy`, `-risk`, `-mine`, `atx-impl`) and every Release exe (p9-2q / p9-2s).

**EXE-FREEZE:** M5 relinked the eval verb (p9-2z), `atx-research-composition` (p9-3a), the IC exe (p9-3b) and the fields
exe (p9-3c), all before the wave-3 P9-B0 re-pin; SIGN makes every `atx-impl-strategy` exe relink at the gate's builds.
Any later relink of the NAV, IC or eval exe after the re-pin redoes P9-B0 and the re-pin first.

## What the next root group (M6) must know

- **A4 (BLOCKED for the flips):** in root = the pre-flip head `6dab6948` only (merge `bba4c06f`); NOT in root = the
  flips `69141218` (ftd), `59d807ea` (regsho) and `fc3e414e`. Before they can merge, the PM / owner must rule whether
  A4-Q2's TRAIN identity may read `C:/atx/atx-db/data/alpha_panel/v1/{ftd,regsho_threshold,security_master}` (and the
  recipe's other atx-db inputs: identity bridge, SEC stages, 13F, short volume, SIC events), or move the flips to P10.
  If allowed: the argv of `task-A4-report.md` "For root" item 3 from the recipe in
  `build-equity/train-2020-2023-lo3-fields-v15-run/receipt.json` (paths and pins only; `--reuse` of `-v15`), with the
  p9-3c fields exe (current) through the bounded runner (`--seconds` <= 600, `--max-rss-mib 2048`, SEAL-ALLOW-3 by source
  check), pass = `<field>.f64` SHA-256 = the manifest's pin (`15bc4e6a...`, `8248b3a2...`), the entry minus `producer`
  equal, the seal / read counts of `engine_fields/<field>.receipt.json` = the manifest's `source_checks.holdings` blocks;
  then merge `69141218`, `59d807ea`, then `fc3e414e` (or the A4 fix-1 head if `Task A4: APPROVE` names it by then; that
  head sits above `fc3e414e`). Detail: `m5-logs/a4-q2-train-identity.txt`.
- **M6 merges as dispatched:** AL-CLOCK fix head (the ledger now has `Task AL-CLOCK: APPROVE at eed512bd`), SQL3 item 5
  (iff review 2 APPROVE; Python), the A4 fix head (iff APPROVE; tests only; needs the A4 flips question settled first,
  since it sits above `fc3e414e`), SQL4 (iff APPROVE; LAST). The NAV exe and target-tests are stale by source: the
  AL-CLOCK merge's ROOT-1a build rebuilds them (`atx-impl-strategy-target-tests` + the NAV exe), tag `p9-3d`.
- **G-P5 at the freeze:** 0 rows of P9 lanes left (`m5-logs/sign-mirror-freeze-post.txt`); root can set `P9_FREEZE =
  True` at the freeze without a relabel (the C3B-S relabel is moot). `FROZEN_PAIRS` still names `sign_pm7_35`.
- **OD-3 group:** `p9-od3-runbook.md:207` still says "[UNKNOWN: root's merge of the OD3P follow-up]": that merge is
  `a2cf7e78`. The `mv-aim-v1` removal / re-pin of `flagged_members.json` stays P0's (MV-FLAG-P0).
- **Gate items carried (TEST-1)**, added by M5 to `handoff-root-m4.md`'s list: the n37 test on the Release verb (a
  difference beyond 1e-12 is a red, B2 item 4); whole `atx-impl-strategy-ic-tests` (SIGN, D3W-PB), whole
  `atx-engine-research-fields-tests` and `-composition-tests`; `scripts/tests` seeds 0 / 1 (`prepare_research_fields_engine.py`,
  `wave_rules.py`, `wave_stage_library.py`, `nav_summ.py` changed) and the stage chain; every Release build.
- **PRE (round 2b-ii):** SIGN-SUBST is now unconditional (SIGN merged: the sign receipt is a per-wave file the judge
  pins, B.0 "Manifest"); the A4-Q2 case (both fields in Y-F0's manifest) with "identity not run in M5; flips not in root"
  unless a later group runs it; D3W-PB adds nothing for PRE.
- **Tools of this group** (`m5-logs/tools/`): as `handoff-root-m4.md` plus `mirror_freeze.py [SYMBOL ...]` (the G-P5
  freeze state without editing the guard), `resolve_concat.py FILE...` (conflict regions keep-both, root's lines
  first), `fixture_chain.sh SCRATCH TAG` (copied from `m3-logs/tools`). Interpreter `C:/Program Files/Python312/python.exe`;
  create the `--basetemp` parent dir first; long records with the editor tool.

## Kept and deleted outputs

- Kept: root's object trees `build-equity/` and `build-equity-rel/` (the gate's Debug and Release builds use them; the
  current Debug exes in the table above are the gate's inputs until rebuilt); the receipts and build / cache logs of
  p9-2z, p9-3a, p9-3b, p9-3c in `build-equity/` (copies in `m5-logs/`); the M4 probe worlds `build-equity/p9-m4-tw-*`.
- Created nothing under `build-equity/` except the four receipts and their logs (the identity runs of this group wrote
  only into the session scratchpad).
- Deleted: the session scratchpad `root-m5/` (basetemps, the D3W-PB identity's BASE archive and outputs, the SIGN
  identity's rows / receipts, the fixture-chain catalogs), by literal name, after the records commit.
- Lane pools: nothing deleted; pool-23's `build-equity/` untouched (TEETH-BIN-23).

## Uncommitted state at the end

- None of root's. Five untracked `docs/plans/*.png` (not root's) left. Any ledger lines the PM adds after this commit.

## Permission refusals

- None.

## Open questions for the PM (one line each)

1. A4-Q2: may root's TRAIN identity read the holdings stages under `C:/atx/atx-db/data/alpha_panel/v1/` (owner rule "never touch atx-db/"), or do the A4 flips go to P10?
2. The A4 fix-1 head sits above `fc3e414e` (which contains the flips): if the flips stay out, is the fix round merged at M6 by its test-only commits alone, or held with the flips?
3. The three questions of `handoff-root-m4.md` were ruled (COMPOSE-1, MV-FLAG-P0, C3B-DOC); M5 adds only the two above.
