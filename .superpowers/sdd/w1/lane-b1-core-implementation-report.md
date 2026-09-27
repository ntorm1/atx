# W1-B1 core and adapters: source qualification pending

Production `a3ee26f0f7401d7e1f01bb98af1d19b4ff2d5e41`; postimplementation
checks `7cad09c92b30a6a31e154d4f4392fa8ee7cc76fb`. Pool5 source was aligned
with root `8ccf098285ef46a1efb49bae410a1af3f52884e9` before implementation.
The existing lease was retained; pool6 and the primary checkout were untouched.
Implementation preceded tests. No configure, compilation, numerical execution,
market-data or warehouse access occurred in this lane.

Authority: original DAG W1-B1 and root's explicit bounded core/adapters assignment
under the owner's build-first override. The preparation brief remains the full B1
contract. The current ledger is `atx-engine/docs/LEDGER.md`; its former atx-vol
location was retired. Only the orchestrator updates the ledger/CMake/DAG.

## Delivered source

- `cost/cost_surface.hpp` and `src/cost/cost_surface.cpp`: immutable shared-owned
  O(names) snapshots. Ordered unique instrument IDs, source SHA256, decision clock,
  liquidity recipe and calibration origin are mandatory. Available row clocks are
  strictly before the decision. Unknown rows use an explicit missing tag; their
  irrelevant numeric payload is normalized. Recipe and snapshot SHA256 encodings
  use length-prefixed strings and little-endian numeric words, canonical signed
  zero, and canonical hexadecimal source identity. No hashing of object padding.
- One-way cost uses full-spread fraction / 2, commission in bps (default 1), and
  Y (default 0.6) times daily return volatility times square-root dollar
  participation. Dollar, weight/NAV and coefficient entry points use this same
  recipe. Successful hot quotes allocate no memory. Creation checks a conservative
  working budget before population-sized allocations (default 64 MiB); no actual
  peak-RSS measurement is claimed.
- `price_trade_path` is the pure fitness adapter for supplied chronological actual
  signed dollar deltas and pretrade NAV. It reports component costs, summed NAV
  cost fractions and one-way turnover; it does not construct targets or fabricate
  P&L. Missing nonzero trades fail explicitly.
- `cost_terms_from_surface` supplies the existing risk cost view plus required
  untradeable flags and trade caps. R1/R3 callers must enforce those constraints;
  the coefficients alone do not impose them. No borrow fee or locate is invented.
- `SurfaceReplayCost` binds one snapshot to one execution period and its supplied
  clock. Execution cannot precede the snapshot decision; another period refuses
  fills. The legacy interface has no intrinsic time axis, so the caller owns the
  period-to-clock mapping. Snapshot liquidity cannot be overridden by an external
  liquidity row. Full-request research cost and capped-fill cost remain separate.

Existing FlatBpsCost, SqrtImpactCost and their optimizer conversion arithmetic were
not changed. No current search, optimizer or stage default is silently migrated.
Root registration required: **`atx-engine/src/cost/cost_surface.cpp`** only.

## Independent review and checks

Root independently reviewed the five production files at `a3ee26f0` and reported
no source blocker in hash/budget, units, availability, missing rows and period
binding. This is source approval, not compiled qualification.

The new `tests/core/cost_surface_test.cpp` owns eight `CostSurface.*` checks:

1. Independent formula versus dollar/weight, trade-path, real optimizer evaluator
   and replay at the same signed executed quantity and NAV (1e-12 return units).
2. Capped execution priced at its own quantity; full request remains independently
   priced, with matching optimizer cap and no nonlinear average-rate scaling.
3. Missing rows cannot become free executable trades; zero trade is zero and
   unavailable optimizer names carry mandatory pins/caps.
4. Invalid geometry, duplicate IDs, units/nonfinite inputs and unavailable clocks.
5. Owned input lifetime and canonical recipe/source/order/support hashes.
6. Future snapshot mutation preserves prefix quotes and identity; reversed
   trade chronology is rejected.
7. Full signed reversal and square-root impact scaling with NAV.
8. Fail-closed numeric overflow, including representable weight limits when the
   intermediate dollar cap overflows.

`git diff --check` passed. These tests have **not** been compiled or run. Root owns
the next warm Jobs1 build and focused execution after current A1/E1/V3 qualification.
Relevant existing owning checks are core cost, book replay cost and risk optimizer
cost terms; no full-suite or long performance run is requested by this slice.
Scoped PCH-off compilation of both production TUs remains pending as appropriate.

## Explicit remaining scope

This does not complete B1: pinned Corwin-Schultz/Abdi-Ranaldo/EDGE fixtures and
spread composite, clamped-exponent intercept refitting, borrow tiers and annual
unit adapters, and a properly specified FIM reference scenario are outstanding.
The scalar Y prior is modeled, not an observed execution calibration. No spread
estimator parity, fill calibration, borrow availability, locate capacity or alpha
tradeability is claimed. Full consumer migrations remain A4/R3/B2; B3 still owns
capacity through re-optimization. Register this surface with X1 when the shared
causality harness lands; the local future-mutation check is not the wave harness.

No original acceptance threshold was changed. Large gates and the long benchmark
remain deferred/unpassed under the recorded owner steering.
