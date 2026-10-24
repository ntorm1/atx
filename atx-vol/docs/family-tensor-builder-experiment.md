# Family tensor builder experiment (2026-10-24)

Decision: retain the existing AVX2 Reference builder. The existing scalar
accurate-Fast builder preserves the captured numerical validation, but its
isolated preparation timing differs by only a few milliseconds and the ordering
reverses across processes. A production arithmetic/profile seam is not justified
by this result.

This is an isolated experiment, not a production change or a complete-fit speed
claim. The temporary exporter and CMake target were removed after saving their
sources, inputs, executable and receipts.

## Why a profile switch is not sufficient

`AlSigmaQFamily::prepare` uses AVX2 when the resolved scheme has Reference
arithmetic and the BAW seed. The vector kernel already uses vector Cody normal
probabilities and vector log/exp. Selecting `al_fast_profile` instead invokes the
scalar prepared boundary solver. It also changes the family's evaluation
workspace and exact native-option identity. A one-line profile switch would
therefore change more than the tensor builder.

The replay compares these existing APIs with identical numerical orders,
iteration budgets, tolerance, boxes and node counts. For a builder-only price
comparison, it interpolates the Fast tensor into caller-owned boundary scratch
and prices that boundary through the original Reference family's workspace.
Neither prepared family is mutated; native identity checks are not bypassed.

## Exact inputs and correctness

Source `71c6f97f8e9131862b670d2f95c8250388bcee6e` temporarily exports the
contracts from the full SPY finalizer fixture with `deam_family = Always`.
The optional fixture passes for all 33 slices. The capture has 63 families,
3,392 tensor nodes, 40,704 transformed-boundary values and 567 original scalar
Reference validation probes. This includes the two sparse-expiry families
omitted by the earlier Adaptive-policy experiment. All family options resolve
to Reference 12/24/48/6 with tolerance 1e-10. Relevant family/error/kernel source
files match the integrated `126447ef` candidate exactly.

The standalone replay validates group identities, exact nine-probe joins,
resolved options, statuses and expected denominators before work. It retains
every tensor value and every probe result, including potential failures.

| Check | Result |
| --- | ---: |
| Failed preparations | 0 / 63 |
| Rebuilt Reference price/bound/cold-price bit mismatches | 0 / 567 |
| Nonidentical Reference-AVX2 vs scalar-Fast tensor values | 36,638 / 40,704 |
| Maximum absolute transformed-boundary difference | 6.025388521457842e-13 |
| Differences above existing 1e-12 node parity allowance | 0 |
| Envelope status/coverage changes | 0 / 63 |
| Maximum Fast-workspace probe price difference | $7.389644451905042e-13 |
| Maximum Fast-tensor/Reference-workspace price difference | $6.252776074688882e-13 |
| Fast or builder-only probe violations | 0 / 567 each |
| Builder-only probe acceptance changes | 0 / 567 |

The bound expression depends on the tensor and geometry, not the pricing
workspace's arithmetic-profile flag. The builder-only diagnostic compares its
Reference-workspace price with that tensor's computed bound. These are sampled
comparisons under the existing empirical refinement/parity class. They do not
establish a new uniform bound, prove scalar-Fast tensor agreement with every
scalar-Reference target, or qualify a production builder substitution.

## Isolated timing

The standalone executable was compiled PCH-off with `vol-rel-avx2` (Release,
AVX2, ThinLTO). Three fresh processes pin to logical P-core 6, each running seven
alternating pairs; the second process starts with Fast. Every repetition
constructs fresh geometry and a fresh family, allocates the tensor, solves all
nodes, hashes all tensor values and destroys the objects. The unchanged
Reference validation probes are excluded from this preparation-only clock.
Both routes include identical setup/output accounting.

| Process | AVX2 Reference median ms | Scalar accurate-Fast median ms | Reference minus Fast ms | SMT sibling % |
| --- | ---: | ---: | ---: | ---: |
| 1 | 148.9325 | 142.4799 | 6.4526 | 1.5267 |
| 2 | 138.9608 | 142.0173 | -3.0565 | 0.7937 |
| 3 | 145.5762 | 139.1076 | 6.4686 | 1.5748 |

All 21 measured repetitions per route have stable tensor digests and no
preparation refusals. Every native receipt passes the below-5% sibling gate.
The harness is a serial loop and invokes no fitter or thread pool. Its generic
affinity receipt reports the unused auto-fit-worker default of 16 because
`ATX_VOL_FIT_WORKERS` was unset; that field must not be read as an actual worker
count. Process/thread affinity is `0x40`. These receipts are not complete-fit
qualification receipts.

The archived executable SHA256 is
`92ddbba7c7541421eab5d1f78bb3bc335215946aefbe94e9854b01c79de9c49b`.

## Next bounded experiment

The current vector builder reconstructs T/grid/quadrature-only barycentric
coefficients for every four-node pack. On this corpus, 848 packs each construct
264 pairs with 12 divisions per pair, although those coefficients are identical
within each family. Retaining that invariant geometry for one preparation would
reduce approximately 2.69 million divisions to 0.20 million without changing
the intrinsic arithmetic. Weighted rate/yield exponentials may also be reusable
within each q node; volatility-dependent factors remain per pack. This is a
source hypothesis, not an implemented or measured optimization. It requires
explicit initialization/invalidation and bitwise tensor/probe regressions.

## Evidence

Artifacts are under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/`:

- `tensor-capture-v1.patch`, `tensor-family-contracts-v1.csv`,
  `tensor-family-probes-v1.csv`, `tensor-fixture-v1.json` and
  `tensor-capture-v1.xml`: exact exporter, contracts, hashes and passing capture.
- `family-tensor-replay-v1.cpp`, `tensor-replay-cmake-v1.patch`,
  `tensor-replay-build-v1.log` and `family-tensor-replay-v1.exe`: standalone
  source, temporary target, PCH-off/ThinLTO build and archived executable.
- `tensor-audit-v1.json`: all compared tensor values, probe results and decisions.
- `tensor-timing-trial-{1,2,3}.json`, `tensor-timing-summary-v1.json` and
  `tensor-manifest-v1.json`: all repetitions, process identities, native affinity
  and CPU-activity receipts, source/data/executable hashes.
