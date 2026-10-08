# Handoff: root after M1 (RESUME 8: SQL3 merge), 2026-10-06

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Started from `handoff-root-m2j-fin.md`. Rows:
`integration-log.md` "P9 wave-3 merges" / "M1"; detail: `root-wave3-merge-report.md` "SQL3"; evidence `m1-logs/`
(tools `m1-logs/tools/`).

- **Status: DONE.** Task SQL3 is MERGED AND VERIFIED in root (item 5 pending by rule: B2's redo is not in root).
- **Code head: `fd084e78`** = merge `fbd2c045` (SQL3 `a02c074f`, `--no-ff`, 0 conflicts) + `fd084e78` (SQL2-CLS:
  `atx.run-store/v1` on the `run-receipt` and `run-start` rows of `classes.json`).
- **Docs head:** the commit that adds this file. Earlier docs commit of this group: `cfd5a25d` (step 0 housekeeping).
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at start and end (0 trials).
- **Next free build tag: `p9-2t`.**
- **Known-red set: EMPTY.** Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` stays a named skip
  (not run in M1).
- No lease touched. No permission refusal. No real data opened, no statistic read, `holdout_gate.py` not run.

## Exes (EXE-CUR)

New this group (source `fd084e78` for both builds; receipts `m1-logs/mega-p9-2{r,s}-receipt.json`):

| exe | Debug p9-2r (`build-equity/bin`) | Release p9-2s (`build-equity-rel/bin`) |
|---|---|---|
| atx-research-store | `d8a500d84dc2a25130c69b226d8f5deec47b8dabb6256522c83c00764d3d9d65` | `0d6083af4083d7f613956a6480ef3a9b3570c95cf929c8163124b018365e4770` |
| atx-engine-research-catalog-tests | `f1134aaa72338346fcb642ddd57ff15fe6e59cdeb078f1c20782dca45c0f78ed` | `712907c98c3090f6faa5284a6752aecc6eddcded118bbc901b140ca9b8d3b192` |

Unchanged and still current (`git diff --name-only 327c86b2..fd084e78` over C++ / CMake / presets touches only the
catalog library and its test; no other target links `atx::engine-research-catalog`): the nine Debug p9-2p / Release
p9-2q exes of `handoff-root-m2j-fin.md` (source `327c86b2`): NAV `637f7069...` / `adcf72d4...`, IC `919569fb...` /
`1d24336f...`, strategy, risk, mine, `atx-research-eval`, `atx-impl`, `atx-impl-strategy-target-tests`,
`atx-impl-tests` (full SHA-256 in that handoff). The old Debug store exe p9-1u `3b955ba1` is replaced by p9-2r.

## Results of this group

| item | result (evidence in `m1-logs/`) |
|---|---|
| 0 housekeeping | `cfd5a25d` |
| SQL3 merge | `fbd2c045`, 0 conflicts; merge tree = `git merge-tree` recompute; code = lane's `a02c074f` |
| SQL2-CLS | `fd084e78`; guard 1 failed -> 5 passed; store suite 43 passed x seeds 0 / 1; fixture chain `d32f7655...4e69` unchanged, rel = abs, verify 0, identity 12 / 0 |
| builds | p9-2r (equity-dev, 14 TUs), p9-2s (equity-rel, 25 TUs), 0 warning lines each |
| gtests | `ResearchCatalog*:SealGuard*:StoreCli*` 28 / 28 on Debug and on Release |
| pytest | hook file 8 (real exe p9-2r, no skip); book monitor 13; stage chain 10 x seeds 0 / 1; `scripts/tests` seed 0 @@S0@@; seed 1 @@S1@@ |
| tiny-world identity (catalog off) | **holds**: fake wave 69 / 69 identical pre-merge vs merged (F-1 filter); runner `start.json` / `receipt.json` equal outside the substitution list, no `store` key |
| SQL3-2 | **holds, hook OFF**: `build-equity/research-store/catalog.sqlite` absent; 0 of 1,473 receipts / start receipts carry `atx.run-store/v1` |
| G-P10 | (a) P10 for SQL3 (gate store step; W3-4 landed), (b) PARTIAL, (c) the gate's, (d) PARTIAL, (e) LANDED at `fd084e78` |

## Kept and deleted outputs

- Kept (regenerable): the Debug and Release object trees `build-equity/`, `build-equity-rel/` (C: 50 GB free, above
  the 12 GB line; the next root group reuses them). All M2j outputs listed in `handoff-root-m2j-fin.md` stay as they
  were (their deletion waits on the PM's C3-5 ruling, unchanged here).
- pool-13's `build-equity/` object tree (about 410 MB) kept by SQL3-TREE through the item-5 round (exact names to
  delete afterwards are in `root-wave3-merge-report.md` "Disk and status").
- Deleted: my scratch in the session scratchpad by name (`m1-fw/`, `m1-run/`, `root-m1/`, `m1-merge-msg.txt`).

## What the next root agent must know

- **SQL3-2 before P9-B0 / the cells:** check `build-equity/research-store/catalog.sqlite` does NOT exist in pool-2
  (the hook turns on when it and `build-equity/bin/atx-research-store.exe` both exist); the gate's store step writes
  its catalog at a non-default `--catalog` path (e.g. `build-equity/research-store-gate/catalog.sqlite`) and the gate
  report states it. Also list any `index.sqlite` under a directory a cell's argv names (or its parent, or one level
  below) before the first real run: `selection` would record it into a `store` block (SQL3 review item 5; expect none,
  SQL-11).
- **SQL3 item 5** (three call lines in `cycle_resume.py`, `cycle_verdict.py`, `wave_stage_record.py`) is the lane's
  follow-up after B2's redo merges; then a scoped re-review and a short root merge (Python only: no build; ROOT-1
  under seeds 0 and 1; the hook file with the real exe).
- **Tiny-world identity tool** for later Python merges that touch the wave: `m1-logs/tools/fakewave_f1.py REPO ROOT`
  (one fixed ROOT for both heads; compare the third column).
- **Basetemp:** create the `--basetemp` parent dir first (pytest creates only the leaf; slip in this group).
- **Interpreter:** `C:/Program Files/Python312/python.exe` explicitly (the first `python` on PATH in Git Bash is
  another session's venv).
- The Bash tool's heredoc can fail on long texts with apostrophes (hook layer); write long record files with the
  editor tool.

## Uncommitted state at the end

- None of mine. `p9-prereg.md` (modified) is the PRE-2b-i agent's: not added. Four untracked `docs/plans/*.png`: not
  mine, left. `disk-audit.md` (untracked, ignored): not in the step-0 list, left.

## Open questions for the PM (one line each)

1. pool-13's object tree: kept under SQL3-TREE for the item-5 round (real-exe hook case); confirm, or rule that root deletes it now.
2. Review S4 (stale `catalog.hpp:27` comment): left to P10 or the next lane that edits the catalog header (a header edit makes the catalog exes stale under EXE-CUR); confirm.
