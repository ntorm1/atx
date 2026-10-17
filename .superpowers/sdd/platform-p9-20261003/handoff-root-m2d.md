# Handoff: root M2d (D2 merge, COV merge), 2026-10-04

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Code head **`694b4b95`** (the COV merge). The docs
commit that adds this file sits on top of it. Detail: `root-wave2-merge-report.md` section M2d and the M2d rows of
`integration-log.md`. Evidence: `m2d-logs/` (gtest / pytest logs, receipts p9-2a / 2b / 2c, identity reports, bench
log, scripts in `m2d-logs/tools/`). Nothing is half-merged and nothing of mine is running. Next free build tag:
**p9-2d**.

**Status: DONE.** No new red; no identity miss.

## Done

| step | result | commits |
|---|---|---|
| housekeeping | PM's untracked docs committed (`wave3-carry.md`, `briefs/brief-COV-MV.md`, `resume4-dispatch.md`, `main-merge-verify.md`, `al-data/*.md`) | `64069a3c` |
| D2 merge | `49af0a52` -> `61378337`; CMake tails (engine + engine tests) kept both; `exe_capabilities` untouched (8, sorted) | `61378337` |
| D2-9 slips | 3 long lines wrapped (whitespace only); report S-1 sentence corrected | `e129a7fa`, docs |
| D2 SQL2-CLS | `atx.composition-fit-spec/v1`, `atx.composition-fit/v1` -> fit-output; fixture chain `d32f7655...4e69`, identity 12/0 | `c0446b19` |
| D2 re-proof | p9-2a (9 targets, 0 warnings, canary 15p); gtests 10 / 1 / 206 / 338 / 243 / impl 1135p + 5 skip; pytest 264 (real exes, S-7 incl.) + 7 | `4c9dfd9d` (docs) |
| D2 root items | (i) pair pin `df589a88` recomputed; (ii) no P9 parent uses a resid parent; (iii) flag-absent pre-D2 vs merged fitter: only CM-5 / D2-2 keys + `provenance.admission_sha256` (+ `resid.parent_weights_sha256`), tiny world + 9 fixture cases; draft P9-B0 list in the report; (iv) superseded | - |
| COV merge | `c8ff4f1c` -> `694b4b95`, no conflict; no new literal (guard green, chain unchanged) | `694b4b95` |
| COV re-proof | p9-2b (5 targets, 0 warnings); RiskCovContainer 21p, Risk* 431p + 1 nightly skip, Cov* 29p, unchanged 104p | - |
| COV root items | S12 slip fixed; S9 holds (`returns_applied false`); flag-absent store rebuild = `v8-risk-lo3-v10` (10/10 payloads, manifest only `producer.*`); `--emit-container`: `cov-diff --legacy` exit 0 (1405 sessions); bench p9-2c run once (medians in the report; 4.12 targets met except first-touch N 5627 f64 1.04 ms) | `70104732` (list first), docs |

Trial ledger at the end: `build-equity/trials.jsonl` **133 lines, sha256 `27e40f9f...`** (0 trials).

## Known-red now (unchanged)

- pytest `scripts/tests/test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` (E2).
- Release `BookNormalScore.*` x2 (C2).
- Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (M2c-RED; lane S2R).

## Open items (next root)

- Wave-2 gate (not done here, as ruled): D-COV; COV-7 Debug = Release container identity on the lane fixture; COV
  report step 4 (Y-1 NAV through the container store vs its P9-B0 reference; `build-equity/p9-m2d-cov-risk-container`
  is kept for it, 968 MB, manifest pins in its run receipt) and step 5 (`atx-cov-v1` on TRAIN, Release, after COV-7).
- Wave-2 P9-B0: rule the D2 part of the substitution list (report, "Draft wave-2 P9-B0 substitution list"), plus
  B0-W2-1's `/roles/*/ic_cache_bytes`.
- At E2's merge: the positive live-table check (frozen `D1_SCHEMAS` = merged exe's `--list-rules --json`).
- AL-COMB: merges after D2 (now possible); W1-MIN M-2 theme seam check there; it meets S2 / D2 in
  `exe_capabilities` and `strategy_ic_runner*.cpp`; C2 meets COV in the `atx-impl-strategy-target-tests` source tail.
- W3-11: pools 20 (D2) and 24 (COV) are merged; release / re-lease for D3 / COV-MV is the PM's / root's next step
  (not done here: no lease command was run).
- Engine owner note (COV): `atx-engine/src/book/risk_target.cpp:35` `k * k` in usize, unreachable via a container.
- The Release tree `build-equity-rel` now also holds `atx-engine-bench.exe` (p9-2c) and `deps/equity-rel/benchmark-*`
  (12 MB); its cache is back to `ATX_BUILD_BENCH=OFF`.

## Uncommitted state

- `progress.md` (PM's) modified: not touched.
- `al-data/{atx-db-request,builder-designs,task-AL-DATA-report}.md` and `wave3-carry.md` were modified by another
  agent after my housekeeping commit (content edits, 4 files): not mine, not committed.
- Four untracked PNGs under `docs/plans/`: not mine, left alone.
- Kept in `build-equity/`: `p9-m2d-cov-risk-container` (+ `-run`), `p9-m2d-cov-risk-legacy-run` (receipt only; the
  481 MB payload copy was deleted by name). Receipts `mega-p9-2{a,b,c}-*`.
- Session scratch (tiny world `%TEMP%/m2d-tw`, fixture root `%TEMP%/m2d-fx`, old tools tree, basetemps, chain
  catalogs) deleted by name at the end.

## Questions for the PM

1. COV identity limits: the brief's `--max-rss-mib 8192` can never be admitted on this host (the runner needs free
   >= peak + floor = 8,704 MiB); I ran with 2,048 (argv unchanged, peak 612-625 MiB). Should later COV briefs state a
   feasible limit?
2. FYI (blind rule): one S9 print previewed three 2018 delisting events' metadata, and one print showed the store's
   `mean_r2` (a fit diagnostic); no return / IC / Sharpe / turnover / NAV statistic was read.
