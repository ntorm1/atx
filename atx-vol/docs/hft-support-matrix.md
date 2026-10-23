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
- These are the routes' accuracy CLASS on D0's 41 RESOLVED rows (spot-shift liquidator).
  The plan's D1 objective, "within the reference's uncertainty", is NOT met: the per-event
  drift misses D0's u (doc section 11.3-11.4).

**Measurement.** Run `D1-20261023-fix1\latency\f1\` (`atx-vol-ddiv-latency`, fix round 1):
- binary sha256 `a5cb40ec...` (code at 59cdf815), logical CPU 1 (a P core), clock 2100 MHz
  (the receipt's limit on this host);
- host load: system 40 % (r6's run b: 21 %). The price and IV legs are unchanged code since
  run b and read about 10 % slower here (run b: cold 24.1, price 23.8, IV 114.2 us). The
  bundle gained 2 rollbacks (the vega pair: run b 191.3 us);
- the dev200b divsched cohort: 398 chains, 22,009 contracts, Standard route.
p50 / p99, us:

| events in life | contracts | P1 cold price | P2 price | P2 IV | P3 bundle | group setup |
|---|---|---|---|---|---|---|
| 0 (ex-date on expiry) | 150 | 19.2 / 59.2 | 19.0 / 24.3 | 78.8 / 1061.0 | 189.7 / 277.1 | 2.5 / 2.9 |
| 1 | 11,343 | 23.8 / 39.1 | 23.6 / 37.3 | 101.9 / 192.0 | 234.3 / 367.1 | 2.3 / 11.5 |
| 2 | 4,868 | 29.0 / 52.0 | 28.7 / 50.3 | 129.9 / 473.2 | 286.2 / 499.6 | 2.5 / 12.3 |
| 3-4 | 2,452 | 34.4 / 66.4 | 33.6 / 57.8 | 162.7 / 424.7 | 336.2 / 600.4 | 3.1 / 10.8 |
| 5-8 | 2,082 | 43.4 / 61.4 | 42.9 / 60.5 | 207.6 / 688.4 | 428.9 / 628.6 | 3.4 / 9.8 |
| 9-12 | 1,052 | 62.3 / 81.1 | 61.2 / 78.8 | 311.9 / 561.4 | 613.5 / 742.2 | 4.1 / 16.6 |
| >12 | 62 | 124.3 / 137.4 | 120.6 / 151.7 | 560.0 / 2321.1 | 1126.9 / 2194.0 | 7.5 / 10.7 |
| all | 22,009 | 26.9 / 65.8 | 26.6 / 65.6 | 123.0 / 452.1 | 264.4 / 653.3 | 2.7 / 12.3 |

By event count and tenor, as the tool printed it (`dev200b-divsched-standard.md`; p50 / p99 us,
coverage as quoted / priced / price_only / refused / budget):

| events | tenor | groups | contracts | setup us p50 / p99 | cold price | price | IV | bundle | quoted / priced / price_only / refused / budget | prices |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | <=0.25y | 3 | 150 | 2.5 / 2.9 | 19.2 / 59.2 | 19.0 / 24.3 | 78.8 / 1061.0 | 189.7 / 277.1 | 97 / 79 / 12 / 3 / 3 | 150 |
| 1 | <=0.25y | 84 | 4933 | 2.3 / 11.7 | 23.9 / 32.4 | 23.6 / 31.2 | 103.1 / 161.8 | 235.3 / 317.5 | 3578 / 3044 / 306 / 228 / 0 | 4933 |
| 1 | 0.25-1y | 108 | 6186 | 2.3 / 9.3 | 23.8 / 44.8 | 23.5 / 40.0 | 101.2 / 209.0 | 233.5 / 374.9 | 4739 / 4179 / 224 / 336 / 0 | 6186 |
| 1 | 1-2y | 2 | 224 | 2.2 / 2.7 | 24.7 / 30.6 | 24.3 / 27.1 | 101.1 / 339.9 | 239.6 / 265.9 | 212 / 191 / 8 / 13 / 0 | 224 |
| 2 | <=0.25y | 8 | 252 | 2.5 / 38.5 | 26.7 / 37.8 | 27.1 / 34.9 | 121.2 / 188.8 | 268.0 / 321.8 | 146 / 104 / 22 / 20 / 0 | 252 |
| 2 | 0.25-1y | 87 | 4032 | 2.6 / 11.5 | 28.7 / 52.8 | 28.5 / 50.7 | 128.6 / 345.7 | 283.9 / 503.1 | 3316 / 2998 / 86 / 229 / 3 | 4032 |
| 2 | 1-2y | 4 | 406 | 2.2 / 2.9 | 29.3 / 35.9 | 28.8 / 34.4 | 137.0 / 853.8 | 289.3 / 345.0 | 374 / 355 / 14 / 5 / 0 | 406 |
| 2 | >2y | 2 | 178 | 5.5 / 8.8 | 29.1 / 31.3 | 28.6 / 41.0 | 134.9 / 268.1 | 290.5 / 395.2 | 170 / 162 / 5 / 3 / 0 | 178 |
| 3-4 | <=0.25y | 2 | 46 | 4.9 / 7.5 | 31.6 / 66.2 | 30.8 / 49.5 | 136.3 / 164.3 | 304.0 / 535.6 | 27 / 22 / 0 / 5 / 0 | 46 |
| 3-4 | 0.25-1y | 34 | 1940 | 3.1 / 10.9 | 34.8 / 66.6 | 34.1 / 58.6 | 162.2 / 559.7 | 334.1 / 615.4 | 1753 / 1664 / 50 / 39 / 0 | 1940 |
| 3-4 | 1-2y | 4 | 294 | 3.0 / 9.2 | 36.8 / 42.3 | 36.4 / 42.0 | 168.3 / 356.9 | 363.9 / 401.0 | 282 / 276 / 0 / 6 / 0 | 294 |
| 3-4 | >2y | 1 | 172 | 4.4 / 4.4 | 33.8 / 38.8 | 33.5 / 38.0 | 162.6 / 224.7 | 335.1 / 1924.8 | 172 / 172 / 0 / 0 / 0 | 172 |
| 5-8 | 0.25-1y | 11 | 334 | 3.4 / 8.6 | 45.4 / 74.7 | 44.7 / 70.9 | 214.5 / 678.1 | 432.3 / 643.8 | 207 / 165 / 26 / 16 / 0 | 334 |
| 5-8 | 1-2y | 23 | 1566 | 3.2 / 9.7 | 43.3 / 54.3 | 42.8 / 56.4 | 209.1 / 658.0 | 428.5 / 654.4 | 1499 / 1457 / 21 / 21 / 0 | 1566 |
| 5-8 | >2y | 2 | 182 | 5.3 / 7.5 | 44.2 / 53.9 | 43.6 / 49.0 | 200.8 / 688.6 | 437.2 / 462.4 | 182 / 180 / 0 / 2 / 0 | 182 |
| 9-12 | 0.25-1y | 2 | 48 | 7.6 / 11.9 | 69.0 / 96.3 | 68.2 / 114.7 | 277.2 / 3121.5 | 584.7 / 855.8 | 25 / 18 / 0 / 7 / 0 | 48 |
| 9-12 | >2y | 18 | 1004 | 4.1 / 16.7 | 62.3 / 77.3 | 61.2 / 72.6 | 312.4 / 554.9 | 614.0 / 720.4 | 989 / 983 / 5 / 1 / 0 | 1004 |
| >12 | 1-2y | 3 | 62 | 7.5 / 10.7 | 124.3 / 137.4 | 120.6 / 151.7 | 560.0 / 2321.1 | 1126.9 / 2194.0 | 33 / 27 / 2 / 4 / 0 | 62 |

**Status against the budgets.** The factor is p50 over the budget, by event count.

| Route | P1 cold price | P2 price | P2 IV | P3 bundle | Verdict |
|---|---|---|---|---|---|
| Standard, 0-2 events | MISS 1.9-2.9x | MISS 3.8-5.7x | MISS 3.2-5.2x | MISS 9.5-14.3x | MISS |
| Standard, 3-12 events | MISS 3.4-6.2x | MISS 6.7-12.2x | MISS 6.5-12.5x | MISS 16.8-30.7x | MISS |
| Standard, > 12 events | MISS 12.4x | MISS 24.1x | MISS 22.4x | MISS 56.3x | MISS |
| Refined (synthetic ladder, 0-12 events) | MISS 58-87x | MISS 117-174x | MISS 74-109x | MISS 286-422x | MISS |

The Refined row is from the synthetic ladder run (`latency\f1\synthetic-refined.json`). Its
p50 is 585-872 us cold price, 1850-2713 us IV and 5714-8445 us bundle.

**Coverage.** dev200b divsched cohort, Standard route, the quote is the bid/ask mid:
- Prices: 22,009 of 22,009.
- IV: 17,801 quoted (4,208 contracts have no two-sided market).
  - priced (identified): 16,076, 90.3 %;
  - price_only: 781, 4.4 % (the quote is on the exercise value or at or below the lowest-vol
    lattice price; no volatility is identified);
  - refused (range): 938, 5.3 %. These mid quotes sit strictly below the exercise value, an
    inconsistent quote no model price reaches. Fix round 1 retyped them; r6 counted them as
    price-only (1,719 = 781 + 938);
  - refused by input: 0;
  - iteration budget exhausted: 6.

**Unresolved and unsupported sub-domains.**
- **UNRESOLVED: deep in the money before a late ex-date** (D0 rows F1c, F1p, F2c, F2p) **and
  cash near 60 % of a high-vol spot** (ILp). The reference cannot certify a root at the 1e-7
  IV contract on these rows. The Refined route still misses ILp by a decidable +4.49 bp. On
  F1c the bundle's vega is UNSUPPORTED (exercise-dominated, below); it was 1.3 % off.
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
- **Greeks.** An unsupported greek is NaN plus a bit and a typed reason, never 0.
  - Theta and charm are UNSUPPORTED (`ExDateInStencil`) when an in-life ex-date sits on steps
    0..2 of the route.
  - Vega is UNSUPPORTED (`ExerciseDominated`) on an American call that is exercise-dominated
    before an in-life ex-date: the cash beats the interest on the strike to the next ex-date,
    and the cum-dividend forward is at least 2 sd in the money (D0 F1c: the lattice's vega
    was +1.3-1.7 % off at every N).
  - The time roll across an ex-date is an event operation, not a derivative.
  - Accuracy class on the Refined route, D0's study rows (doc section 11.6):
    - delta within 2.3e-5 (Richardson pair);
    - vega within 0.073 % (Richardson bump pair);
    - rho within 0.24 %;
    - dP/dD within 1.3e-4;
    - roll within 1.3e-3 USD.
  These are classes, not reference-uncertainty claims.

**The dominating stage.** In the cold group clock, the inversions take 99.94 % and
`prepare()` (family_build_validation) the rest. Run f1:
- inverses 2.6377 s;
- family_build_validation 0.0016 s;
- unattributed 3e-7 s.

Per call, the floor is one rollback. The 301-step Standard rollback alone is 17.7 us at
0 events (run b, 2.1 GHz), already 3.5 times P2's 5 us. An IV needs 4-5 rollbacks, and the bundle
needs 9 or more. Lattice tuning does not close that gap. A prepared non-lattice
representation is required.

**The next experiment: one challenger, acceptance predeclared.**

The challenger is a prepared Chebyshev-in-sigma family per (group, strike).
- `prepare()` evaluates the Standard route at n Chebyshev nodes in sigma over the route's
  admissible range. n is the smallest count whose interpolant reproduces the route within
  the tolerance below at 2n check points.
- A prepared price is then an O(n) evaluation, and an IV is a bracketed root of the
  interpolant.
- The family is a pure function of the route's prices, so its accuracy is the route's plus
  the interpolation error. That error is checked at build time and refused, never served,
  when it is exceeded.

Budget: 2 days. Measured on the dev200b divsched cohort (398 groups, 22,009 contracts),
Standard route, B0's harness, one pinned P core.

| metric | target | rejected if |
|---|---|---|
| P2 prepared price, p50 | <= 5 us | > 5 us |
| P2 identified IV, p50 | <= 25 us | > 25 us |
| interpolation error vs the Standard route, every quoted contract | <= 0.1 bp of IV (keeps the 3 bp class against the reference) | any contract > 0.1 bp, or any refusal the route does not make |
| IV coverage | identified >= the route's 16,076; no new refusal class | fewer identified |
| setup (family_build_validation) p50 per group | reported, no target (P1 cold stays a MISS by design) | not reported |
| P3 bundle | reported: vega and the sigma legs come from the family, the other legs stay rollbacks | not reported |

If it fails, this row stays MISS and the recorded result is the measured table. The GIT
hybrid-dividend method is not a second candidate in this experiment.
