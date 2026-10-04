# Task D2 review (wave 2, slot 5): composition fit in C++, `--engine-fit`, `params_schema`, CM-5, pair-cache re-pin

| | |
|---|---|
| Lane / pool | D2, `C:/atx-wt/pool-20`, `feat/p9-d2-20261003` |
| Reviewed | `1239a5ff..9fad6907` (code `d61f73c2`, `56661dc8`, `5c844702`, `4b463071`) |
| Read | brief-D2, wave2-carry (All lanes, D2), resume-rulings D2-1..3, progress D2-4..6, task-D2-report, evidence-D2, full diff |
| Built | nothing (no doubt needed a build: the open questions were settled by reading and synthetic Python probes) |
| Real data | none opened; no trial, no ledger append (diff has neither) |

**Overall: CHANGES_REQUIRED** (2 Required, 8 Suggested).

- Spec verdict: compliant with brief, carry and rulings D2-1..3, except R-2 (a combination the report lists as supported cannot run). D2-5 and D2-6 are ordered fix-round items, not re-litigated here.
- Quality verdict: high. The C++ fit reproduces the Python formula for formula (table below). The P8 claim holds bit for bit. CM-5, `params_schema` and the tests are sound. The two Required items are a tripwire coverage hole and a fail-closed but unrunnable combination.

## Required

| # | where | problem | required fix |
|---|---|---|---|
| R-1 | `atx-impl/src/strategy_marginal_pair_cache.cpp:37-41`, `atx-impl/tests/strategy_marginal_ic_test.cpp:699-705`, `atx-engine/src/combine/marginal_rank_ic.cpp:15,160` | The pair cache's source pin is a tripwire: the cache key holds only `kPairwiseRowCorrelationVersion` and never the digest. After P8 every pair rank comes from `combine/group_rerank.hpp::for_each_centered_rank`, and that header is not in `pair_sources`. The test's premise "no include closure: the residualisation and HAC sources never touch a pair" is now false. A later edit of `group_rerank.hpp` would move pair bits with no failing pin and no version bump, so a stale `pairs1_*` cache would be read as valid. `group_rerank.hpp` is the composition's own rank helper, so D3 / AL-COMB are likely to edit it. Today's claim is true (see "P8 / pair-cache claim"); the hole is in the pin's coverage. | Add `atx-engine/include/atx/engine/combine/group_rerank.hpp` to `pair_sources` (3 paths). Re-pin by the recipe with no version bump: bits unchanged, S2-1 precedent. My recompute at `9fad6907` gives `df589a889927852b86815920e6ed59a37ea0b44d026d9ffb97ae73f2aa37c7bc`, as a cross-check only; the lane computes its own. Update `ASSERT_EQ(id.sources.size(), 2U)` and the test comment. Nothing else reads `marginal_pair_cache_identity().sources`, so no output byte moves. |
| R-2 | `atx-impl/tools/fit_composition_weights.py:2609,2615-2643`, `atx-impl/tools/composition_resid.py:179-214`, `atx-impl/tools/composition_engine_fit.py:45,57,118` | `--engine-fit` with `--theme-resid --theme-resid-parent` cannot run, yet the report lists `--theme-resid` as supported. R-11 cells always pass the parent: the `scripts/specs/v8/r11.json` template derives it (finding R6B-O-5). `composition_resid.apply` runs inside `compose()`, so `parent_check` runs again on the C++-valued second pass. `provenance.engine_fit` is added after that check and is not excluded by it. Synthetic probe (lane stand-ins, `aim_world`, std-aim): (a) an engine-fitted parent makes every resid child refuse, whether the child has the flag or not: `differ: ['provenance.engine_fit']`. (b) A C++ value inside P12, which the real exe gives for ic-shrink ICs (2.3e-15), refuses on the second pass after the C++ "agreed": `differ: ['provenance.blend_in_sample_TRAIN_diagnostic', 'provenance.candidates', 'provenance.themes', ..., 'weights']`. Both refusals blame "a fitter-affecting change". This is fail-closed (no wrong bytes), but the claimed combination is unrunnable and the refusal comes only after both exes ran. | Minimal fix: refuse `--engine-fit` together with `--theme-resid-parent` in `check_args`, before any compute, with a message naming the reason. Also refuse a flag-absent child whose parent carries `provenance.engine_fit`, or exclude that key in `parent_check`. Correct the report's and `REFUSED`'s support list. Alternatively make `parent_check` flag-aware: compare the pass that is written, pop `provenance.engine_fit` on both sides, and use a P12 comparison when the two fits differ in the flag. Add a test for each case. |

## Suggested

| # | where | problem | suggested fix |
|---|---|---|---|
| S-1 | `atx-impl/tools/composition_engine_fit.py:185-189` | `compare_admission_csv` has a NaN hole. If either float cell parses to NaN, `rel` is NaN, so `rel > tolerance` is false and `max(worst, nan)` keeps `worst`. A NaN-vs-finite cell therefore passes silently. Separately, a row with more cells than the header is truncated by `zip` (B1's carried minor, re-implemented here). | Require non-finite cells to match exactly, as `compare_values` already does (:302-305). Also check row length against the header. |
| S-2 | `composition_engine_fit.py:194-197,207,333-334` | Several failures escape as raw Python exceptions instead of a `FitError` refusal: `subprocess.TimeoutExpired`, a missing `admission.csv` / `composition_fit.json`, invalid JSON, a `KeyError` in `kernels`. Nothing is published, but the result is a traceback, not the fitter's refusal. | Wrap the reads and the subprocess call into `error`. |
| S-3 | `composition_engine_fit.py:405-413` | Under the flag the numbers come from the C++ exe. `provenance.engine_fit` records output SHAs and tolerances but not the producing code: no SHA-256 of `atx-research-composition` / `atx-research-admission`, and no fingerprint of `verify` / `kernels` / `check_written`. A `kernels` mapping fix would move written values with no identity moving. | Add the two exe SHA-256s and `module_fingerprint(composition_engine_fit, ("verify", "kernels", "check_written"))`. |
| S-4 | `atx-impl/tools/composition_ic_shrink.py:53,184`; report "Concerns" | Under `--engine-fit`, ic-shrink writes the C++ ICs into `ic_shrink.members[].ic`. `admission.json` keeps the Python `train_mean`; the two differ by about 1e-15. `provenance.ic_source` still says the ICs are "admission.json candidates[].train_mean". | **PM ruling needed**: under the flag, write B1's C++ admission table too; or keep the Python ICs for ic-shrink; or amend `ic_source` under the flag. |
| S-5 | `composition_resid.py:214`, `:83-84`, `:226`; `test_composition_resid.py:625-636`; `scripts/specs/v8/r11.json` | A pre-D2 parent is refused, which is correct: probe output `differ: ['provenance.std']; a fitter-affecting change lies between the two fits`. The message does not name the cause (the CM-5 / D2-2 key rename) or the remedy (refit the parent). The "legacy parent" test only renames the top-level key, so a real pre-D2 shape (`provenance.std.module_sha256`) is untested. Texts still say "only provenance.script_sha256 ... may differ". | Detect `module_sha256` in a parent block and refuse with "pre-D2 (CM-5) parent: refit it with this fitter". Add that test. Update the three texts. |
| S-6 | `fit_composition_weights.py:431-438`; `composition_rules.py:136`; the `attach` functions | On CM-5 coverage: the fitter's `OUTPUT_PRODUCERS` fingerprint is `code_fingerprint.fingerprint`, which hashes import statements but not cross-module reads (FD-2). Edits to `era_pool`, `composition_two_speed` or `composition_engine_fit` that move output bytes therefore move no fingerprint. That was also true of the old whole-file `script_sha256`, so it is not a regression. The module `PRODUCERS` omit the `attach*` writers and `composition_resid.apply`'s additions, for example `PARENT_CHECK`. | Use `module_fingerprint`-style cross-module reads for the fitter, and add the `attach*` / `apply` writers to `PRODUCERS`. |
| S-7 | `evidence-D2/`; report "How root verifies" | Two gaps: (a) evidence has no gtest log for ResearchComposition 10/10 or combine 243/243; (b) `RealExecutables` is the only real C++-vs-Python run through the fitter, and it is skipped unless both env vars are set. It is not in "How root verifies". | Root runs `ATX_RESEARCH_ADMISSION_EXE=... ATX_RESEARCH_COMPOSITION_EXE=... python -m pytest -q -p no:cacheprovider atx-impl/tools/test_composition_engine_fit.py` and reads `1 passed` for `RealExecutables`. |
| S-8 | `composition_engine_fit.py:307` | The ruled P12 measure, `|a-b| / max(1,|a|)`, is effectively an absolute 1e-12 for small quantities: sleeve covariances of about 1e-6 and ICs of about 1e-4 are held only to about 1e-6 and 1e-8 relative. The decision-bearing shares and weights are near-relative. This is information only; the tolerance is the ruled one. | Optionally also record the worst `|a-b|/|a|` per block in `provenance.engine_fit`. |

## Ordered fix-round items (PM rulings; tracked, not re-reviewed)

- D2-5: `scripts/tests/test_no_python_mirror.py:70-72`, the `horizon_stats.py` row, still names lane `D3`. The ruling says P10, as a named G-P5 exception.
- D2-6: lines over 100 columns in the new engine files (`composition_cli.cpp`, `fit.cpp`, headers, `ew_theme_std.cpp`, `research_composition_fit_test.cpp`). Lines moved unchanged stay as they are.
- D2-4: WIP `d61f73c2` merges as is.

## Fit fidelity: C++ against the Python it mirrors

Every row was checked expression by expression.

| step | Python | C++ | same bits? |
|---|---|---|---|
| tier re-grades | `apply_regrades`: `ids.index` (first occurrence), status strings | `apply_tier_regrades`: `std::find`, same strings and order | yes |
| tier weights | `tier_weights`: `total[t] += s` in member order, `s/(T*total)` (int T), cap `1/(2T)` | `theme_gain_weights`: same loop, `scores/(count_t*total)`, `1/(2*count_t)` | yes (2T exact) |
| aim | `s*g`, `a*g` | `score*gain`, `share*gain` | yes |
| member cap | `member_cap`: bound `cap*(1+tol)`, excess per theme, receivers' `sum(base)` sequential, `w += excess*base/mass`, themes `sorted(excess)` | `cap_across_groups`: same expressions, groups in index order = ascending names (fit numbers themes by `std::set`) | yes. The trace only copies (`capped` ascending = `sorted(over)`) |
| ic-shrink | `shrunk_shares`: sequential theme totals, `/count`, `.5*m + .5*x`, floor, mass, equal share `1/n` | `group_shrink_shares`: identical; the fit's `theme_mean` uses the same sequential `/count`; `equal_share_themes` sorted on both sides | yes, given equal ICs. The ICs are the C++ admission mean against numpy's mean (2.3e-15): P12 |
| ERC shares a_k | `parent_shares`: `u/total[t]`, sequential | `within_theme_shares`: same | yes |
| sleeves | `r = r + a_k*cols[k]` over masked decisions | `r[i] = r[i] + share*series[k][keep[i]]` | yes (no FMA: the equity presets have no `/arch:AVX2`) |
| centring | `series.mean(axis=1)` = numpy pairwise / n | `numpy_pairwise_sum(r)/n`; port pinned against numpy for n = 5..1001 and 256-wide rows (re-run, passes) | yes |
| covariance | `np.dot` (BLAS) `/ (n-1)` | sequential dot `/ (n-1)` | **no**, last bits only: the non-exact fixture case plus P12 (stated in `fit.hpp`) |
| ERC solve | `erc_shares` (port of `group_erc_shares`) | `group_erc_shares` unchanged, sweeps 10000, dispersion 1e-10 | same algorithm. Inputs differ only through the covariance |
| tsmom | `theme_masses` (w>0, member order), `r + (w/W)*m[k]`, then 0 outside TRAIN; sequential trailing sum; `masses` | `theme_tsmom_schedule`: same expressions and order; weighted themes ascending = `order`; `theme_tsmom_masses` unchanged | yes, given equal weights |
| resid order | `theme_order(weighted, registered)` | `theme_resid_order`: registered order restricted to weighted themes; frozen prefix enforced | yes |
| kernels moved from atx-impl | `strategy_ic_{shrink,theme_erc,theme_resid,theme_tsmom}.cpp`, `strategy_two_speed.hpp` | engine copies verbatim (diffed), same `/O2 /Ob2 /clang:-finline` Debug flags as the IC TUs before; only `+ - * / sqrt` | yes: IC runner identity unaffected |

## Input classes the three synthetic cases do not cover

| class | Python | C++ | under `--engine-fit` |
|---|---|---|---|
| rank-deficient sleeve covariance (identical sleeves, or more themes than n-1) | `erc_shares` needs only a positive diagonal; the barrier keeps x > 0 | same CCD | shares agree under P12. A dispersion near 1e-10 can pass on one side only: a refusal, not a fallback |
| zero-variance theme (sleeve all 0 over TRAIN) | refuses ("variance not > 0") | `group_erc_shares` refuses | Python refuses in pass 1, before any exe. A constant non-zero sleeve leaves the same ±ulp residue on both sides (same pairwise mean) |
| single-name theme | share 1, always capped at 1/(2T); every theme single-name: infeasible, refuses | identical | same bits. Not in the fixture, but on the covered cap path |
| missing sessions | flat cells are 0 and still counted in n; a member with no live TRAIN decision is never admitted | same; ic-shrink refuses such a member explicitly | `check_factor_series` requires equal sessions. TRAIN bounds are the same constants (`FIT_BEGIN_NS = rw.TRAIN_BEGIN_NS`) |
| ≥ 8 members per theme | every per-theme sum is a Python loop, not a numpy reduction | sequential | same bits |
| two themes spilling in one cap pass | `sorted(excess)` | index order = ascending names | same bits |
| tsmom trailing sum ≈ 0 over an ERC parent | sign of its own T | C++ weights differ by ulps, so T can flip | `themes_off` / masses are compared exactly: refusal. T exactly 0 (window outside TRAIN) is 0 on both sides |
| NaN / ±inf factor | NaN zero-filled; inf gives a non-finite covariance and a refusal | same; JSON `null` for NaN fails `compare_values` on type | refusal |

## P8 / pair-cache claim ("the P8 change keeps every bit")

Verified true at `9fad6907`:

- Old `centred_tied_ranks`: `std::sort` of `pair<f64,usize>` (`sort_ranked`), then `(b + (e-1)) / (2.0*(n-1)) - 0.5` with the denominator hoisted. It returned early for fewer than 2 names.
- New `for_each_centered_rank`: the same type, the same `std::sort`, the same operations in the same order. A `/` then `-` cannot contract.
- The sort result is unique because the indices are unique. Writes happen in sorted order on both sides. Fewer than 2 names: sorting is a no-op and `out` stays NaN.
- Same TU, same flags.

Both pins recompute from git blobs:

| commit | recomputed digest | pinned digest |
|---|---|---|
| base | `88f97f24...` | equal |
| head | `7b9a6dd8...` | equal |

The pins are computed, not hand-typed. The remaining problem is the coverage hole, R-1.

## D2-1 (`--engine-fit`)

- C++ values are written only after all three checks pass:
  - factors: absolute 1e-12, flat days exact;
  - admission: statuses and ranks byte for byte;
  - fit: every key, string, integer and list length exact; floats under P12.
- `compare_values` compares every value that `kernels` consumes. `check_written` requires the written weights to be bit for bit the C++ final weights.
- Any refusal raises before `publish_directory`. There is no fallback to either side.
- Flag absent, the only byte changes are the ruled CM-5 / D2-2 keys:
  - `compose()` with `engine=None` runs the old code;
  - `matrix` is hoisted and has the same values;
  - `gains` float conversion is hoisted and gives the same floats;
  - the `enumerate(starts)` loop is unchanged in effect.
- IC runner default path:
  - the over-cap `--plan-only` omits K1 rows (carried D1 minor; `nlohmann::json` sorts keys, so within budget the bytes are unchanged);
  - `--list-rules` is honoured only in argv[1..2];
  - the `--list-rules` listing changes only `params_schema` (ruled D1-PSCHEMA).

## `params_schema` (D1-PSCHEMA)

- Each row is closed: `additionalProperties: false`, no `required`, `rule` as a typed const, the registered constants as typed consts.
- `ParamsSchemaConstantsAreTheRegisteredOnes` pins each const and type to its C++ constant.
- The re-pinned fixture follows from the code. Parsing the eight raw strings in `strategy_ic_rules.cpp` gives exactly the eight `params_schema` objects in `composition_rules_list.json` (script output "ALL EQUAL"). Only `params_schema` hunks changed in the fixture.

## CM-5 / D2-2

- `provenance.{std,std_identity,ic_shrink,theme_erc,theme_tsmom,resid}.producer_fingerprint` is the AST closure of the writer plus the fingerprints of cross-module reads.
- The tests show the fingerprint stays the same under a comment edit and moves under an own-constant edit and a cross-module constant edit (`CAP_TOLERANCE`).
- The fitter outputs carry `producer_fingerprint(("fit","fit_prior"))`. A cosmetic edit moves no output byte (store test with real edited copies). The summary still prints `script_sha256`.
- A pre-D2 `--theme-resid-parent` is refused (probe), with a generic message (S-5).

## Tests

- The red pytest (`test_composition_theme_tsmom.py`) was fixed at its cause: the constants moved to the engine header, and the test now reads that header. The assertion is unchanged.
- The fixture gtest is a real contract: today's Python, dyadic inputs bit for bit, plus one non-dyadic case under P12.
- Closed forms are tested: F-15 2×2 ERC, the covariance by hand, the cap trace.
- Weakness: the `--engine-fit` end-to-end tests use Python stand-ins, so they test plumbing and refusals only. The real-exe test is environment-gated (S-7).
- The carried D1 minors are all addressed. The 13-theme pins are only commented, as allowed.

## Evidence (this review)

```
cd C:/atx-wt/pool-20; PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider \
  atx-impl/tools/test_composition_engine_fit.py atx-impl/tools/test_composition_fit_fixture.py scripts/tests/test_no_python_mirror.py
............s.........
G-P5: 37 mirrored rule function(s) in 14 row(s) left for P9 lanes
..                                   [100%]
23 passed, 1 skipped, 14 subtests passed in 14.34s
exit=0   (the skip: RealExecutables, no exes built)
```

Probes: scratchpad scripts on synthetic `aim_world` only, deleted after the run.

- R-2 probe: parent plain 0; parent engine 0; engine child of a plain parent 0; engine child of an engine parent REFUSED (`provenance.engine_fit`); plain child of an engine parent REFUSED (`provenance.engine_fit`); engine child with C++ values +1e-14 REFUSED (weights / diagnostic / themes ...).
- S-5 probe: pre-D2 parent REFUSED, `differ: ['provenance.std']`.
- Lane builds (receipts): tag a built the composition library and its tests at clean `a17319fb`, and no engine composition file changed afterwards. Tag d built the IC and target exes and tests with 12 dirty entries that became `56661dc8`. No C++ changed after `56661dc8`. `atx-impl-tests` was never built.

## What root must check at merge

1. Build `atx-engine-research-composition-tests`, `atx-research-composition`, `atx-equity-strategy-ic`, `atx-impl-strategy-ic-tests`, `atx-impl-strategy-target-tests`, `atx-engine-combine-tests`, `atx-research-admission`, and `atx-impl-tests` (never built by the lane).
2. Run the anchored filters in the report. After the R-1 fix, `MarginalIc.PairCacheSourcesPinned` must pass with 3 sources.
3. Run the `RealExecutables` pytest with both exe env vars set (S-7).
4. X-5 flag-absent identity with exactly the four CM-5 / D2-2 substitutions in the report.
5. Flag-present: admission files byte-identical, and weights equal to flag-absent plus `provenance.engine_fit` under P12.
6. E2 merge item: flip `scripts/tests/test_wave_kinds.py:247-251,311-312`.
7. SQL2-CLS literals `atx.composition-fit-spec/v1`, `atx.composition-fit/v1`, `atx.composition-fit-fixture/v1`, `atx.dsl-composition-weights/v3`.
8. Whether any P9 R-11 parent was fitted before D2 (refit needed, 0 trials). Do not run R-11 cells under `--engine-fit` until R-2 is fixed.
9. AL-COMB rebases on the D2 head (P17 / ALCOMB-5) with its `params_schema` rows in the closed shape.
