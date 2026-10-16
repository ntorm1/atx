# Controlled spatial and time audit experiments

Base: `eca684f1a57e01769ac1e7d757c8ed8cda3a5bba`. The audit executable now exposes
four additional synthetic diagnostic profiles under `--full`; production model
defaults, mathematical constraints, and resource caps are unchanged. No vendor
labels or quote-chain data select these profiles.

`sinh-match-domain8` and `sinh-match-domain16` use the same first positive synthetic
variance scale as `sinh129-variance`, including the explicit fixed fallback when
variance is zero. A bounded ascending search from 129 through 1025 nodes takes the
first generated grid whose lower and upper spot gaps are each no larger than the
129-node/domain-4 reference. Integer left/right node allocation makes individual
gaps nonmonotone, so binary search would be unjustified. Reference/candidate grid
failures and search exhaustion are explicit refusals.

`sinh129-variance-time2` and `sinh129-variance-time4` keep the spatial grid fixed,
divide both maximum clock steps and the CFL fraction by their factor, and preserve
every storage/work cap. Dividing only the maximum steps would not refine schedules
already dominated by CFL. JSON records requested factors, configured/effective
clock steps, resolved controls, exact resolved state grids, reference gaps, node
search bounds/counts, and all requested contracts even on model refusal.

## Validation after implementation

- Release AVX2 audit target built through the leased-tree build wrapper.
- Seven native CLI pytest checks passed, with no skips and the binary explicitly
  configured: matched gaps, grid validity, requested contract completeness,
  unchanged caps, fixed spatial grids under time refinement, realized CFL step
  refinement, resource refusal provenance, deterministic fallback, and unknown
  profile rejection.
- Owning translation unit compiled with the PCH-off `vol-hygiene` preset.
- Ruff and `git diff --check` passed.

Only the four new profiles were executed across eleven fixtures. The eleven
baseline cases were reused from the immutable v3 audit, avoiding a redundant full
rerun. Each successful case produced exactly its declared expiry/strike/side/style
universe. Both wider matched grids compiled all eleven fixtures. Time2 refused
`near_cash`; time4 refused `near_cash` and `high_vol`, preserving those cases in all
coverage denominators.

## Numerical evidence

For `high_vol`, the controlled profiles separate the earlier wider-domain
coarsening effect from the domain experiment:

| Profile | Nodes | Diffusion steps | Stored rows | Upper boundary mass | Largest European exact error |
| --- | ---: | ---: | ---: | ---: | ---: |
| Reference domain4 | 129 | 3113 | 401577 | 0.0392899 | 0.0884910 |
| Matched domain8 | 164 | 3143 | 515452 | 0.00309262 | 0.00454857 |
| Matched domain16 | 196 | 3162 | 619752 | 0.000123031 | 0.00455231 |

The large domain4 error falls with domain8; domain16 reduces boundary mass again
without reducing the largest European error. The observed plateau is consistent
with remaining spatial/time error and does not establish a rigorous tail bound.
These grids match two local gaps, not their entire spatial operator. Both domains
retain all requested contracts, including cash and separate-clock fixtures.

Against the saved 24 observed-stable ATM American references, MAE was
0.000476122 for the baseline, 0.000486355 for matched domain8, and 0.000485391 for
matched domain16. All 24 exact signatures are retained. That reference set excludes
`high_vol`, `funded`, `separate_clocks`, and `deterministic_cash`; it cannot establish
the high-volatility American improvement.

Time refinement exposes error cancellation rather than an automatic improvement.
For `negative_rate`, diffusion steps 600/1199/2398 yield largest European errors
0.00159986/0.00193556/0.00210356. On the common 20 American ATM signatures surviving
both refinements, MAE rises from 0.000491582 to 0.000741880 and 0.001002875. Each
refinement improves 3 and worsens 17 prices. Four baseline reference contracts
become unavailable because of the unchanged near-cash resource cap. Time4 also
loses all high-volatility contracts, so its aggregate cannot be compared without
the explicit paired intersection.

Parent-produced independent broad references add 18 American signatures across
`funded`, `separate_clocks`, and `high_vol` at strikes 79.375/100/118.75. The initial
reference run classifies 15 as observed stable and 3 as unresolved, all retained.
On those same 15 stable contracts, baseline/domain8/domain16 MAE is
0.001264894/0.000351003/0.000354939, with maxima
0.010787675/0.001192779/0.001196624. Domain16 again does not improve the domain8
aggregate. Independent high-volatility refinement is pending; observed stability
and its retained uncertainty are evidence, not proof of the reference error.

Next numerical work should refine spatial resolution at a fixed wider domain and
separately refine time at that same grid, retaining refusals and comparable
contract intersections. Do not loosen caps or choose a production policy from
these synthetic aggregates or vendor labels.

## Immutable artifacts and provenance

All files below are under `C:/atx-cache/quote-benchmark/`.

- `markov-audit-20261016-controls-v2.json`: SHA256
  `d2d1269df0c0cfb72b0b1080cd0a0ba288d243d7c47e27745d2d74f1d7a6f03f`.
  Contains 55 cases, per-run command/stdout hashes, reused baseline path/hash,
  binary SHA256 `e0e198b0c5fa5e111083527384a78f7029452eeace005a55c38cc12daf3f439a`,
  and audit source SHA256
  `3e8c4b1b44a9378bb6234bb5c1f3ce350dec991e1450737b92f0618c36ff91c7`.
- `common-lane-controls-compare-20261016-v2.json`: exact comparator report,
  12960 rows across five grids, all 24 reference signatures matched. Uses the
  original ATM reference plus explicit negative-rate replacement provenance.
- `common-broad-controls-compare-20261016-v1.json`: parent-produced broad-reference
  comparison. Its source controls-v1 prices and every other non-timing numerical
  field were checked exactly equal to controls-v2; v2 corrects effective clock-step
  metadata and adds the configured step field.
- `american-reference-20261016-broad-v1.json`: SHA256
  `279fb6f50009ed9597755d4a44d0fd694b601c8d4f84baffe03b3bafa3f44171`.

Concurrent-host timings are diagnostic only. No throughput or latency claim is
made. The forthcoming storage API changes `layer.states` to `layer.states()`;
integration must adapt the two new successful-model provenance reads explicitly.
