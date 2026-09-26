# W1-A1 bounded implementation: prequalification

Source freeze: `0906a4eec10151c86b04c4e60e043ced1d882716`, based on frozen root
`7e16ed267dcd3f1094c0e504dbe14e78f95eae03`. Pool4 owns four production headers and
two new alpha test translation units. Implementation preceded test additions.
No configure, compilation, test execution, benchmark, market-data or database
access occurred in this implementation turn. Graph tools were unavailable;
targeted source searches were used. Root owns combined qualification and CMake.

## Implemented behavior

| Commit | Production change |
| --- | --- |
| `f257bc91` | Whole-width delay is one `memmove` followed by warmup fill; partial delay copies contiguous rows. Delta walks date-major rows, with 64-instrument tiles and explicit xsimd in ResearchFast. Range/buffer geometry is checked; full delay permits overlap, partial/delta overlap returns an error before writes. |
| `667d386c` | ResearchFast sum/mean uses 64-lane SoA scratch and SIMD Neumaier enter/remove updates. Per-instrument arithmetic order is retained. A supplied time-series pool partitions independent tiles; unaligned external ranges remain supported. |
| `bc3e173c` | ResearchFast corr/cov/pair-regression uses existing `CoMomentLane` in fixed-size tiles without per-call lane allocation. Streaming uses the same lane. RelativeV2 flat correlation/predictor classification is explicit; AuditExact and explicit legacy pair routing retain direct arithmetic. |
| `ca8ea138` | Finite-window exponential recurrence shared by VM and streaming. Coefficients/weights are prepared outside the cell loop, storage grows only with the largest encountered window, and clean decay uses a stable closed-form weight denominator. Running NaN/infinity counts replace full validity scans. |
| `383f5c90` | Existing linear-decay/WMA/time-regression lanes now sweep date-major 64-instrument tiles and use the supplied worker pool. Time regression remains regression on time, not pair regression. |
| `59728493`, `0906a4ee` | Independent review found overflow can poison new pair routing beyond the offending observation's exit. Nonfinite raw or derived co-moments now trigger a causal rebuild, then direct fallback if the current window still overflows. Relative-flat covariance retains its chronological direct formula because AuditExact does not zero that case. |
| `e0fffa1d` | Postimplementation future-price/missingness/membership mutation and explicit legacy arithmetic fixtures. |

The normal VM owns distinct live slot buffers (`panel.hpp::SlotPool::column` is
`storage + slot*cells`). Bytecode recycling follows the source's last use.
External range callers are checked for lookback overlap rather than assuming
the pool invariant. Tiles keep all dates of each instrument on one task; scratch
is stack-local and coefficients are immutable during dispatch. Existing
`set_ts_pool` prohibition on dispatching recursively into the same pool remains.

## Numerical and complexity boundaries

AuditExact exponential/pair/windowed-sum arithmetic is unchanged. Explicit
`KernelPolicy::legacy_v1()` retains old sum, pair and exponential routes. Streaming
has no selectable legacy policy; no legacy-streaming claim is made.

For ordinary finite `0 < f <= 1`, exponential state uses
`S[t] = f*S[t-1] + x[t] - f^d*x[t-d]`, seeded from the first complete clean window.
It rebuilds every `2*d` clean steps, after missing data leave, on nonfinite state,
or after severe cancellation. Thus the normal path is **amortized** O(1), not
unconditionally worst-case O(1). The supported DSL factors `f > 1` keep the direct
chronological formula to avoid an unstable forward recurrence. Infinity windows
also use direct evaluation, preserving `0*inf` when old weights underflow.
Overflow/cancellation and flat-covariance exceptions may cost O(d) per cell.

Explicit SIMD currently covers sum/mean and delta. Pair, exponential and existing
unary sliding lanes use contiguous tiled scalar state transitions; this is not a
claim that all time-series kernels have become SIMD. Welford variance, extrema
and order-statistic dispatch have not been redesigned. Existing deferred Welford
relative-flat/OU concerns are not claimed closed by this batch. No bytecode,
opcode, order-statistic or global scheduler changes were necessary.

## Owning qualification (pending)

Both new files belong to `atx-engine-alpha-tests`:

- `atx-engine/tests/alpha/alpha_ts_date_major_test.cpp`: 4 tests for delay/delta
  independent-cell results, unaligned partitions/tails, warmup/window bounds,
  signed zero/NaN payloads, overlap safety, SIMD sum versus scalar compensation,
  and unary tile/worker identity.
- `atx-engine/tests/alpha/alpha_ts_sliding_routing_test.cpp`: 4 tests for actual
  compiled VM pair/exp routing, oracle tolerance and AuditExact bits, exact batch
  versus streaming, factors tiny/0.9/near-one/one/1.5/2, gaps, finite overflow and
  recovery, future mutation, and explicit legacy reproduction.

Focused filter:
`AlphaTsDateMajor_*.*:AlphaDecayExpO1_*.*:AlphaPairRouting_*.*`.

Only `git diff --check` has passed locally. Independent source review is ongoing;
its two reported pair-overflow findings were repaired in the frozen source.
New fixtures have not compiled or run. Existing relevant alpha oracle, sliding,
streaming and worker-determinism suites remain owning qualification checks.
The W1-X1 adapter integration remains with its owner; local future-mutation
fixtures do not substitute for shared harness registration or a positive control.

All original quiet-host Release gates remain unmeasured: per-cell delay/mean/exp/
corr thresholds, eight-worker scaling and strategy-B cold speedup. No long
benchmark has been launched, no numerical acceptance weakened, and **W1-A1 is
not marked complete** by this bounded source freeze.
