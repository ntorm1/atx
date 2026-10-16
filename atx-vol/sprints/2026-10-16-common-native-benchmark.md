# Native common-model quote benchmark

Implemented on 2026-10-16 in leased `C:/atx-wt/pool-35`, branch
`feat/vol-common-bench-20261016`, RunId/HeartbeatId `vol-20261016-commonbench`.
Frozen base: `f76cefc2336be782495b51f09cee05ba4e1a00cd`.
Dependencies were cherry-picked as `b5b94da1` (market adapter, original
`10de6dd7619a9871ab5731055b9ae8d93355ed42`) and `fbb0906c` (policy, original
`218b178ff8cf32c762956f5c533d9d6702e52431`). The root already has these features;
only the final native-lane commit should be integrated.

## Behavior and boundaries

`atx-vol-quote-bench` now accepts `--model quote-slice|common-markov`, defaulting
to the existing quote-slice route. The main translation unit retains the same
closed raw CSV parser. Shared input/prediction DTOs live in `quote_bench_data.hpp`;
the common runner and bounded policy parser have separate private headers/TUs.
No vendor fair prices, fair/quote vols, Greeks, synthetic spots, ticker lists,
or dataset-specific rules enter fitting.

The common route groups by underlier, deliverable root and exact snapshot. It
refuses conflicting chain identities and ambiguous duplicate expiry identities
before classifying liquidity. An ambiguity refuses the entire affected snapshot,
including otherwise distinct siblings, with all row/chain identities preserved.
Raw invalid-market or dispersion failures likewise refuse the whole snapshot.
The adapter constructs one accepted common spot and cumulative funding/yield
schedule. Policy classification receives those exact accepted clock endpoints
and common spot plus the raw strike/side/bid/ask observations. One bounded
positive-process fit then covers all contracts and shared expiry prefixes.

The output retains every source row and the existing scorer columns. New `model`
and `vol_status` columns distinguish numerical families and price/IV outcomes.
Finite-model prices can remain usable with blank IV. The `predicted_vol` convention
is explicitly an equivalent **flat-to-expiry American IV**, using Andersen-Lake,
variance time, `r = cumulative common rate / variance years` and
`q = cumulative common effective yield / variance years`. It is not a fitted
local sigma and does not preserve the full time-varying exercise problem.
Prices at the intrinsic plateau, stopped variance and inversion failures leave
IV absent. The existing scorer already has separate valid-price and valid-vol
masks; those semantics were preserved.

The best finite iterate can be published at an evaluation/sweep limit. Reports
include the precise termination, initial/final objective, interval/midpoint/
regularization losses, interval violations, fitted sigmas, prefix groups and
model diagnostics. This is not a global optimum, successful bid/ask fit, or
continuous-American accuracy certificate. All compiler/fitter positivity,
storage, time-step and work budgets remain intact, with no post-fit repairs.

## Configuration and provenance

Common mode uses `--common-policy auto|FILE`; explicit legacy `--policy`,
`--curve`, or quote-slice quality flags are rejected. A common file requires
both `"schema_version": 1` and `"model": "common-markov"`. Its closed root
keys are `profiles`, `symbol_overrides`, `regimes`, `market`, and `equivalent_iv`.
Every profile has a required name and built-in base plus optional `model`,
`grid`, and `fit` blocks. Fit vectors are copied into immutable catalog ownership.
The parser rejects duplicate keys, excessive nesting/files, unknown fields,
invalid types/ranges, inconsistent catalog configuration, and input/output/report
aliases including hardlinks. Library validation remains authoritative.

The `market` block exposes all adapter dispersion/resource limits independently
of liquidity policy. The `equivalent_iv` block exposes tolerance, iteration cap
and minimum time value. All actual numeric defaults and spans are serialized in
`policy_definition`; each resolved snapshot also records its expanded profile.
Expanded runtime output is provenance, not an input-schema round trip.

Existing `--dividends funding-pv|escrowed-pv|none` is bound to the adapter and
receipt. The aggregate-PV modes use common continuous-yield approximations;
unknown ex-dates and their exercise effects are not reconstructed. `none`
explicitly refuses nonzero PV.

Reports include full row statuses, per-profile price/IV coverage, unresolved
profile rows, snapshot failure stages, market failures, exact solver error codes
and messages, and separately typed market/quote-count resource refusals. Existing
solver `OutOfRange` can also mean a numerical failure, so its full message is
retained rather than falsely labeling every such error as a resource refusal.
All timings are serial diagnostics, not controlled HFT speed measurements.

The Python `quote_benchmark.py run` wrapper now binds `--model` and
`--common-policy` to its command and receipt, refuses passthrough overrides,
hashes custom common policy bytes before/after execution, and retains policy
content plus the native expanded report. It never opens labels during execution.
The established dataset, binary and prediction hash checks remain enabled.

Example after parent integration/review, with a fresh output path:

```powershell
python atx-vol/tools/quote_benchmark.py run `
  --dataset C:/atx-cache/quote-benchmark/<prepared-raw-cohort> `
  --binary C:/atx-wt/<active-root>/build-vol-rel-avx2/bin/atx-vol-quote-bench.exe `
  --output C:/atx-cache/quote-benchmark/<new-common-run>.csv `
  --model common-markov --common-policy auto --dividends funding-pv -- `
  --report C:/atx-cache/quote-benchmark/<new-common-run>-native.json
```

## Verification and parent integration

Implementation preceded tests, per the user's no-TDD instruction.

- Release AVX2 target `atx-vol-quote-bench` built with the leased-tree wrapper.
- **60 native CLI tests passed in 2.64 seconds**, zero skips: 18 common-model
  checks and 42 existing quote-slice checks. Independent formula controls cover
  flat zero-carry prices, shared multiple expiries, and funding-PV with distinct
  calendar/variance clocks. Other checks cover IV-only refusals, whole-snapshot
  dispersion/invalid-row failures, ambiguous expiries before routing, bidless
  upper bounds, exact overrides, legacy flag refusal, closed/bounded policy
  schemas, duplicate JSON keys, aliases and resource refusal with retained rows.
- **64 Python wrapper/scoring tests passed in 11.68 seconds**, including model/
  policy command binding, changed-policy refusal, incompatible options, protected
  passthrough fields, and the existing independent price/vol scoring masks.
- PCH-off wrapper checks passed for `quote_bench_main.cpp`,
  `quote_bench_common.cpp`, and `quote_bench_common_policy.cpp`.
- Ruff passed for the wrapper and affected Python tests; diff checks passed.

Native XML receipt:
`C:/atx-cache/quote-benchmark/common-native-20261016-tests.xml`.
No dev200/vendor-label run, untouched validation, reference panel, broad native
test label, release qualification, or performance benchmark was run by this lane.

Temporary CMake source registration was used only in this lease and restored
before commit. Parent must permanently add `tools/quote_bench_common.cpp` and
`tools/quote_bench_common_policy.cpp` to `atx-vol-quote-bench`, alongside its own
grid/market/policy library registrations. No shared ledger or README was edited.
The acquisition's default-dev configure failure was the known unrelated missing
modules; lease publication and dedicated vol presets worked. Graph tools were
unavailable, so native leased workflows and targeted source reads were used.

The delivery message supplies the exact final commit and lease-release receipt.
