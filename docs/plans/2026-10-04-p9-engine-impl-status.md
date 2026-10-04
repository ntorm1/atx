# atx-engine and atx-impl: status at the P9 owner stop (2026-10-04)

What the research platform holds today, which book is the best primary implementation, and what comes next.
Integration tree: `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`, code head `694b4b95` (docs head
`6960568f`). Book numbers are copied from the final v8 report (`docs/plans/2026-10-03-platform-v8-status-8.md`);
nothing was recomputed for this file. Engineering facts come from the P9 ledger
(`.superpowers/sdd/platform-p9-20261003/progress.md`) and the root merge reports beside it.

## 1. Bottom line

- **Best primary implementation: Y-F0**, the unlevered deployable book (spec `lib-v8ysb-gm.json`): net Sharpe 1.8495,
  net 5.654% a year, max drawdown 2.580%, in sample on 2020-2023.
- **It reproduces on the P9 code.** The re-base "P9-B0" re-ran it on the wave-1 head: 6 runs, 0 differences outside
  the written substitution list. After the wave-2 IC-engine merge (S2) its u and w payloads are still byte-identical
  to those references; the summaries differ only in timing fields and one memory-accounting field. The pinned
  references are `build-equity/p9-b0-*`.
- **P9 has adopted nothing new yet.** No P9 cell has run; the trial ledger is unchanged (133 lines, sha `27e40f9f`).
  The sprint so far is platform work: 8 of 8 wave-1 lanes and 7 of 12 wave-2 lanes are merged and verified.
- **Two findings this session limit what the numbers mean** (section 5): the book's universe reads 2026-dated inputs
  through the atx-db identity bridge (size unmeasured), and `pow` differs by 1 ULP between the Debug and Release C
  runtimes.
- **Speed:** a cold Release signal pass (u) fell from 163.7 s to 93.7 s with byte-identical outputs.

## 2. The primary book and its two levered forms

Scenario S2 (`modeled-1bn-stale5-v1+swap-fin-v1`), TRAIN 2020-2023, 1,005 return sessions. Source: status 8 section 1.

| book | spec | leverage | net annual | net Sharpe 1x | net Sharpe 4x NAV | vol | max drawdown |
|---|---|---|---|---|---|---|---|
| **Y-F0** (primary, unlevered) | `lib-v8ysb-gm.json` | L 1.1828 fixed | 5.654% | 1.8495 | 1.6994 | 3.057% | 2.580% |
| X-10 (fixed leverage) | `x-leverage-L2.0.json` | L 2.0 fixed | 9.306% | 1.8083 | 1.5829 | 5.146% | 4.326% |
| Y-1 (vol-target) | `y-vol-target-y-1.json` | cap 2.0, mean 1.8671 | 8.799% | 1.8298 | 1.6281 | 4.809% | 3.602% |

What Y-F0 is: 67 candidates in the library `lib-v8ysb-gm`, grouped into 12 themes, combined by the rule
`theme-erc-v1`, on the universe role `train-2020-2023-lo3`.

Which form is deployed is the owner's decision (OD-P9-3; P9 keeps L <= 2.0). X-10's Sharpe loss against Y-F0 is the
only one of the three leverage-pair tests that separates from zero (dSR -.0412, p two-sided .0048). Y-1 reads a risk
model and so can never get the history read.

Significance, as printed in status 8: deflated Sharpe .8350 at 62 construction cells; the G-B1 floor (1.0) is met and
the P9 target (1.85) is missed by .000518.

Limits of these numbers:
- All in sample: the book was chosen on the window it is scored on.
- No history read yet: OD-3 is deferred to the P9 freeze.
- Costs are modelled; no borrow-fee data has been bought.
- The universe finding in section 5.

## 3. How the book is built: one row per stage

| stage | executable / code | home | state at head |
|---|---|---|---|
| fields | `atx-research-fields`; registry `atx-engine/tools/field_registry.json` | atx-engine `research/fields` | JSON registry with C++ builder kinds. 9 rows are engine-built and identity-proven on TRAIN (3 from A2, 6 vendor-panel rows from A3); the other rows still run Python builders, which are frozen. AL-SIG added two engine-only rows (`gia_13f`, `russell_recon`); `russell_recon` is marked not point-in-time and the IC runner refuses it |
| signals and IC (u, w, marginal) | `atx-equity-strategy-ic`; DSL VM | atx-impl; atx-engine `alpha`, `factory` | Release build adopted for IC. S2 merged: banded VM kernels, one op-traits table, coverage gate as config, `--label-terminal`, audit-exact eval mode |
| admission, factor series | `atx-equity-strategy-targets factors`; `research/admission` | atx-impl; atx-engine | merged in wave 1 (B1) |
| composition fit | Python fitter `fit_composition_weights.py`; C++ fit verbs behind `--engine-fit` | atx-impl tools; atx-engine `research/composition` | The Python fit is still the fit of record. D2 merged the C++ fit (agrees with Python to 2.2e-16 relative on synthetic cases) and the rule registry (`--list-rules --json`) |
| NAV | `atx-equity-strategy-targets nav` | atx-impl | C1 merged: leverage rule per book, capacity books in the main lockstep. C++ gross calibration (C2) is review-approved, not merged |
| risk model | `atx-equity-strategy-risk` | atx-impl; atx-engine `risk` | COV merged: recipe `atx-cov-v1` (eigenfactor adjustment) and the `.atxcov` container. The default recipe and the store of record are byte-identical |
| statistics of record | Python `nav_summ.py`; C++ verb `atx-research-eval` | atx-impl tools; atx-engine `research` | Python is still of record. The C++ verb's identity part is review-sound, not merged |
| artifact store | `atx-research-store`; SQLite catalog | atx-core `db`; atx-engine `research/store` | SQL1 and SQL2 merged. JSON stays the authority for pinned records through P9 |
| orchestration | wave driver, `research_cycle.py` | `scripts/` | E1 merged. The split into `scripts/cycle/` and the v2 manifest kinds (E2) are in a third fix round |

## 4. What is merged, what waits

Wave 1 (all merged, gate met): E1 runner hardening, T1 canary and CTest registration, A1 and A2 field registry,
S1 marginal-verb speed and Release IC, B1 admission, D1 composition rule table, C1 NAV leverage rule.

| wave-2 lane | what it adds | state |
|---|---|---|
| SQL1, SQL2 | SQLite store core, artifact catalog and CLI | merged |
| A3 | shared vendor-panel source in C++, six fields | merged, post-flip checks green |
| AL-SIG | 13F and Russell stage readers, two fields | merged |
| S2 | IC engine: parallel kernels, flags | merged |
| D2 | C++ composition fit verbs | merged |
| COV | covariance of record, container file | merged |
| C2 | gross calibration in C++, `norm_ppf` fix | approved, not merged |
| AL-COMB | `theme-cov-hedge-v1`, `mom-volman-v1` rules and templates | approved, not merged |
| B2 | C++ statistics verb; deletion of the Python copies | identity part sound; deletion part must be redone after E2 |
| E2 | runner split, v2 manifests, guards against unregistered rules | fix round 3 committed as work in progress, unverified |
| T2 | test tooling | approved, merges last |
| S2R (follow-up) | Release golden test split | Debug done, Release not built |

Tests at the code head, Debug unless noted (root's own runs):

| target | result |
|---|---|
| `atx-impl-tests` | 1,135 passed, 5 skipped |
| `atx-impl-strategy-ic-tests` | 206 passed |
| `atx-impl-strategy-target-tests` | 338 passed |
| engine alpha | 782 passed; Release 779 passed, 1 skipped, 1 failed (section 5) |
| engine factory | 396 passed, Debug and Release |
| mine (golden at 1 and 4 workers) | 44 passed, Debug and Release |
| engine risk (without the 31-minute oracle) | 431 passed, 1 nightly skip |
| engine combine | 243 passed |

Known reds at the head, each with a named owner:
- pytest `test_fields_are_the_rule_applied_to_the_registry`: cleared by E2's merge.
- Release `BookNormalScore.*` (2 tests): cleared by C2's merge.
- Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`: section 5.

## 5. Two findings that limit the numbers

**The book's universe reads 2026-dated inputs.** The book and its role read the atx-db identity bridge
`identity-bridge-v2-pit` (role universe `linked-operating-v3`; 50 of 84 fields). Confirmed in the atx-db source by
three readers: the bridge's primary flag is computed with the 2026 backfill lines included, and those lines are
dropped only afterwards (`identity_table.py:72-85`, `:111-112`, `:187-195`). One reviewer also reads that the role
keeps only primary lines and that the 2026 SEC ticker list changes which links are accepted; that part is not yet
re-verified. The size of the effect is unmeasured. Only the atx-db session can measure it (a rebuild-vs-rebuild diff
of the links that start before 2024). Until the owner has that result or accepts the exposure, no P9 cell runs and
the history read stays closed. Paired tests share the universe on both sides, so relative results are little
affected; the absolute Sharpe and the history read are where it would show.

**`pow` is not the same function in Debug and Release.** Debug calls it in `ucrtbased.dll`, Release in
`ucrtbase.dll`; results differ by 1 ULP on about 0.2% of arguments. It affects `power`, `signedpower` and `^`, and
is latent in `tanh` and `ts_decay_exp`. One book candidate (`res_mom_12_1`) is exposed in principle, but the book's
real outputs were bit-identical across the two builds, because ranking absorbs the difference. The golden digests
were captured on the Debug runtime, so the Release run of that one test fails. No pin has been touched. The interim
ruling makes the pinned test a visible skip in Release and asserts VM = oracle in every build; the lasting fix (an
in-repo deterministic `pow`) moves expected hashes and needs the owner's approval.

Also open: the Release cache path for the `lo3` cache is 251 characters under audit-exact, one over the 250 bound;
cells run in their reference's eval mode and any audit cell gets a short new cache directory.

## 6. Platform gates (plan section 0.4)

| gate | state |
|---|---|
| G-P3 Release adopted | IC half ticked; NAV half waits for wave 3 |
| G-P4 CTest registration | ticked |
| G-P6 no versioned scripts | ticked |
| G-P8 canary | ticked (5 digests, Debug = Release) |
| G-P5 no Python mirror | not yet: Python statistics, the gross-match rule and the fit of record remain |
| G-P7 statistics of record in C++ | not yet: waits for B2 |
| G-P1 one-command wave, G-P2 spec-only kinds | not yet: wait for E2 and wave 3 |
| G-P9 seal, G-P10 store print | not yet ticked |

## 7. Next steps

In order; detail and gates are in `.superpowers/sdd/platform-p9-20261003/status-5.md`.

1. Owner: answer the universe finding (run the atx-db diff or accept the exposure) and choose between the Release
   skip and the re-pin fix for `pow`.
2. Finish the wave-2 merges: C2, AL-COMB, B2's identity part with its TRAIN identity run, E2 after its review, B2's
   deletion redone on top of E2, S2R, T2 last.
3. Wave-2 gate: full suites in Debug and Release, P9-B0 again, parent specs re-pinned, the covariance diagnostic
   D-COV, whole-wave review.
4. Wave 3: typed NAV spec and rule registries (C3), walk-forward refit and the momentum sleeve in the C++ fit (D3),
   catalog dual-write (SQL3), `mv-aim-v1` (COV-MV), `info-clock-rate-v1` (AL-CLOCK), and the pre-registration.
5. P9 cells, in the plan's order without the signals cell (no string cleared the bar): label protocol, theme
   covariance hedge, momentum vol management, info clock, max-Sharpe target, walk-forward, leverage, vol-target on
   the new covariance store. Then the adoption print, the single history read, and the freeze gate.

What moves the book next, by the plan's own estimates: the risk-side rules (theme covariance hedge first) rather
than new signals, since at a Sharpe near 1.85 one more signal adds little.
