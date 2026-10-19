# Deferred review tests lane (2026-10-17 / finished 2026-10-18)

Branch `feat/vol-deferred-tests-20261017`, base `022e9727` (root head after wave
3a). Items 1-4 were written by the resume-6 implementer (WIP `8d6cafa1`, user
stop); lane L0c of sprint 2026-10-18-vol-sota finished items 5-6, the revert
runs and the gates (`adf3ca27` + this note). Tests only: no library source,
header, tool or CMake line changed. Pool-3, `vol-rel`, `-Jobs 4`.

## What and why

Five resume-5 lanes were approved with "minor (deferred): missing test"
findings. This lane writes those tests. Each one pins behaviour that already
exists; nothing in the library moves.

| # | review finding | test(s) | status |
|---|---|---|---|
| 1 | cleanup 3465221c: carry fallback causes | `QuoteSliceCarry.ForwardWithoutAFiniteModelYieldFallsBackAsNoModelYield`, `...CarryTheSliceCannotRepresentFallsBackAsSliceRefused`, `...EveryMalformedMarketFieldReportsTheSuppliedError` | done; InvalidMarket is pinned by contract (see Skips) |
| 2 | cleanup 3465221c: Parity `Contaminated` exclusion | `ImpliedForward.ParityExcludesAnAmericanPairWhoseBiasStepLeavesPositiveForwards` | done |
| 3 | serve-path MINOR-2: refused-row rule | `ExpiryRecord.ACertificationRowWithoutAColdReferenceLeavesTheRecordUncertified`, `ChainTickRecords.ARecordWithARefusedCertificationRowIsRefusedByTheSpotLadder`, `SessionServeRecords.RecordsWithARefusedCertificationRowAreNeverServed` | done |
| 4 | serve-path MINOR-3: box-end solve reuse | `ExpiryRecord.CertificationReusesTheBoxEndBoundariesBitIdentically` (25 vs 27 `AlBoundarySolves`, bit-identical record) | done |
| 5 | smooth-div N2: route-check reorder | `test_native_empty_route_over_cash_in_life_is_a_window_mismatch` (`python/tests/test_quote_bench_cli.py`) | done |
| 6 | cleanup Markov nit: few-ulp retry near a bound | `AmericanMarkovFitSearch.RetryTowardAFewUlpRoomIsEvaluatedWithAFewUlpDelta` (characterisation, `min_sigma` mirror) | done; `max_sigma` case unreachable (see Skips) |

### Item 5: how the N2 path is reached

One dated chain (the `make_quotes` strikes, rate 5%, 182-day life, one 1.00
dividend whose PV dates it exactly 91 days out) is run under an
`inferred-schedule` policy at two valuation days. The schedule is the same at
both: status ok, one exact event at day 91, coverage covered, one event in life.

- Day 20,000 (control): route `lattice`, reason `ex_date_in_life`,
  `route_events` 1.
- Day 69,000 (year 2158): `dividend_schedule_instants` refuses at its int64
  guard (`dividend_schedule.cpp:512-517`, valuation day below about 67,640), so
  the route is empty. The reason is `route_window_mismatch`, and `no_ex_date_in_life`
  with the `d131b3f8` reorder reverted (`quote_bench_main.cpp:814-819`).

### Item 6: what the characterisation pins

The retry at `american_markov_fit.cpp:718-724` (`jacobian_column`) skips a
direction only when its room is exactly zero or the probe rounds back to the
centre. Setup: `min_sigma` 0.2, initial `0.2 * (1 + 1e-15)`, step 0.5, and a
`max_rollback_work` cap equal to one evaluation at the centre (refuses the
forward probe; ExplicitEuler work rises with sigma).

- Measured: room 1.11e-15, retry probe = `min_sigma`, delta -1.1102e-15 (5 ulps
  of log sigma). The retry is evaluated and its quotient is the column:
  `GradientTolerance`, 3 evaluations, 2 Jacobian, 1 failed, best = initial.
- That column's gradient norm is 113554.955 against 113576.875 from an
  admitted 1e-3 forward probe (2e-4, inside the reference's own truncation).
  On this smooth chain the few-ulp quotient is accurate, not noise. Pinned at
  1e-2 because the low bits depend on the pricing ISA.
- The review's proposed fix (treat a room far below the step as "cannot move";
  tried as `room > 1e-6 * step`) turns this into `JacobianFailure` with 2
  evaluations, and the test fails. The test flags that change if it is ever made.

## Skips and limits (documented, not defects)

- **InvalidMarket cause is unobservable.** `fit_quote_slice` validates the
  market before carry resolution, and the carry guard's predicate
  (`quote_slice_carry.cpp:178-179`) is a subset of `validate()`'s. Inside the
  seam the cause would ride only on the Supplied fit's error, and
  `fit_supplied` records a resolution on success only (`:151-155`). Measured:
  deleting line 181 leaves `atx-vol-implied-forward-tests` 31/31 green. The test
  pins the documented contract ("the Supplied fit reports it") for 15 malformed
  markets.
- **The near-`max_sigma` retry is unreachable through the public API.**
  With `room_up < jacobian_log_step <= room_down` the primary probe is backward
  (`american_markov_fit.cpp:714-716`). A forward few-ulp retry therefore needs a
  refused backward probe. Every refusal a caller can arrange is a cap that grows
  with sigma or stays flat: `max_rollback_work` (`:384`) charges ExplicitEuler
  work proportional to the CFL count (sigma^2; flat on resolvent clocks), the
  compiler's transition, stored-row and transition-work caps
  (`american_markov.cpp:347-361`) scale with the same count, and `:371` (batch
  values) depends on the state count only. So a probe below an admitted centre
  is not refused by any of them. What remains are numerical-range refusals
  (e.g. resolvent factors, `american_markov.cpp:164-166`) that no cap controls
  and that this lane did not prove monotone in sigma: the claim is "unreachable
  through any refusal a caller can arrange", not a proof over every input. The
  `min_sigma` mirror is the reachable case and is the one tested.

## Revert evidence (transient library edits, each restored by inverse Edit)

| item | transient revert | result with revert |
|---|---|---|
| 1 NoModelYield | drop `fallback = NoModelYield` (`quote_slice_carry.cpp:210`) | FAILED: fallback `none` (0) vs `no_model_yield` (4) |
| 1 SliceRefused | drop `fallback = SliceRefused` (`:222`) | FAILED: fallback `none` (0) vs `slice_refused` (5) |
| 1 InvalidMarket | drop `fallback = InvalidMarket` (`:181`) | 31/31 pass (unobservable, as documented) |
| 2 | failed bias step no longer deactivates the pair (`implied_forward.cpp:603-609`, pre-`3ed89f9b`) | FAILED: contaminated 0 vs 1, unidentified 0 vs 1, strike 95 Retained, weight 0.0161 |
| 3 | `cert_err_ = worst` (pre-`e695a411`, `expiry_record.cpp:414`) | all three FAILED: finite certificate, ladder refuses 0 of 5, session serves the calls |
| 5 | pre-`d131b3f8` branch order (`quote_bench_main.cpp:816-819`) | FAILED: `no_ex_date_in_life` |
| 6 (extra) | the review's proposed fix (`american_markov_fit.cpp:721`) | FAILED: `JacobianFailure`, 2 evaluations |

After every restore the working blob equals HEAD's (`git hash-object`):
`quote_slice_carry.cpp` d0c6863a, `implied_forward.cpp` e6d55632,
`expiry_record.cpp` a4053261, `quote_bench_main.cpp` 4f54758d,
`american_markov_fit.cpp` 0c2df843.

## Gates (vol-rel, restored sources)

| gate | result |
|---|---|
| anchored ctest `^(atx-vol-implied-forward\|atx-vol-american-markov-fit\|atx-vol-quote-benchmark-native)$\|^tick-path:\|^fitting/SessionServe` | 66/66 |
| `atx-vol-american-markov-fit-tests` / `atx-vol-implied-forward-tests` | 42/42 / 31/31 |
| native pytest (`test_quote_bench_cli.py`, `test_common_markov_bench_cli.py`) | 153/153 |
| ruff on `test_quote_bench_cli.py`; `git diff --check 022e9727`; 100 columns on added lines | clean |
| PCH-off hygiene | not applicable: no header touched |
| dev200 default parity, lane binary `845d3600...` | `c89d0739...` MATCH (`C:/atx-cache/quote-benchmark/L0c-deferred-tests-20261018-parity/`) |

## Open (not this lane's to fix: library change)

- **Refused-row records report no exercise edge.** When a box-end edge solve
  fails (the refused-row case), `e_lo`/`e_hi` is NaN (`expiry_record.cpp:293-294`).
  `std::min`/`std::max` then carry the NaN into `e_min`/`e_max` (`:295-300`), so
  `has_edge_` (`:301`) reads false: edge 0 on every refused-row record. It fails
  closed because the record is uncertified, but the record's edge geometry is
  wrong. Routed by the PM to the short-dated/LEAPS lane.
