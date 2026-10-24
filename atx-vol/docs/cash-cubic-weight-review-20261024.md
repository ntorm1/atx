# Fixed local cubic-weight precision study

The first fixed 12-cell/two-precision study is **negative**: all 24 requests
compute and all 168 independent positive-integrand quadrature checks pass, but
three cells fail the predeclared `1e-40` agreement threshold between 100 and 200
decimal digits. Stage A remains incomplete. No 18-request cubic semigroup
experiment, American reference replacement, production change or timing claim
follows from this receipt.

## Frozen scope and provenance

The declaration is `bench/cash-gaussian/cubic-moment-plan.md`, first committed at
`73dd008e`; the executed clean source is
`d922d1946cc9989f686dff04772aef51308b25dc`. The fixed 12 cells and both precisions
were selected before their numerical output. There was one native export, with
exit code zero and no numerical rerun. The original 50-case cash manifest,
150 PDE outcomes, 32 Gaussian pilot outcomes and 18+5 PWL diagnostic outcomes
remain unchanged.

Evidence directory:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/cash-cubic-weights-v1`.

| Artifact | SHA-256 |
|---|---|
| `weight-control.exe` | `5cefcc2c9544c6419bb5f9220611a4f709f11be924f418ef252b974a6bb4ea18` |
| `native.json` | `70e635b6d7becba2a51d6e9ef71d4e3e7dd5e0b6ee3bf727e4302fefaf0f90bb` |
| `summary.json` | `d030ff11d37b86025e2e819fe45427b8f41ed6fe4f290bc4929f3091dfdd5164` |
| Numerical reader | `808813614103ff6bfb199859214ad4bdd3413360d6762bd8b0d1dd652f3b1205` |
| `receipt.json` | `98330cd9cbb763ec3ee1d4c92b57413bdb667270908835622c896e30e45633e6` |

The receipt hashes 22 retained artifacts, including the original negative
summary, native output, source/build manifest, executable, test results and
freeze helper. It verifies unchanged numerical source and reader bytes against
the execution manifest. The only source change after execution is the test
contract correction described below. The native helper receives no reference
prices or baseline outcomes. The reader uses Python 3.12.2 and SciPy 1.12.0 from
`C:/Program Files/Python312/python.exe`; no package was installed.

## Complete results

The reader compares every reported local price weight, derivative weight and
scaled raw moment at both precisions. The table reports the largest relative
difference per cell; all 12 comparisons are required.

| Cell | Maximum relative difference | Fixed precision gate |
|---|---:|---|
| central | 2.31840e-98 | pass |
| zero-lower | 9.99651e-99 | pass |
| adjacent-high | 6.14476e-37 | **fail** |
| adjacent-low | 1.78930e-37 | **fail** |
| adjacent-unit | 1.05132e-38 | **fail** |
| lower-tail | 4.51014e-94 | pass |
| upper-tail | 6.71761e-96 | pass |
| positive-near-zero-time | 9.33135e-46 | pass |
| negative-rate | 2.92674e-100 | pass |
| negative-yield | 1.16112e-98 | pass |
| small-variance | 2.48443e-88 | pass |
| wide | 7.47156e-100 | pass |

All 168 quadrature comparisons pass: seven price/derivative components for
12 cells at both precisions. The independent reader directly integrates
positive Bernstein basis functions against the local lognormal density on
the complete interval `[0,1]`; it does not subtract the native raw moments.
Its fixed tolerance remains `5e-11*abs(value) + 8*estimated_quadrature_error`,
with `epsabs=1e-14`, `epsrel=3e-13` and at most 200 subdivisions. Density
rescaling prevents underflow without discarding a tail or interval.

These results establish agreement at the quadrature's much looser tolerance,
not 100-digit accuracy. Conversely, a relative discrepancy around `1e-37`
does not demonstrate a dollar-level pricing error. The failed precision gate
identifies substantial cancellation in a primitive intended to validate a
new representation. Its threshold is retained rather than reinterpreted after
observing the result.

The native path computes four shifted lognormal moments, eight normal-function
calls and 28 polynomial combination terms per completed request. Across this
fixed export that is 96 moment attempts, 192 normal-function calls and 672
combination terms. These are operation counts, not latency measurements. The
current moment formulas scale by the upper cell edge, then subtract polynomial
moment combinations containing inverse powers of the cell width. Adjacent
binary64 edges make those combinations ill-conditioned even at high precision.
This is a source-based diagnosis supported by the cell pattern; the receipt
does not isolate a certified error contribution from each transcendental call.

## Controls and honest test status

Three refusal-stub tests first failed at runtime. After implementation, six
new native groups and 26 unchanged Gaussian groups pass. They cover positive
weights and conservation, adjacent-stock cells, degree-zero through degree-three
identities, invalid/numerically unsupported inputs, work reset and partial
resource exhaustion. Three owning translation units pass with PCH off.

Two initial Python fixture tests incorrectly required this numerical fixture
to pass. They failed when the actual negative receipt was supplied. The
test-only correction now checks that the reader faithfully returns all actual
precision/quadrature decisions and detects an injected common moment bias.
All four reader test groups pass, including malformed-input mutants and
retained refusals. The numerical reader, its thresholds, native output and
negative summary are byte-for-byte unchanged. A green reader test suite is
therefore explicitly separate from the failed numerical control gate.

The summary keeps `controls_passed=false`, `stage_a_complete=false`,
`semigroup_run=false`, `full_price_error_bound=false` and
`timing_qualified=false`. Even a future successful 24-request study cannot
replace the full-domain/tail, repeated-polynomial, analytic-Delta and
cash-kink obligations in `bench/cash-gaussian/cubic-plan.md`.

## Next bounded experiment

The prospective [local-density series plan](../bench/cash-gaussian/cubic-local-series-plan.md)
forms the seven positive local weight integrals directly. It avoids recovering
them by cancellation of nearly equal raw moments. It fixes an analytic
admission domain, truncation selection, owned workspace and work caps before
new output. Price-weight error and final Delta amplification have separate
ledgers. The original high-precision moment path remains a labeled control
outside the new local domain; no precision, grid, monetary budget or failed
historical classification is changed.

The next implementation needs its own source review and controls before any
fixed-24 successor export. Passing that export would authorize only continued
Stage A work, not the 18-request option experiment or a cash reference claim.
