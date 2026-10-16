# Common-model continuation, October 16

Resumed at the user's explicit request from `feat/vol-continue-20261016`,
frozen base `9717c4769fb93ff8e40e9159bcc6b741aa46bf7e`. Integration branch:
`feat/vol-resume3-20261016`, pool-33, RunId/HeartbeatId `vol-20261016-root3`.
Main is untouched. The long-running goal remains active.

User instructions retain label isolation, mandatory no-arbitrage, reusable
high-performance code, symbol-sensitive configuration, subagent development,
and implementation before tests. No graph or broker tools are exposed in this
session; native agents use separate pool leases and bounded source discovery.

## Integrated fitter

The previously independently approved fitter pair was cherry-picked as
`e2ffa6f1` and `0d12fb86`. Both source and monolithic/focused test registration
are now permanent. Release AVX2/IPO checks passed: 14 fitter cases and 36
Markov/PWL cases. Owning fitter source and both test objects compile with PCH
disabled. These results establish integration, not continuous-American accuracy
or optimization quality.

The README public-header census was stale: after fitter integration it is 58
Tier-A plus 28 Tier-B plus the umbrella, 87 api headers. New APIs remain Tier-B.
A focused umbrella target runs the existing public-surface contract suite.
It executed 10 cases: 9 passed, including census/install/tier closure; the
fixture-path scan failed on five preexisting absolute literals in
`chain_export_test.cpp`, `oracle_bench_test.cpp`, `chain_export.hpp` and
`oracle_bench_main.cpp`. The full umbrella gate is therefore not green.

## Integrated grid, adapter and reference

Grid `807c4036` received a fresh independent APPROVE and is integrated as
`f76cefc2`. Adapter `10de6dd7` received parent source/header/test review and is
integrated as `22e69239`. Permanent focused and monolithic registrations added;
README census is now 30 Tier-B headers. Parent Release Markov/grid 47 cases and
adapter 20 cases passed. Parent source/test object checks passed with PCH off.

The synthetic audit now has five explicit sinh profiles (65/129 at scale .20,
65/129 at .05 and129 at .005), plus the previous eight. `--pricing-only` reports
PWL as not_requested and measures scalar/batch prices. Requested strike/side/
exercise universes are recorded before compilation, including refused models.
Compiler limits and production grid defaults remain unchanged. Final audit TU
passed Release and PCH-off compilation.

Original reference `8f73464d` and enhancement `2c0cd3ff` integrated as
`701267a7`/`f376b606` after independent review; parent reran30 Python cases,
all passed. QuantLib remains an isolated offline dependency.

New refined negative-rate ATM evidence:
`C:/atx-cache/quote-benchmark/american-reference-20261016-negative-rate-refined-v2.json`,
SHA256 `411f45df26c549b626a182e5717f9b0fb98fecdbcb08d3405c8355dbeb997edf`.
Input is the preserved ATM-only document; filter negative_rate, base1600 time /
1600 space,3 levels, absolute tolerance .0001. Both rows now observed stable
under unchanged tolerances. Call7.314923899471783, largest observed change
.00006336893283176437 versus threshold .00010731492389947179; put
10.107548269412252, change .0000020073020063904323. These are observed refinement
differences, not proven error bounds. A default-path SQLite checkpoint persists
both completed references.

## Active independent lanes

- Reusable common-model liquidity profiles, shared raw classification,
  owned numeric configs and exact symbol overrides.
- Native common-model benchmark route, modular shared IO, closed policy parser
  and Python runner provenance.
- Exact-signature audit comparison, full requested coverage and paired baseline
  metrics, using saved independent references without recomputing them.

Parent next wires native common-model benchmarking and controlled numerical
comparisons. New measurements must retain whole-cohort coverage/refusals and
paired comparisons; no unseen validation labels are used for development.

## Primary-source checks

Rechecked October 16:

- [SpiderRock surface documentation](https://docs.spiderrockconnect.com/docs/Documentation/PlatformFeatures/Analytics/LiveVolSurfaces/)
  describes bid/ask-channel fitting, different treatment of sparse markets,
  and a separate fast adjustment loop. Its funding-PV forward convention
  supports explicitly converting aggregate cash PV; missing ex-dates still
  prevent reconstructing exercise around cash events.
- [SpiderRock pricing documentation](https://docs.spiderrockconnect.com/docs/Documentation/PlatformFeatures/Analytics/OptionPricing/)
  describes its American cash-dividend modeling. Vendor statements are not
  measured performance evidence for this library.
- [QuantLib v1.44 concentrating mesher source](https://raw.githubusercontent.com/lballabio/QuantLib/v1.44/ql/methods/finitedifferences/meshers/concentrating1dmesher.cpp)
  is a primary implementation reference for concentration; our finite-process
  positivity, CFL cost and continuous approximation need separate assessment.

Source parquet remains `C:/Users/natha/Downloads/OptionIntradayHist_30min_2026-10-14.parquet`.
Reuse the hash-bound `C:/atx-cache/quote-benchmark/20261014-dev200` cohort; its
72,348 selected rows include 63,942 input-eligible rows. Labels are separate.
