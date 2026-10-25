# Full represented-curve expectation: Stage A component

This implements the independently reviewed prospective interface
`d41b30082d52c1639959d426007379a5d083becc`. It remains behind the existing
default-off cash-Gaussian research target. No pricing API, rollback engine,
calibration input or production numerical default changes.

`cubic_expectation` visits every stored finite Hermite cell and the complete
infinite affine tail. It returns owned decimal value and Delta strings at the
explicit 100- or 200-digit precision. Its finite contributions use the existing
reviewed local-weight and offset/increment contractions; their decimal output
conversion is counted. Continuous endpoint values cancel moving-boundary
terms globally, while one-sided slope kinks remain explicit in deterministic
results. The stochastic output is smooth. No probability cutoff is used.

The affine tail uses direct survival moments, with complete moments when its
lower endpoint is zero. Its excess `M1-a*M0` has a separate conditioning ledger.
The full value absolute-term sum retains
`abs(v*M0)+abs(m)*(abs(M1)+abs(a*M0))`, making cancellation inside that excess
visible again when finite cells and the tail are combined. All-zero curves are
valid; unresolved cancellation of nonzero terms into zero refuses. The original
precision-dependent `10^(digits-30)` condition limit and numerical domains are
unchanged. They are supported-domain checks, not rigorous error bounds.

The actual zero-time, zero-stock and zero-volatility branches are distinct.
The existing binary64 point API is unchanged. Nonidentity deterministic
transport uses a bounded decimal-only private point helper, never a binary64
stock projection. It refuses rounded equality or ill-conditioned distance to a
knot. Identity transport keeps authoritative one-sided knot slopes. Positive
tiny variance always uses the stochastic route, including when that route
refuses its domain; it never silently becomes deterministic.

## Work, storage and refusal accounting

The whole call applies the frozen hard limits for knot/cell/moment work,
selection/product/function counts, owner conversions/terms and elementary
functions. Each finite child receives the smaller of its existing cap and the
remaining aggregate allowance. Actual partial child work is retained on
refusal. Exact dyadic proof operations are aggregated; the last cell retains
the input-bound proof/status. No input or result spans escape the synchronous
call. No partial expectation is published.

The scalar preflight occurs before constructing the selected-precision
arithmetic context. Its conservative storage reservation is:

```text
owner tuples + 256*sizeof(selected precision) + 16384 + sizeof(work record)
  + maximum sequential child reservation
```

The child frame includes the owner tuples, 128 selected-precision scalars and
8192 bytes of strings. Positive stochastic finite cells add the larger of the
per-cell numeric reservation and the enabled fixed-integer proof reservation.
The global frame remains live while that child runs; reused child numeric
workspaces are not added once per cell. This is a conservative explicit
reservation, not measured RSS or allocator-metadata accounting.

Before each finite contribution/tail, the owner reserves 128 logical
conversions and 16 coefficient/accumulation terms. It charges actual counts,
including later string parsing and accumulation. The decimal deterministic
point helper separately reserves `128+2*number_of_cells` conversions because
its location scan can inspect every endpoint. Checked remaining-cap arithmetic
prevents size_t wrap; caller limits above hard limits cannot enlarge the domain.
Unit controls verify the finite contribution counts fit the declared allowance.

Work resets on every call and records route, phase/index, completed finite and
tail contributions, actual special-function attempts, sequential workspace and
the bounded last-cell/tail ledgers. The index identifies a finite cell or the
tail; the separate Point phase has no component index. Invalid arguments, numerical conditioning
and unavailable resources remain separate error categories. Allocation
exceptions are contained; no allocator-fault injection is claimed in this
component. `endpoint_projection_count` is always zero because this API returns
decimal strings and does not reconstruct an owner.

## Controls and remaining gates

The refusal stub produced eight runtime failures before implementation. The
initial 19 unit controls cover independent full moments/Black value and Delta,
partitioned affine cells including adjacent high/low/unit endpoints, signed
nonconvex cubic moments, finite `x^3` with its actual affine tail, deterministic
interior transport, one-sided kinks and the zero-stock limit. They also cover a
positive tail below `1e-500`, tiny positive variance without route substitution,
nonzero-drift knot uncertainty, cancellation between complete finite/tail
derivatives, whole-call budget exhaustion after a completed prefix, input
validation, hard-cap overflow protection, and owned result lifetime.

The independent formulas never call the implementation's moment/contraction
helpers. Supported value/Delta comparisons use the unchanged `1e-40` relative
gate; exact analytic zeros require exact zero. All 65 previous research controls
remain included in the scoped executable. The complete component run passes
84 tests before the ownership correction below. Owning-header hygiene is
recorded separately in the receipts.

The final scoped commands were:

```text
powershell scripts/atx-build.ps1 build atx-vol-cash-gaussian-research-tests -Preset vol-rel -Jobs 2
build-vol-rel/bin/atx-vol-cash-gaussian-research-tests.exe --gtest_output=xml:.../cubic-full-expectation-final-green.xml
powershell scripts/atx-build.ps1 check atx-vol/bench/cash-gaussian/cubic_expectation.cpp atx-vol/bench/cash-gaussian/cubic_expectation_test.cpp atx-vol/bench/cash-gaussian/stock_cubic.cpp -Preset vol-hygiene -Jobs 2
```

All three owning PCH-off objects passed (the unchanged third object was current
in the final command). The initial hygiene command required reconfiguring the
existing research target to discover the new source files; that command failure
is retained. It was not a numerical or owning-include failure. Source identities,
eight-failure stub evidence, all final controls and exact commands are pinned in
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/cubic-full-expectation-v1/receipt.json`.

This does not complete Stage A. In particular, it does not supply the separate
global-polynomial-tail descriptor or decimal-to-binary64 endpoint projection
accounting needed for repeated propagation. No 324-control matrix, 18-option
study, American rollback, PDE comparison, latency run or full-price/Greek error
bound is claimed. The earlier negative PWL/precision studies remain unchanged.
Fresh exact-source review is required before any later numerical export or
prospectively declared matrix.

## Moved-from owner correction

Fresh review of `ff8b4fac` found that the new reservation lacked the existing
point evaluator's empty-cell/tail-origin guard. Moving a valid owner can leave
the source with an empty vector and its original positive tail boundary. The
new stochastic evaluator then incorrectly reported a tail-only integral as a
full-domain expectation. The initial 84 tests did not cover that invalid
representation; their receipt remains unchanged and does not establish this
missing invariant.

A runtime regression reproduced the false success before the fix. The new
preflight now applies the same guard: an empty owner must have tail.lower=0.
The regression checks both precisions and all four dispatch routes refuse
before arithmetic, and the moved destination still produces its independent
full-affine value. This adds one control, bringing the scoped suite to 85.
No numerical formula or valid-owner result is changed. The original finding,
runtime RED, corrected green and two owning PCH-off checks are retained under
`calendar/cubic-moved-owner-v1/receipt.json`; no matrix or option run occurred.
