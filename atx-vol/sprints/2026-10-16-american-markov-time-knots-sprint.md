# Variance-time-knot term structure for the common Markov fitter

Lane `feat/vol-markov-time-knots2-20261016`, frozen base
`5e978fed9fe5db28c83a1fcf1d47fff0d7f829e1`, pool-34, RunId `vol-20261016-tk2`.
It completes the paused WIP `7f466d4d` (see the
[WIP handoff](2026-10-16-american-markov-time-knots-wip-handoff.md)), which was
cherry-picked as `09764534` with a clean automatic merge. The parent's later
`validate_model` / `max_transition_work` / resolvent `minimum_action_work`
lines (`340ae585`) and the native seed helper (`8d4f9354`, `b2b979c1`) are
intact. Implementation preceded tests. No vendor labels, vendor IV, Greeks,
synthetic spot or market data were read; every check below uses synthetic
inputs. Defaults, hard caps and existing shapes are unchanged.

## Design as implemented

**Opt-in shape.** `AmericanMarkovFitTimeShape::VarianceTimeKnots` with borrowed
`AmericanMarkovFitConfig::variance_time_knots`: at least two finite,
nonnegative, strictly increasing absolute cumulative variance-year coordinates,
all chosen by the caller. The parameter count is knots times
`max(1, log_state_knots)`, bounded by the configured `max_parameters` (hard cap
128) whatever the expiry count. Knots with a legacy shape, fewer than two knots,
unordered/negative/nonfinite coordinates and parameter overflow are refused.

**Interpolation.** Each ORIGINAL interval gets one owned row
(`AmericanMarkovFitTimeWeight`): its represented variance midpoint, the bracketing
knot pair and the right weight. Midpoints before the first or after the last knot
use flat tails; a midpoint exactly on a knot is one-hot. Log sigma is
interpolated per state knot and that local-vol vector is frozen for the whole
original interval. No expiry, dividend or schedule boundary is inserted or moved.
Equal controls return the control bits exactly (no `exp(log(x))`). This lane
added a bracket: the interpolated value is clamped to `[min, max]` of its two
controls, removing only exp/log rounding so in-bounds controls cannot produce a
spurious out-of-bounds node. Interpolation weights are never clipped; a lost
interior coefficient (for example a subnormal weight underflowing to zero) is
refused. Paused-variance intervals keep their row with `variance_active=false`.

**Basis admission.** A streaming Givens QR of the chronologically ordered
adjacent-support rows keeps R bidiagonal (two rotations per row). Rows are every
positive-variance interval through the latest quoted expiry, including unquoted
intermediate intervals; paused intervals and intervals after the last quoted
expiry are excluded. Unordered or malformed rows are `InvalidArgument`; fewer
rows than knots, an unsupported column, an exactly dependent column, nonfinite
values, or an observed reciprocal infinity condition of `R/sqrt(M)` below the
fixed `1e-12` are `OutOfRange`. Diagnostics report quoted rows, per-column
support counts and L2 norms, the `sqrt(M)` normalization and the reciprocal
condition. This describes the represented interval log-sigma map only: it is not
an option-price Jacobian, an economic information measure or an identifiability
proof. Prior and roughness terms are never consulted, so no regularization
weight can rescue a refused basis.

**Resources.** Interval-times-state-knot scratch is preflighted against the
model's own `max_stored_volatility_values` in both the fitter and policy
resolution. All other row, work, state, transition and fit caps are untouched.

**Regularization.** Unchanged index penalties over the group-major layout, where
a time group is a time knot: prior distance, adjacent state-knot roughness and
adjacent time-knot roughness. Time roughness is not scaled by knot spacing.
Empty prior still means the fixed initial vector.

**Provenance.** `interval_groups()` is removed without a shim. Owned
replacements: `time_shape()`, `variance_time_knots()`,
`interval_time_weights()` (legacy shapes report one-hot rows; their paused
intervals keep group zero and are inactive) and
`time_basis_diagnostics()` (present only for the knot shape). Copies and moves
retain all of it independently of the caller's buffers.

**Policy and seeds.** The catalog deep-copies time knots, validates them and the
initial/prior layout, counts knot groups in `resolve`, and refuses schedules whose
interval scratch exceeds the profile's volatility budget. The quote-seed
estimator returns `Unavailable` for the knot layout before any other validation
or inversion; legacy seeds are never projected onto knot controls.

**Native benchmark.** A profile's `fit` may set
`"time_shape": "variance-time-knots"` and `"variance_time_knots": [...]`; the
catalog validates them before any output is written. The expanded definition
adds `variance_time_knots` only for the knot shape, so every legacy expanded
definition and its configuration hash keep their previous bytes. Fit reports
replace `interval_groups` with `time_shape`, `variance_time_knots`,
`interval_time_weights` and `time_basis` (null for legacy shapes; the knot shape
also reports the fixed threshold and a `time_interpolation` disclosure). The
seed layout names the shape and records knots. With `quote-seed`
initialization the knot shape reports `fallback_error` / `Unavailable`, keeps the
configured initial and prior, and produces byte-identical predictions.

**Error ordering.** The fitter now checks the interval-schedule predicates (the
same predicates the compiler applies) while building the time map, before
parameter-vector checks. Schedules that were already invalid are refused with
the same `InvalidArgument` code but possibly a different first message. Every
successful legacy output is unchanged (below).

## Midpoint-freezing approximation and its measured bias

Within one knot cell sigma^2 is exponential in variance time, so the
represented/continuous interval variance ratio is exactly `x / sinh(x)` with
`x` equal to half the interval width times the slope of `log(sigma^2)`. The
small-slope bias is about `-x^2/6`: for a one-week interval
(`1/52` variance-years) and a log-variance slope of 1 per variance-year it is
about `-1.5e-5`. An interval that straddles a knot, or whose midpoint lies in a
flat tail while part of it lies on a slope, has a first-order error of either
sign. Compiler substep refinement cannot remove this, because the frozen vector
is chosen per original interval.

The permanent test `MidpointFrozenKnotSigmaBiasIsMeasuredAgainstContinuousCurve`
uses a deliberately steep, coarse panel: knots `0.125, 0.625`, controls
`0.15 -> 0.45` (log-variance slope 4.394), zero-carry dyadic expiries and the
variance resolvent at `1/512` steps. A 32-times refined schedule on the identical
step grid, each piece frozen at its own midpoint, is the continuous proxy; its
own residual cumulative variance error is below `1e-4`. Spot 100, ATM calls:

| Expiry | Interval (variance years) | Interval variance bias | Cumulative variance bias | ATM call price bias |
| --- | --- | ---: | ---: | ---: |
| 0 | 0 - 0.0625, left tail | 0 | 0 | 0 |
| 1 | 0.0625 - 0.125, left tail | 0 | 0 | 0 |
| 2 | 0.125 - 0.375, inside cell | -4.857% | -3.811% | -0.0914 |
| 3 | 0.375 - 0.5, inside cell | -1.246% | -2.624% | -0.0835 |
| 4 | 0.5 - 0.875, straddles knot, midpoint in tail | +8.326% | +5.507% | +0.3303 |

The in-cell values match `x/sinh(x) - 1` to `1e-13`. The bias is a property of
the represented model, not a fitting error: a calibration absorbs it into the
controls, so fitted knot values are not samples of an exact continuous
`sigma(t)`. Tests record every value above as gtest properties.

## Validation

All gates use the leased tree's own wrapper, preset `vol-rel-avx2` (Release,
AVX2, ThinLTO, precise floating point), two build workers, unless noted.
Counts are executed/passed with zero skips.

- **Single-TU checks (Release):** `check` compiled
  `src/fitting/american_markov_{fit,policy,seed}.cpp`,
  `tools/quote_bench_common{,_policy,_seed}.cpp` and
  `tests/american_markov_fit_test.cpp` (focused and monolithic objects).
- **Legacy bit parity:** the WIP's eight-case probe
  (`markov-time-knots-20261016-parity.cpp`, SHA256 `67b71f8a...1a03ac`) was
  registered temporarily, built and run twice, after the cherry-pick and after
  the final edits. Both runs equal the captured baseline
  (`markov-time-knots-20261016-baseline.json`, SHA256 `b3e08ab0...f178a9`):
  8/8 cases, all 232 binary64 sigma/price/objective values and all five
  optimizer counters per case, for Shared and PerVarianceInterval under explicit
  and resolvent pricing with flat and three-node smiles. The registration was
  removed; `atx-vol/CMakeLists.txt` is byte-identical to the base.
- **Target-scoped Release builds:** `atx-vol-tests` (370 steps, exit 0),
  `atx-vol-american-markov-{fit,policy,seed,market}-tests`,
  `atx-vol-american-markov-tests`, `atx-vol-quote-bench`,
  `atx-vol-american-markov-audit` and `atx-vol-umbrella-tests`.
- **Anchored CTest** (`-Ctest -R '^<name>$'`), executed/passed:

  | Test | Executed/passed | New |
  | --- | ---: | ---: |
  | `atx-vol-american-markov-fit` | 28/28 | 12 |
  | `atx-vol-american-markov-policy` | 16/16 | 3 |
  | `atx-vol-american-markov-seed` | 15/15 | 1 |
  | `atx-vol-american-markov-market` | 20/20 | 0 |
  | `atx-vol-american-markov` (pricing) | 60/60 | 0 |
  | `atx-vol-quote-benchmark-native` | 122/122 | 15 |
  | `atx-vol-american-markov-audit-native` | 18/18 | 0 |
  | `atx-vol-quote-benchmark-campaign` | 22/22 | 0 |

- **Monolithic executable,** filter
  `AmericanMarkovFit.*:AmericanMarkovTimeBasis.*:AmericanMarkovPolicy.*:AmericanMarkovSeeds.*`:
  59/59.
- **Public-header inventory,** `atx-vol-umbrella-tests` filtered to
  `TierCountsMatchTheReadmeTable`, `DemotedSurfaceContainersAreNotNamedInPublicHeaders`,
  `EveryPublicIncludeRootShipsAndIsDeclared` and `TierAIsClosedUnderInclusion`:
  4/4. No API header was added; the new helper is private under `src/`.
- **PCH-off (`vol-hygiene`, Debug):** `check` compiled all 15 owning objects of
  the changed headers: the three fitting sources, `american_markov_market.cpp`,
  the three native tool sources, and the fit/policy/seed/market test sources in
  both their focused and monolithic targets.
- **Ruff:** the changed native test file reports the same two import findings as
  the base commit (untouched lines); nothing new.

Receipts in `C:/atx-cache/quote-benchmark/`, prefix `time-knots2-20261016-`:

| Suffix | SHA256 |
| --- | --- |
| `fit-tests.xml` (with bias properties) | `d9b234eaf22ae786519552598d60f2c5707ce1a6a9251c30fe7fb3835f7379dd` |
| `policy-tests.xml` | `fc36965fff419aa98f5af49abb52c23995d4336eaaf1d679a66c114c5517f988` |
| `seed-tests.xml` | `1246f5382c8e834d13a5d4f6a3f4c2f85e604f9d9e77443be1ca0990a3242625` |
| `market-tests.xml` | `c119b4c9b3f1787112d94bcde93fc2efb06c503b042a41d7ed279da1fa5d42b4` |
| `pricing-tests.xml` | `2cc061f570829796c2a83d72108b6ec1d56e047bce74cb40b3e429666bbba254` |
| `native.xml` | `0d3bbc232388e41fbff83f830be8a734a427563caa5f0431fe32e9e736658a0a` |
| `audit-native.xml` | `3a279815bd7926aa7f23441a68460e9f8a624979a533cf3f61216ee923cf36eb` |
| `campaign.xml` | `c27757888dcc86af25e9eb220fbb1d7a35cee55da5bd587248df5b68409eec42` |
| `monolithic-markov.xml` | `ec4b74175b86780445a8a6a695795c59bff11b92d62a749514739067f1a9d223` |
| `umbrella-inventory.xml` | `e07004ad1f65305b0bfe5efdf817b8ee1d84baf87009327fb417c02b8ff9cbb6` |
| `parity-candidate.json` | `d54df7211da4094104005248101ba9ffdb7fd52a9ca784c033cea95acff4422a` |
| `parity-summary.json` | `9e2eecb7439469ddaff98d1053dfe15d419ea690f1d4863ec8fd169bf449d526` |
| `ctest.log` | `faff0e73cf901621bdb9ced7fdddc9db4f49634216165b1b00f076730107a197` |
| `hygiene-check.log` | `7317107f68d232153c01ee2c55a50e912125002f663de986df209f724f2b0286` |
| `build-atx-vol-tests.log` | `247bf1aa36551b7742c39f56594cd5afa7260313ead8e127df782a78f33d8e22` |
| `build-owning-targets.log` | `b72ac7d7da1244753178cce71e72adf9d2dbe08323cd1c722fcf11d09d7b88b1` |

Toolchain: clang-cl 18, Windows 11 10.0.26200. Test wall times are diagnostics
on a shared host, not speed claims.

## Not done or deliberately out of scope

- Fresh exact-SHA independent review has not happened; integration should wait
  for it.
- Full labels, the umbrella suite, full-repository hygiene and qualified
  performance were not run or claimed.
- Time roughness remains an index penalty, not a knot-spacing-scaled derivative.
- Quote seeds do not support the knot shape; there is no projection.
- Basis admission does not establish price-level identifiability: a knot can pass
  while its quotes carry little vega.
- No market experiment, default promotion or campaign change was made. The
  predeclared resolvent experiment keeps its frozen seed-free binary.
- `tests/CMakeLists.txt` permanently grants `src/` to the focused fit-test target,
  matching existing per-target grants, because the basis tests drive the private
  helper directly. The temporary parity-probe registration was removed and
  `atx-vol/CMakeLists.txt` is byte-identical to the base.
