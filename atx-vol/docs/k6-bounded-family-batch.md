# K6: bounded family pricing and native acceptance

The internal `family_prices_bounded` seam evaluates a request span without allocating.
It preserves `family_price_bounded` price bits and admission statuses. Its error bound
is at least the existing scalar bound. This is an optimization of the existing
refinement-validated error contract, not a new rigorous interval remainder.

For an accurate family with a current envelope, the implementation combines the premium
quadrature and boundary-sensitivity bound. They share the same interpolated boundary,
carry exponentials and logarithm. The premium retains the scalar operation order,
including `sigma * sqrt(z*z)`. The sensitivity retains its different `sigma * z` order.
On a verified node, scalar computes `min(trivial_box, sensitivity)`; batch uses
`sensitivity`. Dropping that minimum can only enlarge the bound and removes the extra
pair of CDF evaluations. Unverified nodes retain the trivial bound. Fast-profile,
uncovered, stale, invalidated-geometry and exceptional-arithmetic cases retain the
scalar path. Preparation identity is checked before dereferencing the old workspace.

`iv_native_prepare_acceptance` uses this seam for the first pair of endpoints when its
existing batch mode is enabled. Both endpoints retain the width correction and must
certify their own side of the root. Because the prices match and the bounds cannot be
smaller, a batch success is the scalar first bracket, with the same root and endpoints.
An open or invalid batch bracket retries the unchanged scalar enclosure, including
its optional recenter. This preserves scalar acceptance decisions and confirm counts.
Scalar mode is unchanged. `family_batch_prices` and `family_retry_prices` count actual
work, including conservative attempts that need a retry; total `family_prices` also
includes those attempts. The thread-local gather buffers grow and are reused.

The same seam can later serve final-grid and band-edge callers. This change wires only
native acceptance. It makes no full-chain latency or sub-second claim.

Validation receipts are under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/k6/`:

- `red-batch.xml`: missing batch implementation failed price/bound and refusal tests.
- `red-acceptance.xml`: missing native batch work failed the enclosure integration test.
- `pricing-focused.xml`: 19 passed; one opt-in timing probe skipped. Includes 450 exact
  scalar-price comparisons across both sides, strike tails, sigma/q box endpoints,
  independent cold-price bounds, unverified nodes, Fast-profile fallback, stale
  envelopes and geometry invalidation. Existing scalar envelope audits also passed.
- `native-focused.xml`: all three existing native batch regressions passed. Scalar
  route remains bit-identical; AVX2 keeps every acceptance/route/status class and
  confirm count. Direct enclosure tests cover both bounds, width rounding, inflated
  envelopes, open brackets and recentering, with exact scalar root/endpoint bits.
- PCH-off `vol-hygiene` checks passed for `american_family_error.cpp`,
  `american_iv.cpp`, and the downstream `deam_iv_cache.cpp` consumer of both headers.

The opt-in `AmericanFamilyError.BatchIsolatedCostProbe` compares scalar and batch in
one binary on 8,192 paired endpoints for each side, alternating measurement order.
Family construction is excluded. Set `ATX_K6_COST=1`; run only that test from
`build-vol-rel-avx2/bin/atx-vol-pricing-slice.exe` on a quiet pinned core. Round zero
warms the code; later rounds are the measured repeated-pricing workload. This is not
a cold-fit benchmark. The printed results cannot establish whole-fit improvement.

The first pinned ThinLTO probe used clean commit `29da16c5`, logical CPU 6, with
SMT sibling 7 measured idle throughout. Its raw post-warmup paired speedups were
1.51–1.94x for puts and 1.39–1.46x for calls. Absolute batch-time sample CVs were
15.38% and 9.12%, respectively, so the 5% stability gate rejects a qualified
aggregate. These are isolated observations, not a promoted performance number.
`isolated-receipt.json` records executable SHA, source identity, affinity and CPU
activity; `isolated-cost.txt` retains every round. Full-fit timing must separately
account for scalar retries and unchanged downstream confirmation work.
