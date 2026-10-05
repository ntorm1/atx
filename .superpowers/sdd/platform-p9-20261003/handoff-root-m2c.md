# Handoff: root M2c (OD-A3-6F, S2 merge), 2026-10-04

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Code head **`f6444417`** (the S2 merge). The docs commit
that adds this file sits on top of it. Detail is in `root-wave2-merge-report.md` section M2c and the M2c row of
`integration-log.md`. Evidence logs are in `m2c-logs/`, including the exe SHA-256 lists and the scripts used. Nothing
is half-merged and nothing of mine is running. Next free build tag: **p9-2a**.

**Status: BLOCKED on one item.** A Release red outside the known-red set:
`AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (evidence:
`m2c-logs/release-atx-engine-alpha-tests-whole.log:803-822`). Every other item of the step group is done.

## Done

| step | result | commits |
|---|---|---|
| 0 OD-A3-6F | reference grep: no spec / pin / test reads them; five `build-equity/p9-a3-train-*` dirs deleted by name (about 762 MiB) | - |
| 1 S2 merge | `2ed2fc18` -> `f6444417`, no conflict; `task-S2-rereview-1.md` copied in (report, review, handoff and `s2-evidence/` came with the merge) | `f6444417` |
| 2 re-proof | builds p9-1x (Debug), p9-1y (Release), 0 warnings each; p9-1z Release `atx-shm-worker` (test prerequisite). Debug: S2 filter 28p, ic 204, mine 44 (golden 1 + 4 workers), alpha 782, factory 396, all green. Release: filter 28p, ic 204, mine 44 (golden 1 + 4), factory 396 (after p9-1z), alpha 779 passed / 1 skipped / **1 failed** (the red). Canary 5 digests green Debug + Release. pytest 8 + 15 (+ Release 15) | - |
| 3 digests | merged tree: `dsl_vm` `5bcd55c7`, `ic_result` `dd3c6011` = lane = pins; no other lane touched a digested source | - |
| 4 i | X-5 u / w vs P9-B0: payloads byte-identical; summary differs only at timing leaves plus `/roles/0/ic_cache_bytes` (+56 B, the sizeof growth of the S2-1 fields). Cold Release u pre- vs post-S2: 163.7 s -> 93.7 s, peak equal, payloads and 67 / 67 signal payloads byte-identical | - |
| 4 ii | path margin, Release + audit-exact from pool-2: lo3 real 251 (-1 vs 250); parent `lib-v8ysb-gm` 250 (margin 0) | - |
| 4 iii | `exe_capabilities` append-only (5 -> 8, sorted, existing kept) | - |
| 4 iv | no new literal (guard green); fixture chain `d32f7655...4e69`, identity 12 / 0 | none needed |
| extra | S2-6: no `imputed-v1` entry under the spec caches. P13: live manifests v13 / v15 carry seal `2024-01-01`. DISK-2 pool-18: already clean (0 B). My two cold caches (8.9 GB) deleted | - |

Trial ledger at the end: `build-equity/trials.jsonl` **133 lines, sha256 `27e40f9f...`** (unchanged; 0 trials).

## Known-red now

- pytest `scripts/tests/test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` (E2's M1a-RED).
- Release `BookNormalScore.*` x2 (M1c-RED, C2). Not re-run: its target is not in S2's list.
- **New, ruling needed:** Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`.
  - Release prints `0xeeae40c4a6d7d93b / 0x24c97b5bfeab6518 / 0xeeae40c4a6d7d93b` (vm audit-exact, vm research-fast,
    oracle). The Debug-captured pins are `0x971c3da6... / 0xe9e7128f... / 0x971c3da6...`.
  - The oracle misses its pin too, while S2 changed the oracle only by a predicate that `OpSig.PredicatesAgree` proves
    equal. So I attribute the red by code reading to a pre-existing Release-vs-Debug FP difference in a golden pinned
    on Debug (W2, `41894ea4`). It is first exposed now: this is the first Release build of `atx-engine-alpha-tests`.
  - Not proved empirically: that needs a Release build of the pre-S2 head `4497784b`.

## Not done / next

- The PM rules on the new red. Options: (a) known red, plus Release pins beside the Debug ones; (b) contraction-free
  battery (C2-RED precedent); (c) attribution build first.
- The wave-2 P9-B0 substitution list must add `/roles/*/ic_cache_bytes` (u and w `summary.json`, every build type).
- W1-I1 / E2: with the lo3 cache root, a field candidate's id must be <= 18 chars for Release + audit-exact to stay
  <= 250. The 24-char AL / PRE budget needs a cache dir <= 51 chars, or a shorter out root.
- Remaining wave-2 merges: E2, B2, D2, then AL-COMB (re-check the W1-MIN M-2 theme seam), C2, COV, T2 last.
  - D2 / AL-COMB will meet S2's `exe_capabilities` (keep it sorted, size = count) and `strategy_ic_runner*.cpp` tails.
- The Release tree now holds alpha / factory / mine test exes and `atx-shm-worker` (useful for the wave-2 Release suite).
- Carried: ALSIG-3; M2A-4; S2-FIX1 (3) (30-day cause-classification clock) goes to the P9-L prereg; S2-6 is re-checked
  before P9-L runs.

## Uncommitted state

- `progress.md` is modified (PM's; not touched). Four untracked PNGs under `docs/plans/` (not mine; left alone).
- Kept as evidence in `build-equity/`: `p9-m2c-yf0-{u,w}(-run)` and `p9-m2c-cold-u-{old,new}(-run)` (149 MB).
  Receipts `mega-p9-1{x,y,z}-*`.
- Session scratch (`m2c/`: basetemps, chain catalogs, scripts) is deleted by name at the end. The scripts are kept in
  `m2c-logs/tools/`.

## Open questions for the PM

1. Ruling on Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (options above).
2. Accept `/roles/*/ic_cache_bytes` (+56 B, accounting only) as outside identity, and add it to the wave-2 P9-B0 list?
3. Path margin: the current parent sits at exactly 250 under Release + audit-exact from pool-2. Do P9 audit-exact cells
   run from a shorter out root now, or only when E2's refusal fires?
4. FYI: P13 now refuses 10 v6 / v7-era `recent-fast-*` fields manifests (seal `2025-01-01`). Only superseded specs name
   them.
5. FYI (blind rule): I once printed a stdout tail of my own cold-u run that showed one candidate's screen-reason label
   (no number). No statistic was read.
