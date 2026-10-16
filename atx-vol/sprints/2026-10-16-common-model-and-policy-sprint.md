# Common American model and symbol policy continuation

The user resumed the long-running goal after the 2026-10-16 handoff. Goal is
active; the earlier pause record is historical. Work remains on leased branch
`feat/vol-continue-20261016`, originally pool-35, RunId/HeartbeatId
`vol-20261016-root2`, frozen base `4fa9afddcf84afb4cfc78d4d0fac26a0ff72b744`.
Main has not been merged or changed. No validation labels were inspected.

## Integration and validation

Fresh independent source review approved native policy pair cacb48eb/6b7e5a0b
and local-vol commit 61b93c56 with finite-Bermudan scope limitations. Integrated:

- 35829a69: synthetic audit source and audit handoff.
- afbdd2fe: permanent audit executable registration.
- 46564499 + 28bcba49: native bounded policy configuration and routing.
- dd539657: owned interval local-vol profiles in the common Markov model.
- ef561e62: explicit CLI funding/escrowed dividend PV modes.
- 0d8e225d: optional policy metadata, grouped coverage and paired transitions.

Root Release tests after integration: 32 Markov/PWL, 49 quote/price/policy,
42 native CLI tests passed. PCH-off native runner and audit source compiled.
Python adapter 54 tests passed; Ruff and diff checks clean. These are targeted
gates, not full release qualification or continuous-American accuracy proof.

The dividend CLI is now `funding-pv|escrowed-pv|none`, default funding-pv. Both
aggregate modes explicitly approximate unknown dated cash with continuous
yield. Runtime provenance records the convention; the Python wrapper binds
it before native passthrough arguments. Old ambiguous aggregate-pv spelling
is rejected. Native market preparation no longer incorrectly rejects PV>=spot
for a funding convention with sufficiently negative carry. Independent flat
call tests cover that case and refusal under incompatible conventions.

The funding convention is motivated by the documented forward identity in
[SpiderRock live surfaces](https://docs.spiderrockconnect.com/docs/Documentation/PlatformFeatures/Analytics/LiveVolSurfaces/),
not selected by benchmark label errors. Missing ex-dates remain an approximation.

## Fresh development benchmark

All three runs use the same Release binary and current mandatory static
American strike/calendar gates, at 10:30 NY on the existing 200-underlier hash
development cohort: 72,348 selected, 63,942 raw-input-eligible rows. Exact commands,
binary/input/output hashes and full policy definitions are in `.csv.run.json`
receipts. External directory: `C:/atx-cache/quote-benchmark/20261014-dev200/`.

| Run prefix | Served | Coverage | Own-population price MAE | Own-population vol MAE |
| --- | ---: | ---: | ---: | ---: |
| 20261016-essvi-funding-v2 | 10222 | 15.9864% | 0.356753 | 389.939 bp |
| 20261016-auto-funding-v2 | 10694 | 16.7245% | 0.150988 | 475.242 bp |
| 20261016-auto-escrowed-v2 | 10772 | 16.8465% | 0.155959 | 472.209 bp |

Own-population MAEs are not comparable improvements because serving populations
change. `20261016-compare-essvi-auto-v2.json` reports exact intersections:

- Price 4045 rows: fixed 0.318183628 -> auto 0.201308293 dollars/share.
- Vol 4046 rows: fixed 190.723791 -> auto 118.380774 absolute vol bp.
- Fixed-only price-scored 6153, auto-only 6609: substantial selection changes.

`20261016-compare-dividend-conventions-v2.json` compares escrowed A to funding B:

- Price 10654 rows: 0.156582615 -> 0.150988326.
- Vol 10694 rows: 475.329634 -> 475.242406 bp.
- Funding served 78 fewer contracts and added none. This is a small paired
  convention effect, not evidence that missing ex-dates have been solved.

Automatic funding profile breakdown (same complete eligible denominator per
profile, unresolved metadata retained separately):

| Profile | Eligible | Served | Coverage | Main failures |
| --- | ---: | ---: | ---: | --- |
| dense | 17734 | 3336 | 18.8113% | 8192 strike-arb rows, 3060 calendar, 3146 insufficient |
| ordinary | 34700 | 7050 | 20.3170% | 632 strike-arb rows, 8806 calendar, 18212 insufficient |
| sparse | 10792 | 308 | 2.8540% | 618 calendar, 9866 insufficient |

716 eligible rows have no resolved profile and remain in the coverage denominator.
Across 1138 attempted chains, 132 fit/served; 811 have too few admitted distinct
strikes, 151 calendar-rejected. These results justify interval observations and
a common underlying model; they do not justify disabling admission to recover
coverage. Existing raw quotes, profiles and independent scoring remain separate
from srPrc/srVol labels. All timings were noisy diagnostic timings, not benchmark
speed claims (other agents compiled and some accuracy runs overlapped).

## Active implementation lanes and next work

- PWL crossing: actual failure is opposite +/-7.105e-15 gaps between adjacent
  doubles 175 and 175.00000000000003. Agent is implementing a conservative
  cumulative crossing-resolution error budget with the unchanged global
  convexity gate. Exact represented crossings stay intact. Public default caps
  remain, so exact-knot growth still needs attention.
- Common-model fitting: reusable raw bid/ask interval loss, positive absolute
  local-vol parameters, configurable flat/flexible/time layout and smoothing,
  grouped batch rollback with bounded optimization/resources. No parquet or
  label dependency; one shared carry process. Implement first, then validate.
- Independent stochastic American reference: separate numerical reference
  tooling with dependency/version/convergence receipts; no silent date rounding
  or claim that same-grid agreement is continuous-American accuracy.
- Root audit now adds ATM and nearby probes to the prior 33-strike panel;
  preserve old artifacts and mark changed sampling in new output schema 2.

Full joint-model admission, continuous exercise/dividend accuracy, useful
calibration coverage, hot update/query performance, analytics and eventual
release qualification remain unfinished. Do not declare the goal complete
from targeted tests or these limited development comparisons.

## Crossing fix and expanded numerical panel

Reviewed ac07729f's source and error argument, integrated as 8d7e7d1e. The
crossing bound propagates through positive sums and exercise maxima with outward
rounding and must stay within the unchanged fixed allowance. It is explicitly
not a bound on all IEEE arithmetic or model discretization error. Combined
Release Markov/PWL tests: 36 passed, including integrated local-vol tests.

The updated audit includes spot and spot*(1+/-1e-6), schema 2, and the maximum
output crossing-error bound. Release build, full run, and PCH-off audit source
check passed. Artifact `C:/atx-cache/quote-benchmark/markov-audit-20261016-atm-v2.json`,
SHA256 `bfd5dba7ee1909e0d2acba83c22bc8ca1e4d170b74dddb2fb4040e94d96725f6`.
Do not compare its sampled maximum errors to schema 1 without accounting for
the added strikes. A separate reference lane is assessing stochastic American
accuracy using this expanded panel.
