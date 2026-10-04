# Handoff: root after M2g (wave-2 gate diagnostics, part 1; no merge), 2026-10-04

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Detail: `root-wave2-gate-report.md` (each item's
pre-run list, committed before its run, then the result); rows: `integration-log.md` "M2g"; evidence `m2g-logs/`.

- **Code head: `30b9c9d2`** (= `42f42a07` + the AL-DATA spec amendments 5.1 / 5.2).
- **Docs head:** the commit that adds this file (on `ec14d3ec`; earlier M2g docs commits `3fd0bf98`, `1b8c4df2`,
  `29adf2fa`, `ec14d3ec`).
- **Status: DONE_WITH_CONCERNS.** Items 1, 2, 3, 5, 6 done; item 4 stopped at COV-7 (identity miss), so D-COV and COV
  report steps 4 and 5 were NOT run and no `atx-cov-v1` store exists. Nothing of mine is running; nothing half-merged.
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at start and end (0 trials).
- **Next free build tag: `p9-2m`** (used: p9-2k equity-dev, p9-2l equity-rel).
- **UCRT** `ucrtbase.dll` 10.0.26100.7623, SHA-256 `2c08e77f...65ef` (unchanged).

## Results (numbers allowed to be printed)

| item | result |
|---|---|
| 1 AL-DATA | `al-data/*.md` were already committed as they stood (`ab189471`); D-n1 / D-n2 applied (`3fd0bf98`); 5.1 + 5.2 in one spec commit `30b9c9d2` ("before" lines byte-identical); registration tests 11p before / after; 5.3 not applied (P10) |
| 2 ALD-DIAG | names / delisted <= 2023-12-01 / unclassified: 2020 2,420 / 83 / 0; 2021 2,540 / 103 / 0; 2022 2,325 / 106 / 0; 2023 2,186 / 95 / 2. 387 + 2 exits in all; M&A dominate (performance 5 in four years) |
| 3 B2-13 | X-10 vs Y-F0 rho 0.9994863034956717 (1 - rho 5.137e-4); Y-1 vs X-10 rho 0.9947113980219977 (5.289e-3); P9-B0 = ledgered; no degenerate pair |
| 4 COV | S9 holds; **COV-7 MISS** (both recipes); D-COV / steps 4 / 5 not run |
| 5 Q5 | q +0.0502 (size +.0334, residual_vol +.0275, liquidity -.0107); q_lit +0.0671 (beta +.0091, leverage +.0078); ex-ante vol ratio not produced |
| 6 S2R-DIAG | `smax` 0, `smax5` 0 cells NaN under AuditExact and finite under ResearchFast (94.6 s run) |

## COV-7 miss (needs a PM ruling before D-COV)

- Lane fixture (`CovPanel(300, 200, 23)`, extracted from the Debug test exe's temp dir and hash-verified), both risk
  exes, both recipes, `--emit-exposures all --emit-container --bias-families extended`: `cov-diff` exit 1 at block 2
  (both exes); container blocks differ (v1.1: 45,796 block bytes + 9,335 block-hash bytes; atx-cov-v1: 49,934 +
  9,342); factor covariance median 17 / 24 ULP (max rel 1.5e-9 / 3.3e-11), specific variance median 4 ULP. Exposures,
  slots and factor returns identical; `diagnostics.csv` first moves at `bias_factor` / `lambda2_factor`: the variance /
  EWMA / VRA arithmetic. Each build deterministic run to run. Cause not attributed (stop rule); candidates: CRT
  transcendental functions (`ucrtbased` vs `ucrtbase`, M2c-RED-2 class) or reduction order.
- The same holds on TRAIN (seen in Q5's Debug cross-check): Release atx-risk-v1.1 store vs Debug, factor covariance
  99% of cells, median 52 ULP, max rel 1.1e-8; the Debug store equals the store of record `v8-risk-lo3-v10`, the
  Release one does not. G-2a's printed q is unaffected.
- Note: the root-hash criterion of re-review 2 ("equal root SHA-256") can never hold across builds: META carries
  `producer.executable_sha256`. The pre-run list replaced it by "bytes equal outside META's producer and the two root
  fields"; that criterion is what missed.
- Options for the PM (root's reading, not a ruling): (a) D-COV in Debug (atx-risk-v1.1 Debug 53 s on TRAIN;
  atx-cov-v1 Debug was 4x Release on the fixture, so TRAIN may exceed the runner's 600 s cap); (b) a lane makes the
  risk estimator build-invariant (moves no Debug byte, like C1 / S2R) and COV-7 is re-run; (c) accept Release as the
  producer of the `atx-cov-v1` store of record, documenting the ULP-level Debug / Release difference (P9-X / Y-1 keep
  reading the pinned Debug-built v1.1 store).
- Kept for the ruling: `build-equity/p9-m2g-cov7/` (fixture copy, four synthetic outputs, repeats; 163 MB).

## D-COV store (W3-10)

None produced. The D-COV pre-run list stands for the re-run, with `--seconds 600` (the runner refuses more).
`build-equity/p9-m2d-cov-risk-container` (968 MB, manifest `9c12852e...17c7`) stays kept for COV step 4.

## Trees

- `build-equity` (Debug, equity-dev): configured at `c2416d62`; bin as after M2f except `atx-equity-strategy-risk`
  p9-2k `94b5a5e7`.
- `build-equity-rel` (Release, equity-rel): configured at `c2416d62`, bench OFF; bin as after M2f plus
  `atx-equity-strategy-risk` p9-2l `a31a0f96` (new in this tree) and `atx-equity-strategy-targets` p9-2l `53ca5dd4`
  (was `cb2df40d`).
- Receipts `build-equity/mega-p9-2{k,l}-*` (copies in `m2g-logs/`).

## Known-red set (unchanged)

- pytest `scripts/tests/test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` (E2).
- Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`: a named Release skip, 0 failed (OD5-2).
- No gtest / pytest suite was run in M2g beyond `test_p9_al_sig_registrations.py` (11p) and the one fixture gtest
  `CovRecipe.CovV1RecipeRecorded` (Debug, passed). COV-7 is a gate identity miss, not a test red.

## What remains for the wave-2 gate proper (after the last merge)

- D-COV in the form the PM rules for COV-7, then COV report steps 4 and 5; the `atx-cov-v1` store's manifest pins for
  W3-10 (P9-MV / P9-V).
- Suites Debug + Release, golden at 1 and 4 workers, canaries, P9-B0 (+ UCRT record), the W1-I2 (b) re-pin, the store
  step (M2A-4), scoreboard, G-P ticks, the whole-wave-2 review (a finding in COV or AL-COMB code re-runs D-COV / Q5 once,
  zero trials); optional S-5.

## Kept outputs (M2g)

`build-equity/p9-m2g-cov7/` (163 MB), `p9-b0-yf0-nav-h` (Y-F0 holdings, 163 MB: reusable for a G-2a on P9-H's parent),
`p9-b0-yf0-nav-hrun` (11 MB), `p9-q5-book.csv` (92 MB, `8f756fa2...`), `p9-q5-risk` (Release Q5 store, 481 MB),
`p9-q5-diagnostics/` and `p9-q5-diagnostics-dbg/` (G-2a JSONs), and the run dirs `*-run` (receipts). Deleted by name
after their results were committed (regenerable): `build-equity/s2rd` (audit cache, 4.2 GB), `p9-m2g-s2rd-u`,
`p9-q5-risk-dbg` (481 MB); the session's pytest basetemps. C: 42 GB free at the end.

## Uncommitted state at the end

- None of mine. `progress.md` committed as it stood (LEDGER-1). Four untracked `docs/plans/*.png`: not mine, left.

## Open questions for the PM (one line each)

1. COV-7 missed (Debug vs Release risk estimator differs at ULP level, both recipes, fixture and TRAIN): which of
   (a) / (b) / (c) above?
2. Q5's cell: the spec's nominal X-5 was replaced by Y-F0 (last accepted unlevered cell, P9-H's parent), named in the
   pre-run list before the run; confirm.
3. ALCOMB-9 (b): Q5 cannot produce an ex-ante vol RATIO (one book); PRE cuts it unless G-2a on both books is
   registered (the parent's G-2a and holdings are kept).
4. Bounded-runner cap: `--seconds` above 600 is refused; any brief that states a larger figure (D-COV's list did)
   must state 600.
