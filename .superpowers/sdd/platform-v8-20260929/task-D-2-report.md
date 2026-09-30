# Task D-2 report: `exposures` verb (contract K2)

Lane D, worktree `C:/atx-wt/pool-4`, branch `feat/platform-v8-d-20260929`. Brief: `task-D-brief.md` D-2.
I did not build the C++ or run it (lane rules). Every C++ claim below comes from reading the code. I did run the Python pieces on synthetic data.

## What was built

### `strategy_price_exposures.{hpp,cpp}`: generic pieces the verb is built from

- **`session_interval_returns(panel, t, scratch, out)`**
  - Returns the interval t = (t-1, t] return for every name, NaN when the interval is invalid.
  - Uses `compute_price_exposures`' own kernel and guard (`interval_returns`).
- **`BasisRefusal` and `basis_refusal_id`**
  - The fitter's refusal ids: `too-few-usable-names`, `constant-exposure`, `ill-conditioned-exposures`.
- **`neutralization_basis(exposures, rows, cfg, scratch, basis)`**
  - Standardizes z exactly as `neutralize_target` does, and refuses in the fitter's order.
  - Computes Q of the Householder QR of [1, z] in LAPACK's convention (dgeqr2 + dorg2r, operation for operation), so it matches numpy.linalg.qr's Q to rounding, signs included.
  - The pytest checks this convention against numpy on the fixture: max |diff| is 4e-15.
  - `NeutralizeScratch` gains a `qr` work vector.
- Nothing already in the file changed. The NAV and target-replay paths do not call the new functions.

### `strategy_exposures_verb.{hpp,cpp}` (new)

**`export_decision(panel, member, cfg, d, scratch, basis, forward)`** exports one decision:
- Used names are `member && present && ok`, where ok comes from the exposures at d.
- `basis` (names x 4): Q on the used names, 0 elsewhere, and all 0 on a refusal.
- `forward` (names): the interval d + 2 return, NaN when invalid.
- The exposure scratch's session ring (D-1) is bound once per export.

**`run_exposure_export` / `dispatch_exposures`** implement the verb:

```
atx-equity-strategy-targets exposures --role R/manifest.json --role-sha256 SHA --output NEWDIR [--max-bytes 1400000000]
```

- **Order of checks and loading:**
  - The pinned manifest is read and checked against `--role-sha256` first.
  - The seal is checked from `score_end_ns`, before any payload is read: `research_window.hpp` `kSealBeginNs`, error text naming `research-window-v2`.
  - The engine `read_strategy_role` then loads the role. The verb re-checks the manifest SHA and `is_sealed(last session)`.
  - Presence comes from `panel.in_universe`.
- **Decisions:** `[score_begin, score_end - 2)`, the fitter's window.
- **Output:** exclusive directory, all little-endian:
  - `basis.f64`: decisions x names x 4, `[d][i][k]`. Name i is used at d iff `basis[d][i][0] != 0`, because Q's intercept column is -1/sqrt(m) on used rows.
  - `forward_returns.f64`: decisions x names, NaN when invalid.
  - `manifest.json`, written last. Schema `atx.price-risk-exposures/v1`, contract K2. It holds:
    - the role pin, window id, `ids_sha256`, geometry and window session ns;
    - the price-risk constants;
    - `semantics`, plus the fitter's exact `CONTEXT_SEMANTICS`;
    - `used_rows` per decision (0 when refused, as the fitter's array has);
    - `refused` as `[{decision_index, reason, used_rows}]`, as the fitter records it;
    - the file SHAs, shapes and layouts.
- **Budget:** `--max-bytes` is checked against the presence copy, the ring, per-name rows and slack. The rest goes to the role reader.

### `tools/equity_strategy_targets.cpp`

- New verb `exposures`.

## Tests

`atx-impl/tests/strategy_exposures_verb_test.cpp`:

- **`Exposures.FitterFactorsEqualTo1e12`**
  - Fixture: 170 x 60 synthetic panel, 38 decisions. It includes absent rows, nonmembers, a guarded adjusted-only jump, and one refused decision (too few names).
  - The fitter's factor series (`Context.book`, `factor_returns`, re-implemented from its code) is computed from the export. For a noise signal and a beta-loaded signal, it equals the goldens to 1e-12.
  - The goldens are `fit_composition_weights.py`'s own output on the same fixture.
  - Also checked:
    - live days and per-decision used rows;
    - the refusal (`too-few-usable-names`, 42 rows);
    - six rows of Q against numpy's, to 1e-12;
    - Q orthonormal on the used rows;
    - the forward NaN on the guarded interval;
    - ring and stateless exports are bit-identical.
- **`Exposures.BasisIsTheOlsProjectionAndRefusesLikeTheFitter`**
  - The projection residual, rescaled to the entry gross, equals `neutralize_target`'s result to 1e-12.
  - The first column is -1/sqrt(m).
  - Each of the three refusals is hit, and geometry errors are rejected.
- **`Exposures.VerbWritesTheK2Layout`**
  - Runs on an engine-conformant synthetic role (420 daily sessions from 2018-06, score_begin 383, 35 decisions).
  - The files hold exactly `export_decision`'s bits, and the manifest's SHAs, shapes, window, used rows and refusals are correct.
- **`Exposures.VerbRefusesBeforeAnyOutput`**
  - Refusals checked: a wrong pin, a manifest whose `score_end_ns` passes the seal (message names `research-window-v2`, no payload opened), an existing output, and usage errors.

`atx-impl/tools/test_exposures_export.py` (pytest, synthetic data only; results: 2 passed, 1 skipped):

- **`test_cpp_goldens_are_the_fitters_output`**
  - Extracts the golden arrays from the C++ test file and recomputes them with the fitter (`PricePanel`, `neutralization_basis`, `Context.build`, `book`, `factor_returns`).
  - They agree to 1e-15, so the C++ goldens cannot drift from the fitter.
- **`test_householder_convention_is_numpys`**
  - The C++ QR algorithm, emulated in Python, matches numpy's Q on every unrefused decision (1e-13).
- **`test_verb_export_reproduces_the_fitters_factors`** (runs only when `ATX_EQUITY_TARGETS_EXE` is set)
  - Writes the same engine role and runs the built verb.
  - Builds a fitter `Context` from the K2 files, which is what the fitter follow-up reads.
  - Requires the refusals, used rows, live days and factor returns to equal the fitter's own context, to 1e-12.
  - I validated the reader side offline, with a Python emulation of the export: max |diff| is 2e-18.

## How root verifies

- **Build targets:** `atx-impl-tests`, `atx-equity-strategy-targets`.
- **gtest filter:** `Exposures.*:LogRing.*:StrategyPriceExposures.*:StrategyPriceNeutralize*`
- **pytest:**
  ```
  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_exposures_export.py
  ```
  Set `ATX_EQUITY_TARGETS_EXE=<build>/bin/atx-equity-strategy-targets.exe` to include the end-to-end check.
- **Real role smoke (TRAIN role lo1; it ends 2022-12-30, before the seal):**
  ```
  build-equity/bin/atx-equity-strategy-targets.exe exposures --role build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --role-sha256 3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809 --output <NEW>
  ```
  - Expected: 754 decisions x 5627 names. `basis.f64` is 135.8 MB and `forward_returns.f64` is 33.9 MB.
  - `refused` should equal the `neutralization_refused_decisions` in the accepted fitter output (`mega-weights-v71-ew` admission.json) for the same role.
- **Identity:** no existing output changes. The verb is new, and the new functions in `strategy_price_exposures.cpp` are not called by the NAV or target replay.

## Deviations

- `--role-sha256` is required. K2's argv names only `--role R`, but every pinned-role tool in the tree requires the pin (house rule).
- `forward_returns.f64` keeps NaN for invalid labels. The fitter maps NaN to 0 on read; this is declared in the manifest.
- There is no separate used-mask file. Used is `basis[d][i][0] != 0` (declared in the manifest), which keeps to the K2 file list.
- The Python neutraliser is not deleted, as the brief says (only after the fitter reads the export and `admission.json` is unchanged).

## Cross-lane edits

- `atx-impl/CMakeLists.txt` (lane E): one line appended, `target_sources(atx-impl-core PRIVATE src/strategy_exposures_verb.cpp)`, after the existing `stage_equity_mine.cpp` append.
- `atx-engine/include/atx/engine/data/research_window.hpp` (W0-1's file): W0-1 had not reached this branch, so I created it with exactly the content the W0-1 brief specifies. When merging, take W0-1's version if the two differ. The verb uses `kSealBeginNs`, `kResearchWindowId` and `is_sealed`.

## Open risks

- The engine role reader's own seal is still `kSeal` = 2025-01-01 until W0-1 lands. The verb refuses any role whose `score_end_ns` passes 2024-01-01 before reading payloads, so the hidden-data rule holds either way.
- The verb uses the engine reader's full contract (membership recipe, `declared_output_bytes`, score_begin >= 383). lo1 and lo3 conform, as I checked from the lo1 manifest. A role built outside `prepare_recent_research` may not.
- The golden comparison relies on the C++ fixture reproducing the Python panel. FMA contraction by the compiler could move values by an ulp, which is far inside 1e-12. The used-row sets and guard decisions have no knife edges in this fixture.
