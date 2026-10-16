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

## Active independent lanes

- Concentrated state-grid helper: bounded sinh grid with exact zero, initial
  spot and upper states; no default compiler/cap changes.
- Reference audit: fresh review of `8f73464d` approved its offline diagnostic
  role, 19 baseline tests passed; durable checkpointing, startup source hash and
  QuantLib version qualification under development.
- Common-market adapter: robust aggregation of allowed raw market inputs into
  one common stock process and expiry prefixes, with dispersion/refusal reports.

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
