# Review cleanup: Markov fit, implied forward and time knots

Lane `feat/vol-review-cleanup-20261016`, pool-1, RunId `vol-20261016-clean`.
Frozen base `be85fd21`, rebased onto the moved root `2378605e` before the final
gates. Implementation first, tests after (as the brief asked). Defaults stay
byte-identical: the coordinate search, configured initialization, supplied
carry and the legacy expanded definitions are unchanged (gates below).

It closes the MINOR findings deferred from three integrated lanes:

- Markov-fit review `88c77c5d`: `C:/atx-cache/quote-benchmark/markov-fit-88c77c5d-review-v1/review.md`
- Implied-forward review `a5d2018a`: `C:/atx-cache/quote-benchmark/implied-forward-a5d2018a-review-v1/review.md`
- Time-knots review `f4416a74`: `C:/atx-cache/quote-benchmark/time-knots2-f4416a74-review-v1/review.md`

## A. Markov fit (commit `811282cf`)

| Finding | Fix | Test |
|---|---|---|
| MINOR-1: a one-sided sigma interval was closed with `min_sigma`/`max_sigma`; under `prior: seed` that layout constant became the prior centre (BAGY 0.25 -> 0.080) | The seed library refuses a one-sided interval as the new parameter status `OneSidedInterval`. There is no point seed and no artificial band; the market bound is reported in `interval_lower`/`interval_upper`. For `PerVarianceInterval`, either differenced endpoint being one-sided refuses. Quote-bench keeps the configured initial AND prior, and records state `fallback_one_sided_interval` (or fallback state `one_sided_interval`). That state is counted in `initialization_states`, so the bench report shows which path applied. | Seed: `AskOnlyIntervalIsRefusedNotClosedWithMinSigma`, `BidOnlyIntervalIsRefusedNotClosedWithMaxSigma`, `OppositeOneSidedQuotesCloseTheIntervalFromTheMarket` (two-sided), `PerIntervalOneSidedEndpointRefusesBothDifferencedGroups`; the existing two-sided test asserts no one-sided provenance. CLI: `test_one_sided_sigma_interval_keeps_the_configured_initial_and_prior` (fit stays at the configured 0.25, not sqrt(min*0.45)). |
| MINOR-2: a failed Jacobian column stayed zero and the gradient test still ran; all-failed columns reported `gradient_tolerance` | A refused probe is retried once in the other direction, within the evaluation budget. A column with no admitted probe stops the search with the new termination `JacobianFailure` (`jacobian_failure`), before any gradient test. | `RefusedJacobianProbeIsRetriedInTheOtherDirection`, `ColumnWithoutAnAdmittedProbeStopsWithJacobianFailure` (explicit-Euler work cap; before the fix this reported GradientTolerance) |
| MINOR-3: DampingLimit and the Cholesky refusal untested | DampingLimit is reached through `max_damping == initial_damping` and a first trial refused by the work cap. The Cholesky refusal is unreachable for a finite J'J, so the solver moved to `detail::markov_lm_damped_step` (private `fitting/american_markov_fit_lm.hpp`) and is tested directly. | `FirstRejectedTrialAtMaxDampingStopsWithDampingLimit`, `DampedStepSolvesPositiveSystemsAndRefusesTheRest` |
| MINOR-4: no LM test on `VarianceTimeKnots` reuse | Test only. Interval 1 is paused but maps onto knot 1, so knot 1's probe reprices expiries 1..2 and reuses expiry 0. The stored best model is that reused probe. | `TimeKnotProbeReusesOnlyTheUnaffectedPrefixBitForBit`: `reused_quote_prices == 10`, stored prices equal a full repricing bit for bit, and the reused prefix equals the starting model's bits |
| MINOR-5: the `application`/`aggregation` texts contradicted `prior: seed` and `sigma-interval` | Opted-in variants: a seed-prior application text; a sigma-interval aggregation; a bid-mid-ask-primary + sigma-interval-fallback aggregation. The `interval_estimator` text now states the one-sided refusal. The default bytes are pinned. | `test_default_initialization_definition_keeps_its_bytes`, and the CLI one-sided test asserts the opted-in texts. Probe: frozen e0/r104/r416 definitions hash identically (below). |
| NIT-1 (fallback-on-error doc) | Doc fixed: an estimator error never runs the interval fallback. | - |
| NIT-2 (probe truncated near `max_sigma`) | The primary probe steps backward whenever the forward room is shorter than `jacobian_log_step` (or toward the larger room when both are short). A sigma sitting on a bound never probes that way. | `ForwardRoomShorterThanTheStepProbesBackward` |
| NIT-3 (`levenberg_marquardt` 120 lines) | Split into `jacobian_column`, `refresh_jacobian`, `damped_trials` and a 30-line driver. The normal path is bit-identical. | Existing LM tests unchanged |
| NIT-4 (TDD deviation disclosed) | No change; this sprint also ran implementation first, per its brief. | - |
| NIT-5 (ledger "converges 91/91") | Correction line appended: 51 of the 91 stopped on StepTolerance. | - |

## B. Implied forward (commit `3ed89f9b`)

| Finding | Fix | Test |
|---|---|---|
| MINOR: Parity on an American chain kept an unmeasured pair Retained and active; `pairs_unidentified` was always 0 | The pair takes the step's typed status (`Unidentified`, or `Contaminated` for a non-positive step) and is deactivated; the measured pairs are re-located. With none left, the result is `Contaminated`, "no pair supports a forward". The header contract says so. | `ParityExcludesAnAmericanPairWhoseBiasCannotBeMeasured`: one pair is inflated on both legs (parity intact, anchor above the American bound). Result: Ok, `pairs_unidentified == 1`, robust weight 0. With every pair inflated: Contaminated, all unidentified. |
| MINOR: an `implied_forward` block without an implied `carry_mode` was silently inert | Refused at policy load when the resolved carry is Supplied | Two new rejected documents in `test_native_rejects_invalid_policy_before_output` |
| MINOR: `estimate_implied_forward` was about 157 lines | Split into `align_strip`, `measure_parity_bias`, `publish_carry` and `classify`; the body is now about 50 lines. The AmericanAlignment and European code moved unchanged. | Full suite, including `IsBitwiseDeterministic` |
| NIT: post-estimate fallbacks counted as a bare `fallback:ok` | Typed `QuoteSliceCarryFallback` (`dated_cash`, `invalid_market`, `estimate_refused`, `no_model_yield`, `slice_refused`), emitted as `fallback_cause`. A non-estimator fallback key appends its cause, e.g. `fallback:ok:no_model_yield`. | `QuoteSliceCarry.*` assertions; `test_native_implied_forward_profile_records_every_estimator_control` |
| NIT: the allocation test missed the `align_val_t` overloads | Aligned new/delete replaced and counted, with a self-check | `ImpliedForwardAllocation.OverAlignedAllocationsAreCounted` |
| NIT: `carry_json` omitted `max_outlier_fraction` | Emitted, plus the `AlOpts` fields when the preset is custom | CLI provenance test |
| NIT: ledger "(parity variant 0.313, tight 0.262)" | Correction line appended | - |
| **Deferred**: `quote_slice.cpp:383` carry dispatch before `validate()` | Not touched: another lane owns `quote_slice.cpp/.hpp` | - |
| Not in scope: `implied_forward.hpp:58` allocation-free wording / reported method; `quote_slice_carry.cpp:179` AL preset mismatch; README paragraph order | Left for the owning lane | - |

## C. Time knots (commit `abf15ab0`)

| Finding | Fix | Test |
|---|---|---|
| MINOR: about 140 lines of private implementation inline in `american_markov_time_basis.hpp` | Bodies moved to `american_markov_time_basis.cpp`; the header keeps the declarations and contracts. The focused fit test still uses its private `src/` include. QR row admission is split from the rcond recurrence (review NIT-3), and a comment records that the sqrt(M) normalization cancels in rcond (NIT-4). | `AmericanMarkovTimeBasis.*` and the time-knot fit tests unchanged |
| MINOR: the fit report dropped `interval_groups` while `schema_version` stayed 3 | Common-markov report `schema_version` goes to 4; a code comment lists the changes | CLI: `report["schema_version"] == 4`, no `interval_groups`, exact shared-shape `interval_time_weights` |
| NIT: `[[nodiscard]]` on the new Status/Result helpers | Added to the three time-basis helpers and to `map_time_weights`/`admit_time_knots` | compile |
| NIT: "Markov fit:" prefix on catalog refusals | Catalog refusals from the shared validators keep code and body under "Markov policy:" | `RejectsInvalidVarianceTimeKnotProfiles` asserts the message |
| NIT: unreachable post-clamp range check | Marked as a defensive invariant | - |
| NIT (optional): log/exp per node | **Not done.** It is a pure performance change, and none was requested. | - |

## Gates (vol-rel, pool-1, rebased head)

| Gate | Result |
|---|---|
| Build (fit, seed, policy, implied-forward tests and `atx-vol-quote-bench`, `-Jobs 3`) | exit 0; `atx-vol-tests` never built |
| gtest fit / seed / policy / implied-forward | 41/41, 25/25, 17/17, 26/26 |
| ctest `^atx-vol-(american-markov-(fit\|seed\|policy)\|implied-forward\|quote-benchmark-native)$` | 5/5 (Python312) |
| Native pytest (quote-bench and common-markov CLI) | 149/149 |
| PCH-off `vol-hygiene` check | 16 changed TUs, 23 objects (including the monolithic owners), clean |
| Legacy e0/r104/r416 expanded definitions (group A binary, pre-rebase) | canonical sha256 bd946410 / 33ef8af7 / de236acf, equal to 88c77c5d |
| R104 dev200 parity (rebased binary `c349d99f`) | `6b129f64...` identical (exit 0, 460 s) |
| Quote-slice default parity (`c89d0739`) | not run (user stop) |

## Receipts

`C:/atx-cache/quote-benchmark/cleanup-20261016-*`:

- `cleanup-20261016-bin/`: the final binary `atx-vol-quote-bench-abf15ab0.exe` (sha256
  `c349d99f7818ae64c8b12bd80175a0482983819d57c3b306575b5cb759bfd3e1`) and `head.txt`
- `cleanup-20261016-gates/`: gtest XML and logs, `ctest.log`, `pytest-native.xml`,
  `hygiene-check.log`
- `cleanup-20261016-defs/`: `policy-definition-parity.json` and the e0/r104/r416 reports
- `cleanup-20261016-r104/`: the dev200 R104 run (predictions, report, `run.log` with the hash)
