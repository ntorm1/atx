# Fixed-domain spatial and time diagnostics

Base: storage-integrated `45756049154585b2be731921c2582409bdf5dc93`, plus parent
integration `ba63157a2fb38a547db111a1e9b0aafe3f941e98` (local pick `a4dfa99b`).
This lane adds seven synthetic audit profiles, per-interval time provenance, and
four postimplementation CLI checks. It changes no pricing kernel, production
policy, mathematical constraint, or resource cap. Vendor labels are not inputs.

The new profiles keep the upper domain at eight times spot and use the existing
first-positive-variance concentration rule:

- `sinh161-variance-domain8` and `sinh193-variance-domain8` increase node count
  with the existing clock limits and CFL fraction.
- `sinh129-domain8-time2` and `sinh129-domain8-time3` hold the spatial grid fixed
  while dividing both clock limits and the CFL fraction by two or three.
- `sinh129-domain8-clock4800`, `sinh161-domain8-clock4800`, and
  `sinh193-domain8-clock4800` use maximum calendar and variance steps of 1/4800.
  The explicit CFL constraint still applies. These names specify a maximum step,
  not a promise that every fixture uses exactly 4800 steps.

Every successfully compiled case now reports, per interval, calendar-clock and
variance-clock minimum step counts, realized diffusion steps, and whether the
realized count exceeds the clock minimum. Counts and the declared interval
endpoints determine the exact linearly subdivided dual-clock schedule. Refused
cases preserve requested contracts and controls without inventing a resolved
schedule.

## Validation

Release AVX2 audit build, owning PCH-off translation-unit compile, Ruff, and
`git diff --check` passed. Eleven CLI pytest checks passed without skips. The four
new checks verify identical high-volatility time subdivisions across spatial
grids, fixed grids under time refinement, realized CFL changes, unchanged caps,
complete contracts, and reconciliation of cash/paused-clock interval counts.

The parent storage seam was temporarily applied for validation, then removed
before picking its committed integration. The small overlap was resolved to
retain both the parent's span serialization and this lane's time metadata. The
resulting source hash exactly matches the already-built and validated source.

Only the seven new profiles ran across all eleven fixtures. The eleven existing
129-node/domain8 baseline cases were reused from the immutable v3 audit. All 88
cases were checked against their declared expiry/strike/side/style universe;
every successful case is complete and unique. Maximum scalar/batch discrepancy
is 2.558e-13. The 129-node clock4800 profile compiles eleven fixtures; every other
new profile compiles ten and explicitly refuses `near_cash`. Its 288 requested
rows remain in coverage denominators. No cap was raised to obtain a result.

## Spatial error at fixed domain and time

For all eighteen broad American reference contracts, the three clock4800 grids
have exactly equal time schedules: `funded` uses 1200 steps, `separate_clocks`
2400, and `high_vol` 4800. All eighteen reference signatures are observed stable
after the explicit high-volatility replacement, and are retained on every grid.

| Nodes | High-vol rows | High-vol largest European error | High-vol six American MAE | All eighteen American MAE |
| ---: | ---: | ---: | ---: | ---: |
| 129 | 619200 | 0.00609818 | 0.00155661 | 0.00107416 |
| 161 | 772800 | 0.00278530 | 0.000855747 | 0.000592206 |
| 193 | 926400 | 0.00209940 | 0.000577866 | 0.000394906 |

High-vol upper-boundary mass remains about 0.003093. These controlled differences
demonstrate a remaining spatial-resolution contribution at fixed domain and
time. They do not prove convergence or bound the remaining tail, time, or
reference error. Against the same eighteen contracts, the 193-node profile
improves sixteen prices and ties two relative to 129 nodes; none worsens.

Other fixtures do not all share identical schedules: `clock_pause` and
`short_low_vol` remain CFL dominated; the 193-node `dated_cash` grid also requires
additional steps. Their results must not be described as purely spatial changes.
The ATM comparison pairs twenty contracts because four near-cash references are
unavailable. Its paired 129/161/193 clock4800 MAE is
0.00136847/0.000580350/0.000421153, with explicit baseline-only coverage counts.

## Time refinement and error cancellation

At fixed 129-node/domain8 spatial grid, high-vol step counts are 1961, 3922, and
5882 for baseline/time2/time3. Largest European errors change only slightly:
0.00613112, 0.00610327, 0.00609400. In contrast, six-contract American MAE rises
from 0.000430172 to 0.00134550 and 0.00172987. The unchanged grid and retained
reference signatures make this direct evidence of competing discretization
errors; a smaller coarse-grid aggregate is not a convergence certificate.

Increasing nodes with CFL-determined time steps also changes time. The
129/161/193 high-vol step counts are 1961/3038/4412; their American MAE is
0.000430172/0.000697758/0.000564843. That nonmonotonic aggregate should not choose
a production grid. The fixed-time experiment above is more informative about
the spatial component.

The next numerical work can compare the opt-in positive resolvent under the same
fixed-domain controls, separately refining deterministic exercise/clock steps.
Preserve explicit refusals and exact contract intersections. No speed claim or
policy promotion follows from these concurrent-host synthetic measurements.

## Immutable artifacts

All paths are beneath `C:/atx-cache/quote-benchmark/`. Source artifact provenance
records reused baseline path/hash, exact commands, stdout hashes, binary/source
hashes, and diagnostic elapsed time. Independent references retain original and
explicit refinement provenance and uncertainty; observed stability is not proof.

- `markov-audit-20261016-domain8-fixed-v1.json`: SHA256
  `0c4ced6d4e85476edb7fd775c96bd2ba856cce5055e05c69f8dbda0d0775f51c`.
  Audit source SHA256
  `929dfd45c6e5a0edbe6eb39e420859d522c20e0412c19872fbaa66db5204f535`;
  executable SHA256
  `d5965c51c0ca3913b60266cfb656f1dca32e2faa5bd8c98f17c993ff600fbe41`.
- `common-broad-domain8-fixed-compare-20261016-v1.json`: SHA256
  `78a03428f64a21ad4a310e151af12a454f47e7f323251a157dad18f47f7833ec`.
- `common-atm-domain8-fixed-compare-20261016-v1.json`: SHA256
  `3bea0ad2597ba7a33661f8478aecf645678b0e6ef7f0a7e835b486927915abee`.
- `common-broad-domain8-spatial-compare-20261016-v1.json`: SHA256
  `8acf5a36ec37533f3fa0376db68c676402c57e43c2294d544b5146ae9a5fc89f`.
- `common-atm-domain8-spatial-compare-20261016-v1.json`: SHA256
  `5da21f0127ed0a6837161fce3c2c7fff4a539a359b0378484fd24f65302a9ae5`.

The fixed comparisons use baseline `sinh129-variance-domain8`; the spatial
comparisons use `sinh129-domain8-clock4800`. Each report retains 20736 requested
rows across eight grids, including every refusal and unmatched reference row.
