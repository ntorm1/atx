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

## Integrated policy and numerical comparisons

Policy `218b178f` received independent APPROVE and is integrated as `15e9a9fd`.
It owns all profile vectors, shares the raw liquidity classifier with the old
quote-slice policy, and retains explicit symbol overrides and resource refusals.
Parent permanently registered library/focused/monolithic targets and updated
the header census to 58 Tier-A plus 31 Tier-B plus the umbrella (90 total).
Parent Release policy and quote-slice CTest lanes both pass (12 and 49 cases);
policy source/test and classifier source compile with PCH disabled. Defaults
remain uncalibrated; a valid configuration does not guarantee every market fits
within its work budget.

Comparator `d66fbed6` received parent source/test review and is integrated as
`5896262c`. Parent's 44 standalone Python tests pass. These load this tree's
tool directly and run with `--noconftest`, since the unrelated compiled Python
extension resolves to another checkout on this host. The extension suite was
not run or claimed. Exact numeric schedule/contract joins retain requested
coverage, failures, unresolved references and explicit replacement lineage.

Audit v2 adds concentration equal to the first positive cumulative synthetic
standard deviation, with an explicit fixed fallback for deterministic paths.
This uses known fixture parameters, never vendor labels, and is an experiment
rather than a production selection rule. Both 65/129-state adaptive profiles
compile all 11 fixtures under unchanged limits. The 129-state short-low-vol ATM
put is .032176688742 versus .032239718069 independent FD, compared with
.001257913867 on default65. On the exact 24-contract American ATM reference set,
MAE is .000476122095 versus .163272839667 default65, with all 24 priced. Those
synthetic results are not SpiderRock fit improvements.

Audit v3 adds 129 states at upper8S, and 65/129 at upper16S. All three compile
all 11 fixtures. Across the full 198-case document, 182 compile/price; every
success contains exactly its requested expiry/strike/side/style universe.
Independent review found all 165 overlapping v2 cases numerically unchanged
and recomputed the no-cash European errors within 2e-12. The adaptive129
upper4S/8S/16S worst European errors are .088490959/.006131120/.010136436;
high-vol upper-boundary probability is .0392899/.00309173/.000122766. Corresponding
24-contract American ATM MAE is .000476122/.000596472/.000788782. Widening a
fixed-count grid trades boundary error against central resolution. Next audit
should match near-spot spacing and vary time refinement separately. Defaults
and compiler caps are unchanged. Final v3 audit TU passes PCH-off compilation.

Immutable external receipts under `C:/atx-cache/quote-benchmark/`:

- `markov-audit-20261016-sinh-v2.json`, SHA256
  `b491940f5da502af80998dd8e86537e1a31e8a75a7e7a1ef15c0381bad61362e`.
- `common-lane-sinh-v2-compare-20261016-v2.json`, SHA256
  `d0b2bf6848222fa302f37b31e03c55339a46f0598ec39632cb5f5e5d865097df`.
- `markov-audit-20261016-sinh-v3.json`, SHA256
  `28d6aeabf3cc985b952ad62d8d4f35dca546d196ffae3757ce17da583a481739`.
  Its `.json.run.json` records source base182ee25c, exact pending audit source
  `963189ec548eb90fd97ad3535da11232fb8f1a85cb2abd5b7f5007a68b90778c`, and binary
  `bd7267533daddca3f216cbe23394fb39a6ee406ab6e396e4669830e30797f8f4`.
  Concurrent-host wall time is diagnostic only.

The saved original ATM reference and explicit two-row negative-rate refinement
remain separate artifacts; replacements do not erase their earlier history.

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
