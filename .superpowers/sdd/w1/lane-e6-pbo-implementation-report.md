# W1-E6 PBO scalability: source freeze, qualification pending

This bounded slice implements PBO caching and linear winner ranking. It does not
complete the other W1-E6 items or claim the 50x performance gate.

## Frozen source and postimplementation fixtures

- `1b1158132f1d796e14e52ca688d4aa13eb9e754d`: cached block moments, linear winner
  rank, explicit numerical versions, checked used-input/reference-score domain;
  shared unchanged bounded combinatorics in a separate lightweight header.
- `23e7a8b0ffca831f23daa859a546266c32dbf0b6`: independent review found that normal
  raw scale can still produce subnormal variance or an underflowed error bound.
  Both regimes now require reference evaluation.
- `acb89b8e6bdc717ceb0a66c703c1247c56aa0d9a`: four postimplementation tests in
  `atx-engine/tests/eval/eval_pbo_test.cpp` (11 total tests in that file).

The prior root guard commits `84460488` and `e8adfe4a` were imported locally as
`d46586ed` and `f9171a0b`. The root already owns their CMake target; do not import
the local CMake conflict-resolution ancestry back into the root branch.

## Algorithm and reproduction contract

`PboRule::LegacyGatherV1` preserves the original ascending gather, ordered
two-pass mean/population deviation, first maximum, and stable ascending OOS
ranking for valid inputs. `CachedMomentsV2` is the deliberate new API default.
Persist the rule when reusing research artifacts; a default argument alone is
not durable recipe binding. Names are `legacy-gather-v1` and `cached-moments-v2`.

V2 builds one candidate-by-block cache of centered sum, square sum, and maximum
absolute input. IS and OOS each combine their own selected blocks; OOS never
subtracts whole-sample totals. Per split, the winner is selected in O(N) and
only its OOS rank is counted in O(N), including the original candidate-index tie
order. Cache and scratch allocate once; the V2 split loop does not allocate.

The ordinary cost changes from O(C*N*T + C*N*log(N)) to
O(N*T + C*N*S), where C=choose(S,S/2). Reference fallbacks add their original
ordered gather cost. Degenerate or near-tied populations can therefore remain
expensive; no worst-case speedup is promised. Owned auxiliary memory is
O(N*S + N + T + C), excluding the caller's matrix.

Grouped reductions change floating-point arithmetic. Each estimate carries a
conservative roundoff guard; unstable/cancelled moments, nonfinite intermediates,
subnormal variance/error envelopes, and ambiguous IS or winner-versus-OOS rank
comparisons use the frozen reference arithmetic. These guards are a versioned
numerical policy, not a theorem of universal bit equivalence. The explicit V1
path remains available for exact old valid-domain reproduction.

`PboResult` appends `rule`, `cached_evaluations`, `reference_evaluations`, and
`ambiguous_comparisons`. Existing three-field aggregate initialization remains
valid. Evaluations count one candidate/side/split; attempted cached evaluations
include those later recomputed by reference. The diagnostic counters do not
alter split order or inferential output.

Both rules validate the rectangular matrix, N>=2, even 2<=S<=16, S<=T, cache and
counter sizes, and finite USED returns. Original row stride T is retained;
only T_used=(T/S)*S values per row are consumed, and trimmed tails are not even
validated. A nonfinite resulting reference Sharpe returns InvalidArgument.
Finite reference zero produced by overflowed deviation is preserved. The old
NaN comparator domain was invalid and is intentionally rejected. The unchecked
wrapper now documents all checked preconditions, including the S bound.

`eval/combinatorics.hpp` keeps the existing recurrence and subset walk so CPCV
can stop including the entire PBO API. Its corrected comment explicitly requires
multiply-before-divide intermediates to fit; PBO's S<=16 satisfies this bound.
The root owns the CPCV include switch.

## Qualification boundary

The four new fixtures compare complete logit vectors, PBO, and mean-logit bits
with an independent frozen gather/stable-sort oracle (no production PBO helpers).
They cover normal cached inputs, trimmed row stride, near-tie IS winner and OOS
rank boundaries, exact ties, large offsets, constant values, overflowed variance,
subnormal variance/error bounds, nonfinite used inputs, undefined reference
scores, and nonfinite trimmed tails. They also inspect actual cache/fallback
counters. Owning target: `atx-engine-w1-eval-tests`; filter: `EvalPbo.*`.

Only source inspection and `git diff --check` have run in this lane. No C++
compile, fixture execution, benchmark, real data read, or database access has
occurred. Runtime correctness, scoped header hygiene, and the original
N=2000/T=2520/S=16 >=50x acceptance remain pending. At those dimensions the
documented rule consumes 2512 periods and trims eight; a future benchmark must
compare the same rules/input, preserve flags, report fallback counts, and not
substitute a smaller workload for the performance requirement.

## Production identity integration

The parent authorized the following narrow follow-up after this source freeze:
factory `finalize_run_pbo` and FactoryConfig/FactoryReport; `NnGateCfg` and actual
sweep consumers; explicit V1 for the fixed validation audit; and impl CLI,
discover persisted configuration/fingerprint/report forwarding. V2 must not
silently resume artifacts created under V1. This follow-up is separate from the
kernel freeze and is not yet represented as completed by this report.
