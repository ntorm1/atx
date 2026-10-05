# Handoff: root after M2j (C3 part A merge, D-COV-EIG), stopped by the OWNER STOP, 2026-10-05

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Rows: `integration-log.md` "M2j"; detail and every pre-run
list: `root-wave2-merge-report.md` section "M2j" (lists committed in `9c7aa654` before any run) and
`root-wave2-gate-report.md` items 4 (d) / 4 (e) (D-COV-EIG; amendment 1 `438f0481` before attempt 2); evidence
`m2j-logs/` (tools `m2j-logs/tools/`).

- **Code head: `327c86b2`** = `5c57c1ef` (docs; code `c8ddf736`) + `327c86b2` (merge of C3 part A `0dbfa459`, by SHA;
  0 conflicts). No slip commit to code; **no SQL2-CLS commit yet**.
- **Docs head:** the commit that adds this file (earlier M2j docs commits `9c7aa654` pre-run lists, `438f0481`
  D-COV-EIG amendment 1).
- **Status: STOPPED (OWNER STOP), partial.** Stopped at a clean point: no merge open (`MERGE_HEAD` absent), no run,
  build or test of mine in flight (none had to be ended), nothing half-merged. No identity miss, no red, no conflicting
  `--rule` seen (probe not run), no permission refusal.
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at start and end (0 trials).
- **Next free build tag: `p9-2r`.**

## Builds (through `scripts/research-build.ps1` via `m2j-logs/tools/build.sh`, target-scoped, one at a time, `-Canary`, `CCACHE_RECACHE=1`)

| tag | preset | source | targets | TUs / wall | warnings | canary |
|---|---|---|---|---|---|---|
| p9-2p | equity-dev | `327c86b2` | atx-impl-strategy-target-tests, atx-impl-tests, atx-equity-strategy-targets, -ic, atx-equity-strategy, -risk, -mine, atx-research-eval, atx-impl | 130 / 731 s | 0 | 15 passed (Debug goldens) |
| p9-2q | equity-rel | `327c86b2` | same nine | 132 / 769 s | 0 | 15 passed (Release goldens) |

Exes (EXE-CUR; SHA-256 prefixes, full values in `m2j-logs/mega-p9-2{p,q}-receipt.json`; source `327c86b2`; no C++ /
CMake change since):
- Debug p9-2p: targets **`637f7069`**, target-tests `975e0a01`, impl-tests `3d5f7efe`, ic `919569fb`, strategy
  `3dc162d9`, risk `a50bb015`, mine `1e9e0cda`, atx-impl `fbdcf2c2`, eval `bd8b5e08` (no work: unchanged).
- Release p9-2q: targets **`adcf72d4821de049f2e83db43759f3d28df4493504956739e1ed40f580656368`**, target-tests
  `2a4913aa`, impl-tests `dd753dcf`, ic `1d24336f`, strategy `257d4b30`, risk `0afaffcc`, mine `cca75312`, atx-impl
  `099e68e5`, eval `a57a2090` (no work: unchanged).
- Note: the Release risk exe that wrote the two D-COV stores (p9-2o `ccc73cd1`) is replaced in `build-equity-rel/bin`
  by p9-2q `0afaffcc` (core split relink). The stores are pinned on bytes (CRT-2), not on the producer.
- Pre-merge Release exes kept for the optional tiny-world before / after (`fix1-nav-identity.py old --bin`): session
  scratchpad `m2j/prebin-rel` (27 MB: targets `53ca5dd4`, ic `fccaa07e`, eval `a57a2090` + DLLs; `m2j-logs/prebin-rel.txt`).

## Part A items

| item | state | result / evidence |
|---|---|---|
| merge | DONE | `327c86b2`, 0 conflicts; COVR's and C3's lines both in `atx-impl/tests/CMakeLists.txt`; no root script names `atx-impl-core`; lane report / handoff / review / re-review 1 / `c3-logs/` arrived with the merge (`m2j-logs/c3-merge.txt`) |
| 1 builds | DONE | table above, 0 warnings, canary 15 on both goldens |
| 1 gtests | PARTLY | named filter (11 suites) **33 / 33** Debug and Release; whole `atx-impl-strategy-target-tests` Debug **390 passed, 0 failed, 1 DISABLED** (COVR diagnostic). **NOT run:** Release whole target-tests; whole `atx-impl-tests` on either build (`gt-exits.txt`) |
| 2 flag-absent identity (C3-6) | DONE, **holds** | X-5 Release `p9-m2j-x5-argv` vs `p9-c1-s2-x5-nav` (X-5's reference of record; X-5 had no P9-B0 run, see the list): 26 / 27 identical, `summary.json` only `/producer/build_type`, `/producer/engine_git_sha`; vs Release twin `p9-c1-rel-x5-nav`: only `/producer/engine_git_sha`. Y-F0 `p9-m2j-yf0-argv` vs `p9-b0-yf0-nav`: 26 / 27, the two producer keys only (`item2-cmp.txt`) |
| 3 `--spec` vs argv | DONE, **holds** | schema `atx.nav-spec/v1`; printed specs x5 `53fd737d0aee52b4083a8bae6cf3052ad0c7a6579b3f7c608efa22e3aa7af67f`, yf0 `a793e1832d3421d51a8e25fff69a2f65e01b63a787bc1ec241832b1f5fa00bb4` (= the digest each spec run printed); spec runs 27 / 27 byte-identical to the argv runs (`item3-cmp.txt`) |
| 4 opt-in (PRE-7a, C3-9) | **NOT RUN** | list and tool ready (`optin_cmp.py`; run `nav_run.py argv x5 rel p9-m2j-x5-optin --ann-mean-gross`) |
| 5 Release = Debug NAV, wall | **NOT RUN** | Debug run not made (`nav_run.py argv x5 dbg p9-m2j-x5-dbg`). Release walls from receipts: X-5 38.4 s (after split) vs 41.7 s (pre-split twin); Y-F0 35.8 s vs 27.4 s (pre-split `-hrun`, with holdings); load not controlled |
| 6 SQL2-CLS | **NOT DONE** | `atx.nav-spec/v1` into class `nav-output` (list says why), guard before / after, store suite seeds 0 / 1, fixture chain `d32f7655...4e69`. Until then the classes guard is expected red |
| 7 W3-8 flip | **NOT DONE** | `new-cost-model` out of `SCHEMA_ONLY_FIXTURES` in `scripts/tests/test_wave_kinds.py` (lines not yet located: D3 is editing the file) |
| 8 C3-5 probe | **NOT RUN** | `m2j-logs/tools/m2j_c3_probe.py base`, `wave c3r`, `report c3r` (rule `aim-partial-v5`, trade_fraction .1, pm6-6-engine); needs the p9-2q Release bin (current) |
| 9 ROOT-1 | **NOT RUN** | `scripts/tests` seed 0 and stage chain, separate invocations (`m2j-logs/tools/pyt.sh`) |
| M2I-HK | **NOT DONE** | `build-equity/p9-m2i-cov-y1-nav` (12 MB) still on disk |

## Part B: D-COV-EIG (COV-13 (b)) — DONE

- Attempt 1 (`m2j-logs/dcov-eig-attempt1.txt`) is void: the list's own check failed (per-rank counts 1,278 vs T_k
  1,089: the tool counted 189 factor blocks before the first specific forecast, which the harness never forms,
  `strategy_risk_model.cpp:1182`, `:1377`). Slip fixed in the tool; amendment 1 committed (`438f0481`) before attempt 2.
- Attempt 2 (`m2j-logs/dcov-eig.txt`): pins verified; session check EQUAL on all 51 ranks in both stores. **No rank
  00-02 is structurally null in either recipe** (median share (i) + (ii): .70 / .82 / .04 in both). Ranks 00-01 are
  near-null directions on industries (ind_ff39, ind_ff24) with a .2-.3 style share; rank 02 is a style direction.
- Third pooled row (`dcov-eig-pooled.txt`): nothing excluded, so it equals the all-ranks row (appended in gate report
  4 (d)).

## Kept outputs / deleted

- Kept (new, regenerable, 11 MB each): `build-equity/p9-m2j-{x5,yf0}-{argv,spec}` and their `-run` dirs. The item-4 /
  item-5 comparisons need `p9-m2j-x5-argv` as their base: keep it until they run.
- Never deleted: `p9-m2i-dcov-v11`, `p9-m2i-dcov-covv1`, `p9-m2d-cov-risk-container`; Debug / Release object trees.
- Deleted by name: my synthetic test store (session scratchpad `m2j/synth`). No pytest basetemp was made.

## Known-red set

- Empty (Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` stays a named skip). Expected, not yet
  observed: `atx-engine/tools/test_research_store_classes.py` fails on `atx.nav-spec/v1` until SQL2-CLS (item 6).

## Uncommitted state at the end

- None of mine. `progress.md`, `p9-prereg.md`, `r6-wave3-common.md`, `resume6-addendum.md` committed as they stood
  (LEDGER-1). Four untracked `docs/plans/*.png`: not mine, left. No lease touched.

## Open questions for the PM (one line each)

1. Resume M2j from item 4 in the same order (4, 5, 1 rest, 6, 7, 9, 8 last), or re-plan after the owner stop?
2. X-5's reference: X-5 had no P9-B0 run; I used its C1 / DEC-20 reference of record `p9-c1-s2-x5-nav`: confirm.
3. D-COV-EIG: no rank is structurally null, so P9-V's predictions quote the all-ranks row (dominated by ranks 00-01): rule whether that stands or a different exclusion rule is wanted (COV-13 (c)'s carry still applies).
4. Should `build-equity-rel/bin` keep a copy of the store producer exe p9-2o `ccc73cd1` (now replaced by `0afaffcc`)?
