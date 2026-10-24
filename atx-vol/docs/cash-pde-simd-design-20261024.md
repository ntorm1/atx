# Cash PDE: heterogeneous-contract SIMD investigation

Status: source and work inventory only. No SIMD pricing implementation, latency
claim or cash-price qualification is supplied by this report. The source is
`cec21ad92718a1ce4a7a880fcb43d5a399cfbdf9`; its complete 50-contract, three-grid
replay preserves all 150 scalar outcomes. The finest grid still has 43/44 passing
spot-liquidator prices and one uncertain DSPYp comparison. The other six cash
models remain separate. See [the cash frontier](cash-d0-d1-next-experiment-20261024.md).

## What can run together without equal coefficients

`src/pricing/american_discrete_div_reference.cpp::thomas_solve` has a dependency
along grid rows, but no dependency between contracts. A four-lane binary64 vector
can hold the same row from four contracts. Each lane retains its own `lo,di,up`,
active mask, obstacle, RHS, `cp`, `dp`, work values and boundary values. Equal
volatility, strike, carry, grid spacing or coefficients are unnecessary.

The scalar recurrence is the contract:

```text
m[i]  = d[i] - l[i] * cp[i-1]
cp[i] = u[i] / m[i]
dp[i] = (rhs[i] - l[i] * dp[i-1]) / m[i]
x[i]  = dp[i] - cp[i] * x[i+1]
```

An active obstacle row and the terminal boundary select identity coefficients;
their original signed-zero arithmetic remains part of the parity check. Use the
same divisions and the compiler's verified scalar multiply/subtract contraction.
Replacing division with a shared reciprocal, changing reduction order or using
cyclic reduction is a different numerical experiment. No such change is proposed.

`solve_lcp` seeds each fresh segment, selects a policy, solves and independently
checks each lane. A converged lane must retain its last accepted values while
other lanes continue. It must not adopt the slowest lane's policy or execute extra
committed Howard iterations. The scalar `changed`, worst-residual, tie count,
maximum-iteration/time and refusal rules remain lane-specific. Masked retired
lanes need safe arithmetic operands and must not update diagnostics or state.

## Declared schedule inventory

Two input-only groupings were declared. Both require equal actual space count,
exercise style, Rannacher count, grading and segment step counts. The first also
requires exactly equal canonical maturity/interior-event time bits. The second
permits different times in each lane, preserving the same segment loop shape.
Event amounts and all numerical model inputs remain untouched. Within each group
the original manifest order is split into groups of four. Neither prices nor
observed iteration counts choose a group or order.

The analysis reconstructs canonical event admission/coalescing and the existing
segment-step formula from original inputs. Its step counts match every native
replay record. Actual per-step Thomas counts then quantify padding. This is not
an implemented batch scheduler.

| Grid | Exact-calendar groups | Useful lanes | Same-step-count groups | Useful lanes |
|---|---:|---:|---:|---:|
| 400 x 200 | 17 | 52.78% | 14 | 60.37% |
| 800 x 400 | 17 | 52.39% | 16 | 54.77% |
| 1600 x 800 | 17 | 52.03% | 16 | 54.36% |

At the finest grid, same-step-count grouping turns 45,688 scalar Thomas sweeps
into 21,012 four-lane sweep instructions. Of 84,048 issued lane-sweep slots,
29,418 are unused tails and 8,942 belong to lanes already converged. The
arithmetic-count ceiling is therefore 2.17x, before extra masking, packing,
instruction throughput and scalar work. Exact-calendar grouping gives 2.08x.
For the 44 spot-liquidator cases the corresponding ceilings are 2.13x and 2.13x
after rounding. These are not runtime predictions or speedup measurements.

The current record does not contain each sweep's individual mask positions. It
cannot quantify how often the four lanes disagree at a row or establish that
every possible exercise pattern is a single interval. The earlier one-run
observation is fixture evidence only. Arbitrary alternating/fragmented masks
remain required kernel tests.

Evidence: `calendar/cash-pde-simd-inventory-v1/inventory.py` and `receipt.json`
under `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`. The script pins the
complete source replay SHA, retains all 50/44 denominators and reports both tail
and converged-lane overhead. It performs no native pricing or timing.

## Layout, setup and scalar boundaries

Keep a small group in an array-of-structures-of-arrays layout: each grid row has
four adjacent values for each state array. The current state has seven binary64
arrays plus one jump scratch array and an active-byte array. At 1601 nodes, four
fully populated lanes require 416,260 payload bytes, before metadata, temporary
jump output and allocator overhead. The `cp`/`dp` pair alone needs 102,464 bytes.
This does not fit in a small L1 data cache. Actual cache behavior must be measured;
there is no evidence that arithmetic savings dominate layout traffic.

Cold timing must begin before validation, event canonicalization, grouping,
allocation, grid/domain construction, exponentials and terminal cell averages.
It must include RHS/obstacle formation, every policy solve, masked work, original
jump processing, final derivatives/interpolation, output unpacking and refusal
handling. A hot Thomas-only kernel benchmark is an additional attribution result,
never the complete batch throughput result.

Initially retain scalar per-lane setup, far-boundary calculations and cash jumps.
They depend on each model, strike, rights and values; equality of a loop shape
does not justify sharing their results. Preserve every source arithmetic order.
Keeping the working curves interleaved throughout a segment avoids full
pack/unpack traffic on every Howard sweep. Jumps can gather one lane into an
owned scratch span and scatter it back, with those costs included. A later
strided scalar jump seam is a separate change and requires its own parity proof.

## Bounded prototype and gates

1. Implement a private four-lane Thomas primitive and unchanged scalar fallback.
   Test heterogeneous diagonally dominant rows, both boundary signs, zero signs,
   all/none/alternating obstacle masks, tails, very short grids and malformed
   inputs. Compare every `cp`, `dp` and solution bit, not just the final price.
   Verify the actual scalar contraction emitted by the configured compiler.
2. Capture a bounded, predetermined set of real scalar sweep inputs and outputs
   covering every case/grid and fresh/Howard paths. Preserve model IDs and refusal
   evidence. A capture is diagnostic only; it must not become runtime input or
   select favorable contracts. Avoid retaining every full-grid sweep: that would
   create unnecessary gigabytes of evidence.
3. Add private owned batch state with exact-calendar grouping first. Each lane
   keeps independent Howard continuation, stopping and typed failure. Feed the
   unchanged scalar jump/setup routines. Require all 150 result and policy-work
   fields to match the original replay, plus small generic regression cases.
   Record active/tail/retired lanes and actual mask disagreement.
4. Only after full parity, compare complete cold scalar and batch calls on the
   same frozen inputs and binary, one pinned core, fresh owned workspaces. Keep
   every run, refusal and empirical price outcome. Use paired alternating runs
   with sibling-idle checks and report allocation/packing/jump costs. Do not
   declare an all-case accurate cash route while DSPYp is unresolved.

The broader same-step-count grouping is a later controlled follow-up if its
small occupancy improvement justifies more lane-specific time bookkeeping.
Asynchronous cross-schedule compaction is not part of this bounded proposal.

## Primary research and its limits

László, Giles and Appleyard describe CPU SIMD Thomas solvers across independent
systems with different coefficients, including local transposition and four
binary64 AVX lanes. Their cache discussion explains why temporary-array size and
layout matter. This motivates an interleaved-contract experiment; it supplies no
speed estimate for this Howard/cash workload. Their reciprocal-based pseudocode
is not a bit-preserving substitute for the current scalar divisions.
[Manycore Algorithms for Batch Scalar and Block Tridiagonal Solvers, 2016,
sections 2.1, 5 and 7.2.1](https://people.maths.ox.ac.uk/~gilesm/files/toms_16b.pdf).

Akkurt and coauthors describe small groups that store the same-index entries of
independent systems adjacently, making each forward/backward step vectorizable
while improving cache locality. That layout idea applies here. Their distributed
diagonally dominant solver, preprocessing assumptions and large-scale CFD
throughput do not establish cash-option accuracy or single-core performance.
[A Distributed-memory Tridiagonal Solver, 2024 version 1, sections 3 and 4.1](https://arxiv.org/html/2411.13532v1).
