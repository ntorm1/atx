# Task T9 review: TRAIN-only composition weight fitter `mv-shrink-0.9-nonneg-v1` + tau_k + NR denominator

Reviewed: root commit `17da002a` (package `review-T9.diff`: `atx-impl/tools/fit_composition_weights.py` +619,
`atx-impl/tools/test_fit_composition_weights.py` +547). I read the diff once, in two passes (tool, then tests).
File line numbers below are file lines (diff line - 14 for the tool, - 639 for the test).

### Spec Compliance

- ✅ **Spec compliant.** Brief steps 1-6, the 2026-09-27 addendum and the global constraints are all implemented.
  Each check is cited below.
  - **Inputs and signs (brief 1).**
    - Library, role, orientations and cache are all SHA-pinned: `:92-97`, `:113-146`, `:152-199`, `:202-222`.
    - Orientation identity is checked against the library order, id, family and dsl sha: `:140-145`.
    - Sign 0 gives weight 0, with null stats, and still reports tau: `:525-527`, `:536-541`.
  - **Exposures (brief 2).** I checked these line by line against `atx-impl/src/strategy_price_exposures.cpp`.
    - Guard: `:243-251` against cpp `:107-147`. Same log-difference operations, same validity rules.
    - Market over ALL instruments: `:252-255`.
    - Windows and first interval: `:262-265` against cpp `:80-91`.
    - Beta: two-pass, pairs need the market, >=126, var>0. `:268-287` against cpp `:164-183`.
    - Vol: >=32, sample SD. `:288-297` against cpp `:185-199`.
    - ADV: full window, absent days add 0, log only if the mean is >0. `:298-300` against cpp `:149-162`, `:377-378`.
    - Refusals: min_names 50, the relative-SD constant floor, clip +-5, the pivot floor on the Jacobi-equilibrated
      Cholesky, and the residual floor of 1e-9 x entry. `:305-336` and `:420-422` against cpp `:230-249`,
      `:290-309` and `:350`.
  - **Factor (brief 3).**
    - Centered tied ranks use the `each_centered_rank` formula (`:339-363`).
    - Ranks are taken over used rows with a finite signal (`:415`). Sign is applied by negation, which is exactly
      antisymmetric.
    - Residualization with one refinement (`:420`), then sum|q| = 1 (`:424`).
    - f = q . r(d+2), where guarded or absent returns contribute 0 (`:402-403`, `:427-428`). The study's forward
      return (`studies/compose_study.py:10-12,25`) also applies the guard. That confirms the brief's "unguarded"
      means "not tripped by the guard", which is how the fitter reads it.
  - **MV fit (brief 4).**
    - The fit is `np.cov(ddof=1)`, then `0.1 S + 0.9 diag(S)`, solve, clip at 0, normalize, and refuse if all
      weights are zero (`:436-453`).
    - Sh is always positive definite when diag(S) > 0, so near-duplicate factors cannot make the solve singular.
  - **Output (brief 5).**
    - Every library id gets a weight (`:549`), plus `library_sha256`.
    - Provenance: rule, lambda, role manifest sha, orientations sha, per-candidate `cache_payload_sha256`,
      `script_sha256`, window, per-candidate mean/sd/Sharpe (`:546-580`).
    - Bytes are canonical: `sort_keys`, `indent=2`, `allow_nan=False` (`:464-465`).
    - Publication is exclusive. There is an early refusal before any work (`:495`) and a no-replace
      `os.link` (`:468-487`).
  - **Streaming and precompute (brief 6).**
    - One candidate is held at a time, and its signal is deleted after the book is built (`:516-520`).
    - The per-date orthonormal bases are built once (`Context`, `:366-407`).
- **Global constraints, checked explicitly.**
  - **TRAIN only.**
    - The role seal requires `end_ns <= 2023-01-01T00:00Z` and the last session before it (`:166-167`, `:174`).
    - The cache sidecar must be `role == "train"` (`:218`), and the cache dir is keyed by the TRAIN manifest sha
      (`:496`).
    - The real TRAIN manifest has `score_end_ns = 1672531200000000000`, exactly the seal, so it passes.
    - In the root's run the last label session is 1672358400e9, which is 2022-12-30.
    - No validation or 2025+ path exists in the tool.
  - **tau_k.**
    - `tau = mean over the T-1 consecutive transitions of sum|q(d) - q(d-1)|` (`:431-433`). q is the neutralized
      sum|q| = 1 book from `book()`, with no drift, and deployment is excluded.
    - The flag is strict `> 0.70` (`:524`, `:564`).
  - **weighted_standalone_turnover.** It is `sum_k w_k tau_k` over the FINAL normalized weights: rows are updated
    at `:540-543` before the sum at `:545`. It is written to provenance and to the stdout summary.
  - **Runner pinned-weights parser** (`strategy_ic_runner.cpp:352-393`).
    - The file must be a JSON object with `schema == atx.dsl-composition-weights/v1`, the matching
      `library_sha256`, and an object `weights`. The fitter's output meets this.
    - No unknown ids and no missing ids. Every value is a finite JSON number >= 0.
    - No duplicate keys at any nesting level: `unique_key_json` scans the nested `provenance` too, and Python dicts
      cannot emit duplicates.
    - The file is at most 1 MiB, enforced at `:582`. The real v1 file is 36.1 KB.
    - `null` values inside provenance are legal for nlohmann.
    - The runner pin is the SHA-256 of the file bytes, which is the `sha256` in stdout.
  - **Neutralization consistent with T3 price-risk-v1.** See brief 2 and 3 above. The only differences are
    solver and summation rounding, plus scaling to 1 instead of the entry gross, which is a constant factor.
  - **Timing.** `strategy_nav_replay.hpp:13-15` says: decide at d, fill at the close of d+1, first return row
    d+2, decisions `[begin, end-2)`.
    - The fitter's decisions are `[score_begin, score_end-2)` (`:370`).
    - `PricePanel.returns[t]` is the interval (t-1, t] (`:241-251`), and `forward = returns[begin+2 : end+2]`
    (`:402`). So f(d) uses close[d+2]/close[d+1]-1, which matches the NAV replay. ✅
    - Known difference: the NAV replay marks actual (unguarded) closes and carries absent names at their last
      mark. The fitter zeroes guarded or absent intervals, as the brief and the study specify.
  - **Does the TRAIN role manifest SHA appear in the weights JSON?** Yes. It is under
    **`provenance.role_manifest_sha256`** (`:567`). The file also carries `provenance.orientations_sha256` (the
    pinned orientations.json file SHA, `:569`) and `provenance.library_sha256` (`:566`). There is no top-level
    key and no key named `train_manifest_sha256`. See M2.
- ⚠️ **Cannot verify from the diff alone:**
  1. **Real C++ acceptance of the real file.** The test uses only a Python port of `composition_weights()`
     (test `:286-304`). No runner run has consumed `build-equity/mega-weights-v1/composition_weights.json`. I
     grepped build-equity for sha `cdfe0dc2…` and found no hits.
     - Root check: run the IC runner with `--plan-only --composition-weights build-equity/mega-weights-v1/composition_weights.json --composition-weights-sha256 cdfe0dc2e95b57ee52c303432ea2401788d121cb53977c0512743bf2208ce7fd`.
     - This is cheap because weights are validated before any payload is loaded (`strategy_ic_runner.cpp:876-878`).
  2. **T1 fix coordination.** Both of these live in T1's lane, not T9's.
     - (a) The TRAIN-binding key: see M2.
     - (b) The T1 C1 fix adds VM/engine identity to the cache key. The fitter's cache validation (`:202-222`)
       must adopt the same identity field or directory layout in lockstep. If the change is a directory-layout
       change, the fitter fails loudly. If the change is only a new sidecar field, the fitter would silently
       accept entries the runner would refuse.
  3. **Member mask.** The fitter uses `member & present` (`:371`; report decision 1), where the brief literally
     says "member".
     - This matches the runner's effective blend support and the combined artifact's `member_semantics`.
     - `neutralize_price_risk` has no caller in `atx-impl/src` yet (grep). T4 must pass that same effective mask
       for the fitter's used-row set to equal the book's. Controller: ratify the choice and carry it into T4.
  4. **Performance bound (brief 6: < 150 s at 100 candidates).**
     - The implementer's synthetic figures are plausible. On this host I measured a synthetic compute floor of
       about 0.75 s per candidate: ranks 0.40 s, two projections 0.20 s, sha 0.06 s, the rest small.
     - The root's real run is slower: 48 candidates in 98.35 s, context 26.86 s, and 0.78-2.19 s per candidate
       with a median of 1.47 s (`mega-weights-v1-run/stderr.log`).
     - That extrapolates to about 173 s at 100 candidates and about 320 s at 200. It is not proven over the
       bound on an idle host. See M1.
- Real-run structural sanity: I read only the root's TRAIN output.
  - `neutralization_refused_decisions == []`, and used rows are 2718-2988.
  - `flat_decisions` is 0 for all 48 candidates, and all 48 are `fitted`.
  - tau ranges from 0.024 to 0.184, with none flagged. `weighted_standalone_turnover` is 0.0708, and 30 of 48
    weights are nonzero.

### Strengths

- **A faithful port of price-risk-v1, proved against a literal loop port rather than self-consistency.** The
  test `Exposures` covers:
  - clipped windows, a genuine split (kept), an uncorroborated spike, and |log adj| > 1.5;
  - absent gaps, zero volume, and too few beta pairs;
  - identical NaN sets, with rtol 1e-11.
- **Thorough pinning.**
  - Role payload receipts are re-verified (`:191-199`).
  - The cache payload SHA is checked (`:221`).
  - The orientations identity is checked row by row.
  - The TRAIN seal is enforced on both the role and the sidecar.
  - Loud `FitError` refusals are tested (test `:498-543`).
- **An independent reference for the end-to-end result.** The slow reference in the test (`:107-156`) uses
  per-date literal loops, `lstsq` and explicit moments. It agrees with the fitter on weights, tau and factor
  stats to rtol 1e-9. The hand cases for ranks, MV with clip, and tau are exact.
- **Well-handled degenerate cases.**
  - Degenerate candidates are dropped from the solve with an explicit status, not a singular-matrix crash.
  - Shrinkage keeps Sh positive definite.
  - Neutralization-refused decisions are recorded in provenance, not hidden.
- **Streaming holds memory flat.** Peak RSS on the real run was 548 MB, as the report predicted.
- The synthetic suite ran 16/16 with `-W default` in 3.9 s, with no warnings (I re-ran it to check output
  cleanliness only).

### Issues

#### Critical (Must Fix)

None.

#### Important (Should Fix)

None.

#### Minor (Nice to Have)

- **M1. The report's performance guidance is not borne out by the real run**
  (`task-T9-report.md:55-59,132`, against `build-equity/mega-weights-v1-run/{stdout,stderr}.log`).
  - The report estimated about 55 s for v1 (48 candidates). The real run took 98.35 s.
  - The report's 300 s cap for 200 candidates would probably be exceeded at the measured rate of about
    1.47 s per candidate plus 27 s of context. The likely extra cost is cold 52 MB reads and host contention.
  - Fix: size the bounded-runner `--seconds` from the real rate (about 30 s + 1.6 s x K). If v3 is at or above
    100 candidates, either re-measure on an idle host or add an optional candidate-level process pool, merged by
    index so the output bytes stay deterministic. Per-candidate work is independent.
- **M2. The TRAIN-binding key name may mismatch the pending T1 fix** (`:567`, `:569`).
  - The T1 review fix text (`task-T1-review.md:161-165`) says to check `train_manifest_sha256`. The fitter emits
    `provenance.role_manifest_sha256`.
  - In the runner's own artifacts, `role_manifest_sha256` means "the role this artifact was scored on", while
    `train_manifest_sha256` means the TRAIN binding (`strategy_ic_runner.cpp:245-247`). A runner fix that reads
    the fitter's key by runner convention could compare it against the validation role.
  - Fix: add an explicit `provenance.train_manifest_sha256`, or a top-level one, now. Also consider
    `orientations_artifact_sha256` to match the runner's name for the same value. Then have the T1 fix read
    exactly those keys.
  - Otherwise the existing `cdfe0dc2…` file has to be regenerated once the runner requires the binding. That is
    cheap, but it changes the pin.
- **M3. Some failures bypass `FitError`.**
  - KeyError on a manifest or receipt missing a key (`:157-158`, `:193`).
  - AttributeError on a non-dict library or orientations row (`:120`, `:141`).
  - OSError from `os.link` on a filesystem without hard links (`:483`).
  - Each of these escapes `main()`'s handler as a traceback. The exit is still nonzero, so this is only
    diagnostic quality.
  - If the write or fsync inside `with stream:` fails (`:478-481`), the `.pending` file is left behind, and every
    later run refuses until someone removes it by hand. The refusal is loud, but the message does not say that.
- **M4. `SHRINK_LAMBDA` is recorded but never used** (`:58` against `:437`).
  - Provenance says `lambda: 0.9` while the solve hard-codes 0.1/0.9, so an edit to one silently disagrees with
    the other.
  - Fix: add `assert SHRINK_LAMBDA == 0.9` next to `shrink_solution`, or derive the literals from one table.
    `(1 - 0.9)` is not bit-equal to `0.1`, so do not derive it arithmetically without re-pinning.
- **M5. `m` means two different things in `PricePanel.exposures`.** It is `len(cols)` at `:266` and then the
  market matrix at `:278`. This is harmless today but a trap for any later use of the row count. Rename one.
- **M6. Test coverage gaps.** Each would need a single assertion:
  - no refusal fixtures for a sidecar identity mismatch (`dsl_sha256`, `role_manifest_sha256` or geometry,
    `:212-217`);
  - none for an orientations identity mismatch (`train_manifest_sha256` or candidate order, `:134-145`);
  - none for a stale `.pending` (`:476-477`);
  - no check on the tau flag boundary: exactly 0.70 must not be flagged.

### Assessment

**Task quality:** Approved

**Reasoning:** The fitter implements the preregistered rule, the tau_k/NR-denominator addendum and TRAIN-only sealing
exactly as specified. Its price-risk-v1 neutralization is a verified faithful port, its factor timing matches the NAV
replay (d → d+1 fill → d+2 return), and its output meets the runner's pinned-weights parser. What remains is
coordination with the T1 fix (the binding key name and the cache identity), real C++ acceptance of the real file,
and sizing the runner cap from the real per-candidate rate.
