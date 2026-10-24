# Exact family tensor geometry reuse (2026-10-24)

The AVX2 family builder now retains identical time/grid/quadrature coefficients
within its existing preparation scratch, and retains weighted rate/yield
exponentials while the four-lane carry key is unchanged. It keeps the original
intrinsic arithmetic, numerical options, boundary seeds, iteration controls,
domain guards and fallback paths. Volatility-dependent factors are rebound for
every pack.

This follows the negative scalar-Fast profile experiment in
[family-tensor-builder-experiment.md](family-tensor-builder-experiment.md).
The Reference builder and Reference validation remain in use.

## Ownership and invalidation

`NodePackScratch` belongs to one `AlSigmaQFamily::prepare` call. Its explicit
ready flags initialize to false even under `make_unique_for_overwrite`; large
coefficient arrays are filled before read. No prepared pricing state, shared
cache, global identity or public option changes.

The invariant key compares the actual initialized values bit-for-bit: expiry,
boundary/quadrature counts, collocation times, nodes, barycentric weights and
quadrature nodes/weights. A mismatch rebuilds every invariant coefficient and
invalidates the carry cache. The carry key compares all four rate and yield
values bit-for-bit. It does not depend on strike or volatility, which do not
enter those retained expressions. Unsupported shapes still use the original
inline-geometry kernel. An entirely refused pack returns without publishing
new cache contents.

The retained expressions use their original vector instruction sequences.
Lane-uniform intermediate values are stored as doubles and broadcast back
without arithmetic reassociation. Node-solve results, classification and
scalar retries are unchanged. The scratch grows by roughly 12 KB and has no
additional allocation; it is still part of the existing heap allocation for
one preparation.

## Regression and exact replay

The new regression failed at runtime before reuse was implemented. It compares
fresh and reused scratch masks and every returned boundary-node bit across
independent changes in strike, volatility, yield, rate, expiry, boundary count,
quadrature count and solve controls. It also exercises a changed reference
lane, an entirely invalid pack, an unsupported hoist shape and return to a
previously supported shape. Work counters verify that only the relevant
coefficients rebuild.

The focused `AlSigmaQFamily` and `AmericanFamilyError` suites report 30 passes
and one explicitly opt-in timing-test skip. The changed kernel, consuming
family source and test translation units compile with PCH disabled. The
standalone ThinLTO replay compares the exact current Always-policy SPY corpus:
63 families, 3,392 nodes, 40,704 transformed-boundary values and 567 original
Reference probes. Every Reference tensor value is bit-identical to the frozen
pre-change artifact. All probe price/bound/cold-price records and acceptance
decisions are identical. No validation probe is removed.

## Isolated timing

Three pairs of fresh processes run archived baseline and candidate executables,
reversing their order in pair 2. Each process runs seven alternating Reference
and scalar-Fast control passes. Every pass includes fresh geometry, tensor
allocation, all node solves, digest and destruction; unchanged validation is
excluded. This is preparation-only timing, not complete-fit qualification.

| Pair | Baseline Reference median ms | Candidate Reference median ms | Saved ms | Baseline / candidate unchanged Fast control ms |
| --- | ---: | ---: | ---: | ---: |
| 1 | 141.0566 | 133.4631 | 7.5935 | 138.8169 / 138.2109 |
| 2 | 148.9702 | 133.7929 | 15.1773 | 143.8945 / 144.5084 |
| 3 | 142.1452 | 132.8717 | 9.2735 | 140.7935 / 141.0340 |

All six receipts have distinct PID/creation-time identities, P-core 6 affinity,
`ATX_VOL_FIT_WORKERS=1`, no preparation refusals and identical Reference tensor
digests. Five report 0% SMT sibling activity; the first baseline reports 0.79%.
The unchanged control varies by at most 0.62 ms within a pair. These runs show a
consistent modest preparation gain; an integrated full-input audit and whole
fit measurement remain necessary before assigning that gain to cold fitting.

Baseline executable SHA256:
`92ddbba7c7541421eab5d1f78bb3bc335215946aefbe94e9854b01c79de9c49b`.
Candidate executable SHA256:
`4396afb4378f132233432bb3ea3cd61d7f91ad93fc4a97ce3c3051d542426c71`.

## Evidence

Artifacts are under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/`:

- `tensor-hoist-red.xml`, `tensor-hoist-final.xml`,
  `tensor-hoist-final.log` and `tensor-hoist-hygiene.log` contain runtime
  red/green and owning PCH-off evidence.
- `tensor-fixture-v1.json`, `tensor-audit-v1.json`,
  `tensor-hoist-audit-v1.json` and `tensor-hoist-parity-v1.json` retain exact
  inputs and all tensor/probe comparisons.
- `family-tensor-replay-v1.cpp`, `family-tensor-replay-v1.exe` and
  `family-tensor-hoist-v1.exe` retain the common harness and both binaries.
- `tensor-hoist-timing-{baseline,candidate}-{1,2,3}.json` and
  `tensor-hoist-timing-summary-v1.json` retain every repetition and receipt.
- `tensor-hoist-source-v1.patch` and `tensor-hoist-manifest-v1.json` bind the
  exact source changes, compiler commands and artifact hashes.
