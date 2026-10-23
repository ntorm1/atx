# HFT support matrix

One row per pricing domain. A row says which latency budgets the domain meets, which it misses
and what is still unresolved. A domain that stays slow or unresolved is listed as such and is
never left out. Budgets and clocks come from the vol-sota master plan (section 3.3 targets,
section 3.1 clocks). Each row cites the measurement it rests on.

## Budgets (master plan 3.3, p50, one core)

| Id | Workload | Budget |
|---|---|---|
| P1 | standalone cold price | <= 10 us |
| P2 | prepared price / identifiable IV | <= 5 us / <= 25 us |
| P3 | prepared price + primary Greek bundle | <= 20 us |

Status values:
- **WITHIN**: the p50 meets the budget.
- **MISS**: the p50 is above it (the factor is stated).
- **UNRESOLVED**: the accuracy contract cannot be certified on that domain.
- **NOT SUPPORTED**: the model does not implement the domain's convention.

A latency status always comes with the accuracy and coverage of the same rows (plan 3.3: every
latency row includes its accuracy and coverage contract).

## Measurement contract

The harness is B0's (`tools/bench_clocks.hpp`, `docs/benchmark-clocks.md`). The rules:
- One thread, pinned to one logical CPU of a performance core.
- Every report carries the affinity receipt (P-core proof, SMT sibling busy share, clock MHz)
  and the build receipt.
- Per-call steady_clock stamps.
- p50 / p99 are taken over the contracts of a row. They describe one call on one contract,
  not batch throughput.

## Rows

### Discrete cash dividends: American, spot-shift liquidator, production lattice (D1, 2026-10-23)

**Scope.** The path is opt-in: quote-bench key `dividend_lattice: spliced-cubic`, scheme
`DiscreteDivScheme::SplicedCubic`. The API is `american_discrete_div_production_*` and
`DiscreteDivBatch`. The default stays the historical `SplicedCrr` at 301 steps.

**Model and accuracy.** The model is in `docs/american-cash-dividend-reference.md` section 11.
- **Standard route** (CubicCrr at max(301, 25 x in-life events) steps, capped at 2401).
  Within 3 bp of IV of the independent reference on every RESOLVED row of D0's matrix; the
  worst is B2c at -2.865 bp. The historical 301 lattice reaches 34 bp on the same rows.
- **Refined route** (BBS-Richardson 1201 / 601; CubicCrr 2401 for a near-expiry ex-date).
  Within 1 bp on every RESOLVED row; the worst is E20c at -0.646 bp.

**Measurement.** Run `D1-20261023-production\latency\b\` (`atx-vol-ddiv-latency`):
- binary sha256 `ac3f234c...`, logical CPU 1 (a P core);
- clock 2100 MHz (the receipt's limit on this host);
- the dev200b divsched cohort: 398 chains, 22,009 contracts, Standard route.
p50 / p99, us:

| events in life | contracts | P1 cold price | P2 price | P2 IV | P3 bundle | group setup |
|---|---|---|---|---|---|---|
| 0 (ex-date on expiry) | 150 | 17.9 / 19.6 | 17.7 / 18.7 | 74.6 / 965.6 | 141.8 / 162.0 | 3.4 / 8.5 |
| 1 | 11,343 | 21.3 / 34.6 | 21.0 / 31.2 | 93.9 / 226.6 | 169.0 / 285.3 | 2.5 / 11.6 |
| 2 | 4,868 | 25.5 / 37.3 | 25.2 / 36.5 | 119.2 / 311.0 | 202.4 / 497.6 | 2.7 / 24.9 |
| 3-4 | 2,452 | 30.0 / 41.9 | 29.6 / 41.4 | 147.6 / 451.5 | 237.3 / 589.1 | 3.0 / 71.1 |
| 5-8 | 2,082 | 38.3 / 58.2 | 37.8 / 52.6 | 191.1 / 669.1 | 304.1 / 576.0 | 3.1 / 11.4 |
| 9-12 | 1,052 | 53.2 / 75.3 | 52.2 / 90.1 | 278.9 / 815.5 | 417.3 / 812.9 | 4.1 / 10.5 |
| > 12 | 62 | 98.2 / 125.9 | 94.8 / 130.6 | 513.5 / 1918.4 | 739.6 / 924.5 | 5.7 / 13.0 |
| all | 22,009 | 24.1 / 62.2 | 23.8 / 61.2 | 114.2 / 473.2 | 191.3 / 570.1 | 2.8 / 13.5 |

By event count and tenor, as the tool printed it (`dev200b-divsched-standard.md`; p50 / p99 us,
coverage as quoted / priced / price_only / refused / budget):

| events | tenor | groups | contracts | setup us p50 / p99 | cold price | price | IV | bundle | quoted / priced / price_only / refused / budget | prices |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | <=0.25y | 3 | 150 | 3.4 / 8.5 | 17.9 / 19.6 | 17.7 / 18.7 | 74.6 / 965.6 | 141.8 / 162.0 | 97 / 79 / 15 / 0 / 3 | 150 |
| 1 | <=0.25y | 84 | 4933 | 2.3 / 161.5 | 21.3 / 32.2 | 21.1 / 29.3 | 94.6 / 209.4 | 168.9 / 319.8 | 3578 / 3044 / 534 / 0 / 0 | 4933 |
| 1 | 0.25-1y | 108 | 6186 | 2.5 / 9.4 | 21.2 / 36.3 | 21.0 / 32.3 | 93.5 / 223.8 | 168.1 / 277.2 | 4739 / 4179 / 560 / 0 / 0 | 6186 |
| 1 | 1-2y | 2 | 224 | 2.2 / 2.5 | 22.3 / 25.1 | 22.1 / 27.2 | 94.3 / 470.1 | 176.7 / 199.0 | 212 / 191 / 21 / 0 / 0 | 224 |
| 2 | <=0.25y | 8 | 252 | 2.6 / 23.6 | 24.7 / 29.8 | 24.6 / 32.4 | 113.1 / 310.4 | 196.7 / 284.8 | 146 / 104 / 42 / 0 / 0 | 252 |
| 2 | 0.25-1y | 87 | 4032 | 2.7 / 55.0 | 25.6 / 39.7 | 25.4 / 37.7 | 118.5 / 256.8 | 204.0 / 541.8 | 3316 / 2998 / 315 / 0 / 3 | 4032 |
| 2 | 1-2y | 4 | 406 | 2.4 / 7.5 | 25.7 / 33.3 | 25.4 / 31.0 | 129.9 / 478.9 | 202.5 / 260.5 | 374 / 355 / 19 / 0 / 0 | 406 |
| 2 | >2y | 2 | 178 | 1.9 / 2.0 | 24.8 / 30.6 | 24.4 / 33.9 | 120.2 / 390.0 | 198.4 / 270.7 | 170 / 162 / 8 / 0 / 0 | 178 |
| 3-4 | <=0.25y | 2 | 46 | 2.7 / 3.3 | 27.1 / 32.6 | 27.1 / 59.7 | 129.2 / 485.9 | 214.9 / 449.0 | 27 / 22 / 5 / 0 / 0 | 46 |
| 3-4 | 0.25-1y | 34 | 1940 | 3.0 / 78.1 | 30.1 / 42.0 | 29.7 / 41.5 | 147.8 / 444.6 | 237.8 / 596.2 | 1753 / 1664 / 89 / 0 / 0 | 1940 |
| 3-4 | 1-2y | 4 | 294 | 2.6 / 3.9 | 30.5 / 37.0 | 30.1 / 36.2 | 156.7 / 511.3 | 241.6 / 394.6 | 282 / 276 / 6 / 0 / 0 | 294 |
| 3-4 | >2y | 1 | 172 | 7.3 / 7.3 | 29.1 / 32.6 | 28.5 / 32.0 | 142.1 / 277.4 | 228.2 / 295.8 | 172 / 172 / 0 / 0 / 0 | 172 |
| 5-8 | 0.25-1y | 11 | 334 | 6.8 / 8.3 | 39.6 / 53.7 | 39.3 / 55.5 | 192.4 / 661.2 | 317.1 / 519.1 | 207 / 165 / 42 / 0 / 0 | 334 |
| 5-8 | 1-2y | 23 | 1566 | 2.8 / 11.5 | 37.5 / 59.6 | 37.1 / 52.3 | 192.5 / 605.7 | 298.9 / 582.9 | 1499 / 1457 / 42 / 0 / 0 | 1566 |
| 5-8 | >2y | 2 | 182 | 6.0 / 6.8 | 39.1 / 57.1 | 38.7 / 49.8 | 187.9 / 1040.8 | 310.4 / 437.4 | 182 / 180 / 2 / 0 / 0 | 182 |
| 9-12 | 0.25-1y | 2 | 48 | 5.4 / 7.8 | 60.5 / 71.4 | 59.8 / 3554.6 | 261.5 / 1039.4 | 478.4 / 594.6 | 25 / 18 / 7 / 0 / 0 | 48 |
| 9-12 | >2y | 18 | 1004 | 4.1 / 10.3 | 53.2 / 77.0 | 52.2 / 86.2 | 279.3 / 787.5 | 417.0 / 821.2 | 989 / 983 / 6 / 0 / 0 | 1004 |
| >12 | 1-2y | 3 | 62 | 5.7 / 13.0 | 98.2 / 125.9 | 94.8 / 130.6 | 513.5 / 1918.4 | 739.6 / 924.5 | 33 / 27 / 6 / 0 / 0 | 62 |

**Status against the budgets.** The factor is p50 over the budget, by event count.

| Route | P1 cold price | P2 price | P2 IV | P3 bundle | Verdict |
|---|---|---|---|---|---|
| Standard, 0-2 events | MISS 1.8-2.6x | MISS 3.5-5.0x | MISS 3.0-4.8x | MISS 7.1-10.1x | MISS |
| Standard, 3-12 events | MISS 3.0-5.3x | MISS 5.9-10.4x | MISS 5.9-11.2x | MISS 11.9-20.9x | MISS |
| Standard, > 12 events | MISS 9.8x | MISS 19.0x | MISS 20.5x | MISS 37.0x | MISS |
| Refined (synthetic ladder, 0-12 events) | MISS 47-66x | MISS 96-132x | MISS 54-83x | MISS 191-266x | MISS |

The Refined row is from the synthetic ladder run (`latency\b\synthetic-refined.json`). Its p50
is 474-663 us cold price, 1357-2065 us IV and 3826-5325 us bundle.

**Coverage.** dev200b divsched cohort, Standard route, the quote is the bid/ask mid:
- Prices: 22,009 of 22,009.
- IV: 17,801 quoted (4,208 contracts have no two-sided market).
  - priced (identified): 16,076, 90.3 %;
  - price_only: 1,719, 9.7 % (the quote is at or below the exercise value or the lowest-vol
    lattice price; no volatility is identified);
  - refused by input or range: 0;
  - iteration budget exhausted: 6.

**Unresolved and unsupported sub-domains.**
- **UNRESOLVED: deep in the money before a late ex-date** (D0 rows F1c, F1p, F2c, F2p) **and
  cash near 60 % of a high-vol spot** (ILp). The reference cannot certify a root at the 1e-7
  IV contract on these rows. The Refined route still misses ILp by a decidable +4.49 bp, and
  the bundle's vega on F1c is 1.3 % above the reference's.
- **PRICE-ONLY: cash-dominated contracts.** These are contracts whose post-dividend spot sits
  beyond the 6-sd band, as on D0 row E64 (cash 64 % of spot, 5.9 days out).
  - The production routes price E64 to 1e-12 USD of the reference.
  - The IV is not identified.
  - `american_discrete_div_converged_price` refuses such a contract (OutOfRange).
  - Cash beyond the band at the 1024 pre-step cap is flagged `GridBottomExtrapolated`.
- **NOT SUPPORTED: the spot-survivor convention.** When cash exceeds spot, the kernel is the
  liquidator. D0 rows ISp / ISc measure the gap: +0.654 / -0.025 USD.
- **NOT SUPPORTED by this row: the escrowed convention.** It is a different model (D0 group H,
  0.15-0.19 USD from the spot-shift model).
- **Greeks.** Theta and charm are UNSUPPORTED (NaN plus a bit, never 0) when an in-life
  ex-date sits on steps 0..2 of the route. The time roll across an ex-date is an event
  operation, not a derivative.

**What would move this row.** The budgets need a 3.5-10x cheaper prepared price and a 3-11x
cheaper IV up to 12 events (19x and 21x above 12). The 301-step rollback alone is 17.7 us at
0 events (2.1 GHz). Lattice tuning does not close that gap. The bounded challenger the D1 brief
reserves (GIT hybrid-dividend, or Dynamic Chebyshev on a fixed schedule) is the candidate. It
has not been run.
