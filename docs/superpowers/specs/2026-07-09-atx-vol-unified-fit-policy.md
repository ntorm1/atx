# atx-vol unified fit policy and high-throughput quoting

**Date:** 2026-07-09  
**Status:** implemented and measured in `codex/atx-vol-fit-ms`

## Outcome

`PricerConfig{}` is now the production auto path. It extracts board features,
combines them with optional session/event/borrow hints, selects one of the seven
underlier profiles, and returns a public `FitDecision` containing the effective
preset and curve. High-confidence boards route without fitting selector
candidates; only ambiguous liquid boards pay for held-out cross-validation.

The dense-board default is the adaptive 48-node `LinearVariance` curve. Across
the ten real SPY OPRA snapshots (50 cold fits), the default-auto path measured:

| gate | result |
|---|---:|
| p50 cold fit | 73.63 ms |
| p95 cold fit | **88.49 ms** |
| maximum | 98.25 ms |
| worst clean price-in-NBBO | **98.61%** |

The explicitly pinned HFT path measured p50 61.53 ms and p95 77.25 ms on the
same run matrix. The auto-policy feature pass therefore preserves the 100 ms p95
service-level gate while making that route the zero-config default for SPY.

## One decision pipeline

```text
OptionChain + FitContext
        |
        v
classifier_inputs_from_underlier  -- O(number of live quote legs)
        |
        v
profile/ticker/event/session policy
        |
        +-- high confidence --> direct FitPreset + CurveConfig
        |
        `-- ambiguous -------> shared-de-Am held-out CurveSelector
                                      |
                                      v
                         VolaSession::build -> FittedSurface
```

Caller facts that cannot be recovered safely from one OPRA snapshot live in
`FitContext`: `profile_override`, `MarketSessionPhase`, `EventPhase`, event
distance, forward dispersion, effective yield/borrow, HTB, and vol-product
hints. `FitSelectionMode::CrossValidated` remains available for research and
explicit validation. A pinned `CurveConfig` bypasses auto-selection.

## Routing policy

| board/profile | observed condition | preset | curve |
|---|---|---|---|
| dense index/ETF | ticker/profile prior | HFT | adaptive 48-node linear total variance |
| dense mega-cap event | event window and >=1,500 live quote legs | HFT | linear total variance (preserves event W-shape directly) |
| medium event board | explicit event window | Fast | C8 (SVI-JW + ATM/wing bumps) |
| liquid single name | confident liquid profile | Fast | eSSVI |
| ordinary single name | confident ordinary profile | Robust | eSSVI + calendar repair |
| sparse small cap | fewer than 600 live quote legs | Fast | SVI; direct, no held-out search |
| volatility product | explicit hint | Fast | SVI with dedicated broad-wing filter profile |
| HTB/dividend name | HTB hint | Accurate | SVI (native discrete-dividend exercise remains future work) |

C8 is now a live `VolCurveKind`, fitted through the common `FitObs` seam and
stored byte-for-byte in the v3 archive. It is not forced onto dense event boards:
a compact parametric family is useful when observations are scarce, while the
market-node representation is both faster and materially more accurate when the
event board is dense. This is the same bias/variance principle used by the
selector, applied before expensive model fitting.

The policy has a production fallback ladder. A profile is a latency prior, not
permission to drop an underlier, so *every* family declares an ordered descent
and none retries itself:

| primary | rungs, in order |
|---|---|
| C8 | eSSVI, linear variance |
| eSSVI | SVI, linear variance |
| SVI / convex dense | linear variance |
| linear variance | eSSVI |

The ladder covers cross-validated boards too — a curve chosen by the held-out
selector is still a routing decision, not a caller instruction. Only a curve the
caller pinned (`PricerConfig::curve`, or the preset-pinned Hft dense route) is
never substituted. When the whole ladder is exhausted the fit reports the
*primary's* error, not the last rung's. `FitDecision` records the primary curve
and whether fallback was used; `fallback_curve_rungs()` exposes the table.

C8 is gated on board depth independently of the ladder: eight free parameters
need enough quotes to identify them, so a board below `sparse_validation_floor`
routes to the five-parameter eSSVI backbone (C8's own seed family) whatever the
classifier's confidence. A ticker prior says *which* underlier this is, not that
today's snapshot is deep enough to fit it.

## Real OPRA corpus

All payloads were pulled with atx-core's parent-symbology `cbbo-1m` loader behind
an exact `$0.00` metadata cost cap, then converted offline to Parquet. They live
outside git under `C:/atx/data`.

The original matrix contains ten SPY snapshots across three dates and
selloff/rally/calm regimes. The breadth matrix adds fourteen boards:

- QQQ and IWM, two minutes after open and two minutes before close;
- XOM at open and close;
- SOUN as a sparse small-cap board at open and close;
- VXX as a volatility product at open and close;
- AAPL immediately before and after its 2026-04-30 earnings announcement;
- AMZN immediately before and after its 2026-04-29 earnings announcement.

Measured real-data results from the unified policy:

| segment | curve | cold fit | clean price-in-NBBO |
|---|---|---:|---:|
| QQQ open/close | LinearVariance | 43-50 ms | 99.53-99.55% |
| IWM open/close | LinearVariance | 36-37 ms | 99.94-100.00% |
| XOM open/close | eSSVI | 292-409 ms | 98.13-99.11% |
| SOUN open/close | SVI | 108-115 ms | 92.59-100.00% |
| VXX open/close | SVI | 140-156 ms | 87.25-91.79% |
| AAPL pre/post earnings | LinearVariance | 14 ms | 99.71-99.87% |
| AMZN pre/post earnings | LinearVariance | 18-26 ms | 99.74-99.85% |

The sparse/vol-product percentages are computed with their selected profile's
quote filter. Reporting a zero denominator from the ETF-tight default filter
would be misleading on intentionally wide boards.

## Data layout and million-row quoting

`OptionChain::snapshot()` flattens the Universe board into aligned SoA columns in
one pass. `value_chain()` moves its ID column into the output and evaluates the
remaining columns directly; it no longer allocates an `OptionRef` array and
performs an `OptionChain::at()` decode/underlier lookup for every contract.

`Portfolio::create` accepts an expected-unique-contract hint and bounds its
automatic hash reserve. This prevents a million repeated positions from
allocating a two-million-bucket dedup table. The hint is advisory and clamped to
the position count, so an over-estimate cannot reinstate that allocation. The
portfolio quote path also has a `PriceOptions::prices_only` mode: quote refresh
computes IV plus one American mark per unique contract; full Greeks run on their
independent risk cadence. Under that mode both the per-lane Greek columns and the
`PriceTotals` Greek sums are NaN — never 0.0, which a risk aggregator could not
distinguish from a genuinely vega-flat book.

On the measured 64-underlier / 1,000,000-position / 2,688-unique-contract book:

| operation | result |
|---|---:|
| portfolio build + dedup | 45.36 ms |
| price-only quote refresh | **83.20 ms / 12.02M rows/s** |
| full American Greeks | 344.42 ms / 2.90M rows/s |

The SoA scatter is parallel and writes disjoint slots. Totals are reduced in a
second fixed input-order pass, preserving bit-identical output across thread
counts.

## Why algorithms and layout came before AVX2

The cold-fit gain came from reducing American-IV work, fitting independent
expiries concurrently, scheduling dense expiries first, and using compact
contiguous nodes. The repository's prior clang-cl/xsimd experiment made the
Andersen-Lake path slower because the compiler could not vectorize the branchy
root/boundary solves profitably. SIMD remains appropriate for homogeneous B76
lanes, but it is not assumed to rescue irregular American solves. The current
design exposes contiguous SoA columns so a future AVX2/AVX-512 B76 kernel can be
added behind the same API after a measured crossover test.

## Research basis

- Gatheral and Jacquier, *Arbitrage-free SVI volatility surfaces*:
  <https://arxiv.org/abs/1204.0646>
- Gatheral and Jacquier, generalized arbitrage-free SVI:
  <https://arxiv.org/abs/1210.7111>
- Andreasen and Huge, *Volatility Interpolation*:
  <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1694972>
- Leung and Santoli, scheduled earnings jumps and American option surfaces:
  <https://arxiv.org/abs/1412.8414>
- Earnings-event implied-volatility concavity:
  <https://arxiv.org/abs/2307.15718>
- Cboe OPRA quote-interval data description:
  <https://datashop.cboe.com/option-quote-intervals>
- Cboe EOD summary methodology:
  <https://datashop.cboe.com/option-eod-summary>
- Cboe options analytics scale:
  <https://www.cboe.com/solutions/options-analytics>
- Apple 2026 Q2 earnings announcement:
  <https://www.apple.com/newsroom/2026/04/apple-reports-second-quarter-results/>
- Amazon 2026 Q1 earnings announcement:
  <https://ir.aboutamazon.com/news-release/news-release-details/2026/Amazon-com-Announces-First-Quarter-Results/>

## Remaining risk

- HTB/heavy-dividend calls still use escrowed-forward continuous-yield early
  exercise; native discrete-dividend exercise is not solved by curve selection.
- VXX is now available and stable, but its 87-92% gate remains below equity/ETF
  quality and deserves a dynamics-aware profile if the business requires tighter
  marks.
- Sparse-board percentages have small denominators by construction; the corpus
  gates both sample count and accuracy and should grow with more names/dates.
- C8 archive payloads are additive under the existing v3 record format. Readers
  built before C8 correctly reject an unknown kind (records are length-prefixed,
  so an unknown kind is rejected per-surface without desyncing the stream);
  existing eSSVI/SVI/convex/linear archives keep their layout and schema
  fingerprint. `schema_hash()` therefore cannot fold `sizeof(C8Params)` without
  invalidating every already-written archive, so the layout is frozen by a
  `static_assert` instead: changing `C8Params` fails the build and tells you to
  bump the v3 salt.
