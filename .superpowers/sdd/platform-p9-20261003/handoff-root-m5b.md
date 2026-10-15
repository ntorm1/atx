# Handoff: root after M5 and M5c (RESUME 11: B2 fix 2, OD3P follow-up, D3W-PB, SIGN, A4 pre-flip; A4 flips P10), 2026-10-15

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Started from `handoff-root-m5.md` (the STOP 10 stub, which
stays as that record) and `handoff-root-m4.md`. Rows: `integration-log.md` "M5 (RESUME 11)"; detail:
`root-wave3-merge-report.md` "Step group M5 (RESUME 11)" and sections "B2 fix 2", "OD3P follow-up", "D3W-PB", "SIGN",
"A4"; evidence `m5-logs/` (tools `m5-logs/tools/`).

- **Status: DONE** (after M5c). Four lanes merged whole at their MERGE-SHAs (B2 fix 2, OD3P follow-up, D3W-PB,
  SIGN). **A4 is merged at its pre-flip head `6dab6948` only; its flips are P10 (ruling A4-P10-FLIPS).** M5 stopped A4
  at the lane boundary because A4-Q2's TRAIN identity needs the holdings stages under `C:/atx/atx-db/data/alpha_panel/v1/`;
  the PM ruled A4-DB (NO: `C:/atx/atx-db/` is never opened) and dispatched root M5c (the same agent, continued) to look
  READ-ONLY for a copy outside atx-db. None exists (section "A4 (M5c)" below), so the flips `69141218`, `59d807ea` and
  `fc3e414e` are P10 whole; nothing reverted. No unattributed red, no identity miss.
- **Code head: `bba4c06f`.** Commits of the group, in order: step 0 `6cb1070f` (docs); B2 merge `6d47b28d` (at
  `101f695a`) + slip `138e1d2c` (B2-S3 S-1 docstring); OD3P merge `a2cf7e78` (at `4a7372e2`) + slip `b991c755`
  (OD3P-S2 S-4, `classes.json`); D3W-PB merges `6088bfe9` (`c044f5f6`), `175edd61` (`460f5ba2`), `22caee08`
  (`44a331e6`, the S-g conflict); SIGN merge `ad913bd5` (`c2010ed1`, two CMake-tail conflicts) + SQL2-CLS `1dd9b9b1`
  (`atx.sign-receipt/v1` in `ic-output`) + `338e6a7c` (`50a43599`) + `cdc753a8` (`e2844926`); A4 merge `bba4c06f`
  (`6dab6948`).
- **Docs head:** M5's records commit `0a5d0716` added this file. M5c's docs commit (the one carrying this revision)
  updates it, the A4 section of the merge report and the integration log, adds `m5-logs/a4-provenance.txt`,
  `a4-copy-search.txt` and `tools/find_inputs.py`, and commits `progress.md` exactly as the PM left it (no ledger line
  edited by root). M5c changed no code, built nothing and ran no test: the code head stays `bba4c06f`.
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
  (M5)"; in M5c the two fields' manifest entries printed their descriptive `coverage` blocks, not recorded: merge report
  A4 "M5c" item 5). Host: no other build of `C:\atx-wt\` at any admission; free RAM 1.5-3.8 GB.

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

- **A4 (M5c; rulings A4-DB, A4-P10-FLIPS): identity not run: inputs under atx-db; flips P10.** In root = the pre-flip
  head `6dab6948` (merge `bba4c06f`): the engine builders, their kinds, tests and fixture are in; flag absent, both
  registry rows `kind python`, so Python stays the producer of record for `ftd_shares_ratio21` and
  `regsho_threshold_days63` and no byte of Y-F0 moves. P10 whole = the flips `69141218` (ftd), `59d807ea` (regsho) and
  the registry rows of `fc3e414e`; nothing reverted.
  - Provenance found (Y-F0's fields manifest `train-2020-2023-lo3-fields-v15/manifest.json` `26fee5ce`, the two fields'
    `sources` records; `m5-logs/a4-provenance.txt`): `ftd_shares_ratio21` = the ftd stage `manifest.json` (`a76d69be...`)
    and `year=2018 .. year=2023/ftd.parquet` (six files, each with its SHA-256) under
    `C:\atx\atx-db\data\alpha_panel\v1\ftd\`, plus `shares_out.f64` (`1cff53f8...`, from
    `build-equity/train-2020-2023-lo3-fields-v9`, in pool-2); `regsho_threshold_days63` = the regsho stage
    `manifest.json` (`fb073c62...`), `lists.parquet` (`4ebd1881...`, not year-partitioned) and
    `year=2018 .. year=2023/threshold.parquet` under `C:\atx\atx-db\data\alpha_panel\v1\regsho_threshold\`, plus the
    security master `manifest.json` (`3afe0660...`) and `finra_names.parquet` (`1acfed75...`, not year-partitioned)
    under `C:\atx\atx-db\data\alpha_panel\v1\security_master\`. Both fields were reused into v15 from `-v15a` with the
    same stage pins (`reused_from.inputs`).
  - Where root looked (names and sizes only, no file opened; `m5-logs/a4-copy-search.txt`, lister
    `m5-logs/tools/find_inputs.py`): the whole pool-2 tree incl. `build-equity/` (only hits: the A4 synthetic fixture
    `atx-engine/tests/fixtures/research_fields_holdings/{ftd,regsho_threshold,security_master}`, sizes 4153 / 7365 /
    2660 / 1853 bytes, not copies of the recorded 14-18 MB / 173,100 / 86,333 / 1,410,313-byte inputs); `C:/atx/data`
    (0 hits); `data-classification.md` (no ftd / regsho / threshold / security-master / alpha_panel entry); the lane pools
    13, 14, 15, 20, 21, 22, 23 (root only, not its `build-equity/`, TEETH-BIN-23), 27-32 by `ls` of the pool root and its
    `build-equity/` top level (0 hits). Skipped: pools 8 / 24 (frozen), 25 / 26 (other sessions). No SHA-matched copy
    exists outside atx-db, so nothing was hashed and no identity ran.
  - Build: none in M5c; tag `p9-3d` still free; the exe table above is unchanged. The two non-partitioned files (`lists.parquet`,
    `finra_names.parquet`) could not be opened even if a copy existed (their rows may extend past 2023-12-31).
  - A4 fix 1 (ruling A4-FIX1-CHAIN): with the flips in P10, the lane re-applies its tests-only commits on a new branch
    from `6dab6948` (plain cherry-pick by SHA) and the scoped re-review reviews that head (BASE `6dab6948`); else the fix
    round is P10 with the flips. The PM decides at the lane's hand-back; not root's.
- **M6 merges as dispatched:** AL-CLOCK fix head (the ledger has `Task AL-CLOCK: APPROVE at eed512bd`), SQL3 item 5
  (iff review 2 APPROVE; Python), the A4 fix head only in its A4-FIX1-CHAIN form (re-applied on `6dab6948`, iff its
  re-review is APPROVE), SQL4 (iff APPROVE; LAST). The NAV exe and target-tests are stale by source: the AL-CLOCK
  merge's ROOT-1a build rebuilds them (`atx-impl-strategy-target-tests` + the NAV exe), tag `p9-3d`.
- **G-P5 at the freeze:** 0 rows of P9 lanes left (`m5-logs/sign-mirror-freeze-post.txt`); root can set `P9_FREEZE =
  True` at the freeze without a relabel (the C3B-S relabel is moot). `FROZEN_PAIRS` still names `sign_pm7_35`.
- **OD-3 group:** `p9-od3-runbook.md:207` still says "[UNKNOWN: root's merge of the OD3P follow-up]": that merge is
  `a2cf7e78`. The `mv-aim-v1` removal / re-pin of `flagged_members.json` stays P0's (MV-FLAG-P0).
- **Gate items carried (TEST-1)**, added by M5 to `handoff-root-m4.md`'s list: the n37 test on the Release verb (a
  difference beyond 1e-12 is a red, B2 item 4); whole `atx-impl-strategy-ic-tests` (SIGN, D3W-PB), whole
  `atx-engine-research-fields-tests` and `-composition-tests`; `scripts/tests` seeds 0 / 1 (`prepare_research_fields_engine.py`,
  `wave_rules.py`, `wave_stage_library.py`, `nav_summ.py` changed) and the stage chain; every Release build.
- **PRE (round 2b-ii):** SIGN-SUBST is now unconditional (SIGN merged: the sign receipt is a per-wave file the judge
  pins, B.0 "Manifest"); the A4-Q2 entry reads "both fields are in Y-F0's manifest; identity not run: inputs under
  atx-db; flips P10" (A4-P10-FLIPS; Python stays their producer of record); D3W-PB adds nothing for PRE.
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

- None open. M5's two questions were ruled: A4-DB (no atx-db read; then A4-P10-FLIPS, applied in M5c) and
  A4-FIX1-CHAIN (the PM's, at the A4 fix-1 lane's hand-back). The three of `handoff-root-m4.md` were ruled earlier
  (COMPOSE-1, MV-FLAG-P0, C3B-DOC).
