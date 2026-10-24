# Family validation probe batch experiment (2026-10-24)

Decision: retain scalar validation probes for now. A batch with conservative scalar
retries preserved every captured SPY family decision, but saved only 10–13 ms in
the isolated replay. The extra empirical parity assumption and retry machinery do
not justify prioritizing this over the remaining short-expiry inversions.

This is an experiment, not an integrated production change or a full-fit speed
claim. The temporary exporter, replay test, and CMake target were removed after
their exact sources and receipts were saved.

## Exact input and correctness

The frozen source was `aa9daa822215591254cb2c83338187df21e2dc8c`. A temporary
exporter in `deam_family_certify` captured all 549 validation contracts from the
full 33-expiry SPY finalization: 61 families, the original nine points per family,
unchanged Reference options, family prices and bounds, and scalar cold prices.
The source retained the random, cell-midpoint, and near-exercise probes.

The replay transformed calls through put-call symmetry and used the existing
`american_put_boundary_batch` Reference implementation in groups of nine. Each
group used two four-lane packs and one scalar tail. All 549 recomputed scalar
prices matched the captured price bits. The greatest AVX2/scalar discrepancy was
$1.5347723092418164e-12, or 1.0719654565680378e-13 relative to
`max(abs(scalar_price), 1)`.

A batch value was usable only when finite and

```
abs(family_price - batch_price) + 1e-11 * max(abs(batch_price), 1) <= family_bound
```

Every other point retried the original scalar cold pricing call. This margin is
an empirical scalar/AVX2 parity class, not a new rigorous numerical error bound.
It caused 151 retries (27.5%). There were zero original violations, zero apparent
batch violations, zero discrepancies outside the tested parity class, and zero
changed final family acceptance decisions. No probe or bound check was removed.

## Isolated timing

The temporary native executable was built with `vol-rel-avx2` (Release, AVX2,
ThinLTO). Three separate processes pinned to logical P-core 6 each ran nine
alternating scalar/batch pairs. Every native receipt reported one fit worker and
0% SMT sibling activity. The timing includes packing, batch prices, retry
decisions, all 151 scalar retries, and result storage. It excludes the unchanged
envelope construction and bounded-family price calls. These are repeated probe
replays within each fresh process, not fresh-process full fits.

| Process | Scalar median ms | Batch plus retries median ms | Difference ms |
| --- | ---: | ---: | ---: |
| 1 | 62.8770 | 50.0916 | 12.7854 |
| 2 | 61.8107 | 49.6585 | 12.1522 |
| 3 | 60.6565 | 50.3179 | 10.3386 |

The experiment binary SHA256 was
`9c74abaaa16195f8535bf74afc76954d1f5145291d03dbb941995e01d489173c`.
No result from this experiment changes the shipping family-validation code.

## Evidence

All artifacts are under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/`:

- `family-probes-capture-v1.patch`, `family-probes-scalar-v1.csv`, and
  `family-probes-scalar-v1.json`: exporter, original contracts, source/data hashes.
- `family-probes-capture.xml` and `family-probes-replay-v1.xml`: passing full SPY
  capture smoke and exact scalar/batch correctness replay.
- `family-probes-replay-v1.patch` and `family-probes-replay-v1.json`: exact replay
  source, per-point prices, errors, bounds, and retry decisions.
- `family-probes-timing-v1.cpp`, `family-probes-timing-cmake-v1.patch`, and
  `family-probes-timing-build-receipt-v1.json`: exact timing source/target,
  source/binary hashes, and compiler configuration.
- `family-probes-timing-trial-{1,2,3}.json` and
  `family-probes-timing-summary-v1.json`: every measured repetition, process
  identity, affinity/topology, and per-CPU activity receipt.
