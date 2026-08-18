# Oracle RSI v2 — design

Status: DESIGN, 2026-08-17. Supersedes the *loop* half of
`2026-08-15-oracle-rsi-loop-design.md`. The broker / lease / capability half of v1
is KEPT verbatim and is not re-specified here.

Goal: make atx-vol the fastest and most accurate American equity option fitting
and pricing library, measured against SpiderRock srPrc / srVol / greeks.

---

## 0. What v1 actually is (measured at `main` 1af4e826, not assumed)

| Claim | Evidence |
|---|---|
| The improvement loop has NEVER executed | `vol-oracle-iter.js:1635-1649` returns `verdict:'FAILED'` before Measure; `:1651-1725` sits inside `/* RETIRED_READY_PATH */` |
| `vol-sprint` is a 19-line stub | returns `passed:false, failure:'ORACLE_BROKER_MIGRATION_REQUIRED'` unconditionally |
| Every ready/ratchet gate command is unrunnable | they pass `--mode --scorecard --aggregate-only --benchmark-speed --preset --quiet-host`; `oracle_bench_main.cpp:76-103` accepts only `--cohort --smoke --tune --convention-sweep --store --out --git-sha --iter`, requires `--store`/`--out`, hard-errors on unknown flags |
| `oracle/canonical` and `main` are FULLY DIVERGED | `git merge-base --is-ancestor` fails in both directions. canonical `e232a118` holds the Stage 1+2 receipts and none of the Stage 3 code; `main` holds the Stage 3 code and none of the receipts |
| No ratchet baseline exists | `iter-000.json` absent on every ref; no speed pin |
| Broker operations `measure`, `sprint_build`, `sprint_integration`, `ratchet` | registered, never opened by any caller |

v1 is a **bootstrap harness, 2 of 4 stages complete**. The convention sweep it
produced was hand-driven, not loop-driven. v2 is therefore not a refactor of a
working loop; it is the loop's first implementation on top of v1's genuinely
strong mutation-safety substrate.

**KEEP unchanged from v1**: the broker (HMAC-sealed capability records,
`timingSafeEqual`, root guard, lease revalidation by keeper PID + StartTime,
reparse-safe path containment, patch canonicality, single-use finalize capability
with `git update-ref` CAS, quarantine, sealed idempotent recovery);
`lease-worktree.ps1` and its keeper; the one-broker-verb-per-agent capability
partition; the "workflow computes the verdict, the agent only prepares evidence"
rule; and the `iterationCommandError` no-broad-builds policy.

---

## 1. The thesis

**v1 searched over UNITS. v2 must search over ESTIMATORS AND MODELS.**

v1's whole hypothesis vocabulary is a 31-key `ConventionMap` of scale factors. It
cannot express "compute theta as a one-sided forward difference on a 1/252
volatility-year clock", because that is a different *functional*, not a rescaling.

This is not speculative. The oracle documents its estimators, and they are finite
differences of a tree, not analytic derivatives.

---

## 2. What the oracle is (vendor-documented, fetched and verified 2026-08-17)

Source: `docs.spiderrockconnect.com/.../Analytics/OptionPricing/`, quoted directly.

- **Model**: "The SpiderRock pricing models use a numerical method (**modified
  binary trees w/splicing**) to price options with discrete dividends for American
  calls and puts (**Vellekoop & Nieuwenhuis, 2006**)." American *futures* options
  use **Ju & Zhong (1999)**.
- **Vega**: "recalculating the option price at **1% increments both up and down**
  and evaluating the slope via a **centered difference**."
- **Theta**: "always measured numerically, by calculating the difference between
  the current option price and the option price calculated with **volatility time
  decreased by 1/252 years**." One-sided forward difference. When time to
  expiration is under one volatility day, the next-day price is taken to be the
  expiration payout. Theta may go negative when an ex-dividend date falls within
  the next day.
- **Rho, Phi**: "recalculating the option price at a **1% higher rate** and
  evaluating the slope via a **right-handed difference quotient**." One-sided.
- **Theta SIGN**: "reported in terms of decay, that is, its value is reported as a
  POSITIVE value of how much option premium is expected to decay in 1 days' time."
- **Phi is dOpx/dSDiv — carry/borrow rho, NOT discrete-dividend sensitivity.**
  `OptionPositionRecordV5` gives the shorthand directly, and the PnL attribution
  fields confirm it: `opnPnlSDiv = optPhi * dSDiv` while `opnPnlDDiv = dePr *
  dDDiv` is annotated "uses BS delta not phi". A phi hypothesis that bumps the
  discrete dividend will never fit.
- **Clock**: hybrid, and the formula IS PUBLISHED — this is not a black box:

      Time = (TradingHoursRemaining) * alpha/1890
           + (NonTradingHoursRemaining) * (1-alpha)/6870

  1890 = 252 trading days x 7.5 h (6.5 h session + 1 h after close); 8760 - 1890 =
  6870. alpha = 0 recovers BUS/252; every worked example uses **alpha = 0.7**,
  justified by "roughly 70% of the price variance occurs during market trading
  hours and additionally during the hour immediately after market close". The
  production value of alpha is NOT stated.
  **But alpha is directly solvable from data we already hold**: our cohort carries
  both `years` (volatility time) and `yearsC` (calendar time). That pair
  over-determines alpha. Solve it, do not reverse-engineer it from theta.
  Errata to ignore: the VolTimeCalc page prints 1638 (= 6.5 x 252) once; the
  formula, the Option Pricing page, and the page's own sanity check (7.5/1890 =
  1/252) all use 1890. Use 1890.
- **Rates are quoted Act/365 but INTERPRETED in vol time**: "All rates are stored
  using an Act/365-day count convention but are interpreted in the platform using
  SpiderRock Vol Time." So discounting is exp(-rate * volTimeYears) with an
  Act/365-quoted rate. Default curve is SpxBox — risk-free rates implied from SPX
  box prices, not OIS or SOFR.
- **Expiration day special-cases**: American options switch to European, and
  rate = sdiv = carry = 0 are forced.
- **Accuracy target**: "around 1/10th of the minimum price variation" — approx
  $0.001 at a penny tick. Worked example uses **301 tree steps**, error "roughly
  half of $0.001", benchmarked against a **10,000-step CRR tree**.
- **Volga, vanna, charm**: NOT DEFINED anywhere in the documentation. They exist
  only as data fields. Definitions must be requested (their docs offer "technical
  notes provided upon request") or reverse-engineered.
- **`sdiv`**: from the Live Volatility Surfaces page, SpiderRock "continuously
  calibrates call and put implied volatilities for expirations by adjusting the
  **sdiv** pricing parameter to minimize the mismatch between call and put
  surfaces", sometimes as an sdiv *curve* across strikes. So sdiv is a fitted
  residual yield absorbing dividend error AND hard-to-borrow — it is not a
  physical dividend yield.

### 2.1 This independently confirms the Stage 3 sweep

The sweep fit 277,952 rows and resolved these empirically. The vendor documents
the same values by a completely different route:

| Sweep resolved | Vendor documents |
|---|---|
| theta day count `BUS_252` (1/252) | "volatility time decreased by 1/252 years" |
| `vega_scale = per_point` (0.01) | "1% increments both up and down" |
| `rho_scale`, `phi_scale = per_point` (0.01) | "a 1% higher rate" |
| `volga_scale = per_point_squared` (1e-4) | undocumented; 1e-4 = (0.01)^2 off those same bumps |

Two independent methods, one answer. Treat the Stage 3 unit resolution as
**CONFIRMED** and stop re-litigating it.

### 2.2 It also explains the residual structure

| Metric | Symmetric residual | Explanation |
|---|---|---|
| theta | 12.95% | we compute the instantaneous PDE identity; they compute a ONE-SIDED forward difference over 1/252 VOL-years on a HYBRID clock. Different quantities by construction. |
| rho / phi | 11.4% / 11.9% | they use ONE-SIDED right-handed quotients, O(h) biased at h=0.01 |
| vega | 8.1% | they use CENTERED +/-1%, O(h^2). Our second-best greek. Consistent. |
| price | large | they use a SPLICED RECOMBINING TREE for discrete dividends; we apply a PV/forward shift and reach calls via put-call symmetry, which is derived under a CONTINUOUS yield and does not survive an additive cash jump |

**The dominant residuals are model and estimator mismatch, not numerical error.
More Andersen-Lake precision closes none of them.**

### 2.3 There is a HARD reproducibility floor, and it is not ours

SpiderRock documents a **two-tier compute path**: "we continuously compute and
broadcast a collection of 'calibration records' which are computed using our high
precision models. These calibration records then allow us to quickly and
accurately compute prices, volatilities, and greeks using much faster analytic
techniques." The supporting enums are published — `CalcEngine = {None, FastHybrid,
NumericLow, NumericStd, NumericMax}`, `CalcPrecision = {None, Low, Normal, High,
Custom}`.

**Our 30-minute cohort file carries NO `calcEngine` or `calcPrecision` column.** So
for any given row we cannot tell whether the published value came from the
Vellekoop-Nieuwenhuis tree or from the fast analytic approximation. That is an
irreducible floor on how tightly srPrc can ever be reproduced, and it is a property
of the data, not of our pricer.

Three more published facts that bound the target:
- srPrc is defined as "SpiderRock calculated option price **from srVol**", and the
  2021 dictionary adds "may not always be within bid/ask". It is a model price
  evaluated at the surface vol, not an independent mark and not clipped to market.
- Their stated accuracy target is only ~1/10 of a tick.
- No published statistic exists anywhere for srVol -> srPrc round-trip agreement.

**Implied-vol tolerance floor (derived, not cited).** Error propagates as
Delta_sigma ~ Delta_p / vega. At their ~$0.001 price accuracy: an ATM 30d option on
a $50 underlier has vega ~ $0.05/vol point, so ~2 bp of vol from their convergence
error alone; a 25-delta wing at vega ~ $0.01 gives ~10 bp; and a deep-ITM American
put at the exercise boundary has vega -> 0, where implied vol is **not identified**
— a whole interval of sigma reproduces the price within any finite tolerance.

Consequence for targets: the charter's 5 bp Mode A vol target is at or below the
noise floor in the wings, and unachievable in principle near the boundary. v2 must
report NOT-IDENTIFIED for those cells rather than a number, and must not count them
as residual. SpiderRock publishes exactly the fields needed to detect this:
`loBound` ("minimum noarb opx zero volatility given sdiv ddiv years rate") and
`exValue` (early-exercise premium).

**`loBound` also settles the American bracketing question**: the correct lower
no-arb bracket for an American inversion is the ZERO-VOLATILITY price, not
intrinsic. The vendor computes and publishes it.

---

## 3. Cited frontier (every number carries a source)

### 3.1 Accuracy vs throughput — the only rigorous published frontier

Healy, "Pricing American options under negative rates", arXiv:2109.15157,
*J. Computational Finance*. Independent of the ALO authors; reports RMSE AND
options/second. (m = collocation knots, n = fixed-point iterations, l/p = the two
Gauss-Legendre orders. NOTE: QuantLib swaps the letters m and n.)

| Method | RMSE | opt/s individual | opt/s batch (10 spots) |
|---|---|---|---|
| ALO m=5,n=4,l=11,p=21 | 4.1e-5 | 39,040 | **179,705** |
| ALO m=7,n=8,l=15,p=31 | 4.9e-6 | 12,507 | 70,881 |
| TR-BDF2 m=20 | 7.1e-4 | 4,708 | 36,375 |
| TR-BDF2 m=40 | 1.8e-4 | 1,330 | 10,010 |

ALO beats a good FD solver by 10-30x at equal accuracy at short maturity.
**Batching over 10 spots gives 4.6x** — amortisation of the boundary solve, NOT
SIMD. Long maturity (T>=5y): iterations, not nodes, are the lever (n=4 -> 1.4e-3,
n=16 -> 1.4e-4).

Independent production reference: Gituliar / TastyHedge 2024, QuantLib
`QdFpAmericanEngine` fast scheme on one AMD Ryzen 9 core — **45,000 opt/s**
pricing, **16,500 opt/s** for IV calibration, **2,800,000 opt/s** European IV via
Jaeckel. American IV therefore costs ~2.7x an American price.

### 3.2 The ALO convergence ladder — liftable as a gate fixture

QuantLib `test-suite/americanoption.cpp::testAndersenLakeHighPrecisionExample`
encodes the ALO paper's own high-precision example: American put, S=K=100,
q=0.05, sigma=0.25, T=1y, comparing the early-exercise PREMIUM.

| (l, iters, nodes) | tolerance |
|---|---|
| (5,1,4) | 1e-3 |
| (11,2,5) | 1e-4 |
| (24,3,9) | 1e-6 |
| (35,8,16) | 1e-9 |
| (65,8,32) | **1e-11** |

Golden premia at the 1e-11 rung:
- r=0.050: FP-A `0.1069527028247546`, FP-B `0.1069526779971959`
- r=0.075: FP-A `0.3671112309062572`, FP-B `0.3671111267813689`

### 3.3 Greeks — what is proven

- **Frozen-boundary vega is wrong by 3.35%-46.46%** even from an exact boundary;
  corrected 0.0005%-0.03%. Liu, Cui & Zhang (2016), *Finance Research Letters*
  19:204-208. They name ALO as affected literature. The correction is a SECOND
  Volterra equation for dB/dsigma with terminal condition dB_T/dsigma = 0.
  Independently corroborated by a different method: Capriotti, Jiang & Macrina
  (2017), *Algorithmic Finance* 6:35-49 — frozen regression coefficients bias vega
  specifically.
- **Delta and gamma with the boundary held fixed are EXACT**, because B(.) has no
  spot dependence. Not an approximation. `american_greeks_al` is correct here.
- **Rho and phi share vega's structure** (B depends on r and q). NOT tested in the
  literature. Assume the correction is needed until measured.
- **One order of convergence is lost per S-derivative.** Giles & Carter (2006),
  *J. Computational Finance* 9(4):89-112, Table 1: with 2R Rannacher half-steps,
  European call orders are V=2R+1, Delta=2R, Gamma=2R-1. R=2 optimal; MORE damping
  is WORSE.
- **Rannacher is necessary but NOT SUFFICIENT for American Delta/Gamma.**
  in 't Hout (2024), arXiv:2401.13361: CN+Rannacher needs N >~ m/4 with a
  problem-dependent constant; extra damping steps do not remove the restriction.
  Use DIRK theta = 1 - sqrt(2)/2 on grid t_n = (n/N)^2 T.
- **Never bump a binomial tree for gamma — you get exactly ZERO.** Pelsser & Vorst
  (1994). For small h the exercise indices are unchanged, so Delta is locally
  constant and Gamma identically 0. Holds for American.
- **Second-order American greeks are a research gap.** The only published
  treatment is Wallner & Wystup (2004), *Wilmott* Nov 2004: Leisen-Reimer trees,
  h=0.05 for volga (FIVE vol points), h_S=0.003 / h_v=0.03 for vanna, and
  second-order stencils BEAT fourth-order ones because the value function is not
  accurate enough to support the higher order. **Charm has no source at all.**
- **AAD through a fixed point**: Christianson (1994), *Optimization Methods and
  Software* 3(4):311-326 — the adjoint satisfies its own fixed point converging at
  least as fast as the forward one, no taping. Henrard (2014) for the finance
  mechanics. Cost bound 3-4x the function (Griewank & Walther 2008). This is what
  `adjoint_greeks.hpp` already implements.
- **AAD does not extend cleanly to second order.** Maran, Pallavicini & Scoleri
  (2021), arXiv:2106.12431.
- **QuantLib's `QdFpAmericanEngine` assigns only `results_.value`** — the reference
  ALO implementation produces NO greeks. Our adjoint work is ahead of it.

### 3.4 Discrete dividends — the crux, with numbers

The consistent model is **piecewise GBM** with a deterministic jump S -> S-D.
The **escrowed model is inconsistent** (the implied stock process depends on which
option you are pricing), and **Roll-Geske-Whaley inherits that flaw**.

Measured escrowed-model error, Le Floc'h arXiv:2106.12051 Tables 1-2
(S=100, sigma=30%, r=0%, T=1, single dividend 7):

| dividend at | strike | exact (HHL) | escrowed BS | error |
|---|---|---|---|---|
| t=0.1 | 100 | 8.42464720 | 8.33854820 | -1.0% |
| t=0.9 | 100 | 9.07480014 | 8.33854820 | **-8.1%** |
| t=0.9 | 150 | 1.06252881 | 0.82943120 | **-21.9%** |

**The escrowed prices are numerically IDENTICAL for both ex-dates** (r=0 makes the
PV timing-independent) while the true price moves +7.7% ATM and +23.6% at K=150.
The escrowed model is structurally blind to dividend timing. Bos-Vandermark's
hybrid shift cuts the vol-point error 40-70x.

Worse for greeks — Veiga & Wystup (2009), multi-dividend, at K=100:
gamma 77.35 exact vs 103.25 escrowed (+33%); vega 80.77 vs 60.49 (-25%).

Worst near expiry — Buryak & Guo arXiv:1407.7328 Table 1: dividend one day before
a 1y expiry, escrowed-with-vol-adjustment is **+43.9%** wrong.

QuantLib's own test `testEscrowedVsSpotAmericanOption` rescales
`sigma_escrowed = sigma_spot * S/(S-D)` — 30% -> 33.33% for a dividend of 10% of
spot — just to make the two engines agree to 1e-2.

**Vellekoop & Nieuwenhuis (2006)**, *Applied Mathematical Finance* 13(3):265-284:
a standard RECOMBINING tree on the dividend-free process, with the continuation
value INTERPOLATED across the dividend jump at each ex-date. Arbitrage-free,
convergent, accurate for small and large dividends. Nardon & Pianca measure the VN
interpolation tree matching the exact HHL benchmark to ~1e-4, while forced-
recombination hacks (averaging, stretching) are wrong by 4-20% in the wings.
**This is what SpiderRock uses, by name.**

**Merton (1973)**: American call early exercise is confined to the instant just
before an ex-date — a comparison at each ex-date, not a continuous boundary.

**Haug, Haug & Lewis (2003)** give the exact single-dividend American-call integral.
An official errata exists (Finance Press, 2016; Table 6 T=2 15.1989 -> 15.2007,
T=3 18.5984 -> 18.6002). Read it before implementing.

**The boundary is NON-MONOTONE under discrete dividends.** Jourdain & Vellekoop,
arXiv:0911.5117: the exercise boundary "may no longer be monotone", tends to 0 as
t -> t_div^- at a characterised speed, and jumps back after. One global
Chebyshev-in-sqrt(tau) interpolant cannot represent this.

**ALO + discrete dividends is UNSOLVED in the literature.** QuantLib's
`QdFpAmericanEngine` has ZERO dividend support (grep for `dividend` returns
nothing). Put-call symmetry, which the ALO call path uses, is derived under a
continuous yield and the additive jump is not invariant under S<->K. Any ALO
extension must be SPLICED PER INTER-DIVIDEND INTERVAL. Itkin arXiv:2510.18159
(2025/26) is the only candidate method and is unreproduced.

### 3.5 Speed levers, ranked by measured evidence

1. **Boundary amortisation across the strike ladder.** B(tau;K) = K*b(tau) by
   degree-1 homogeneity — Alexander & Nogueira (2007) state it for American
   vanillas explicitly. Healy measures **4.6x-5.7x**. Under homogeneity, "reuse
   across 10 spots at fixed K" and "reuse across 10 strikes at fixed S" are the
   same computation. **No paper claims this as a contribution** — the literature
   treats it as an implementation detail.
   **CONSTRAINT: homogeneity BREAKS with discrete cash dividends** (the problem
   then depends on D/K). So this lever and the VN model are in tension and must be
   scoped per regime — full reuse in the no-dividend regime, per-inter-dividend-
   interval reuse otherwise.
2. **Preset rung selection.** `al_bulk_opts` already measured at 25.8us/op vs
   `al_fast_opts` 46.7us/op (1.81x) at accuracy adequate for the oracle.
3. **Boundary warm-start across sigma-Newton steps during IV inversion.** American
   IV costs ~2.7x an American price (TastyHedge 45,000 -> 16,500 opt/s), and every
   Newton step currently re-solves the boundary. **No published work exists on
   continuation/homotopy of the boundary across contracts or across sigma.** This
   is the most promising unexploited lever and is genuinely open ground.
4. **SIMD.** ispc's `binomial_put` example IS American; published 7.94x on 1 core
   AVX, 14.83x AVX-512. Panova et al. (2022) measure only 2.5x from vectorisation
   alone on a Black-Scholes kernel, and find AoS ~= SoA on Cascade Lake. Lowest
   confidence; strictly after 1-3.

### 3.6 Existing house anchors

`bench/ANCHORS.md` already governs external claims: American 45,000 prices/s/core,
QuantLib QdFp ~39k single / ~180k batch, ALO ceiling ~100k/s/core, Jaeckel
European IV 2,800,000/s. House rules: cite CPU + URL, never compare an American-IV
rate against the European figure.

**ANCHORS.md records that NO published American-GREEKS throughput number exists.**
Combined with "no public American greek accuracy benchmark exists" (Chung &
Shackleton flagged it in 2002; still true in 2026), this is an unoccupied
benchmark and the clearest available claim to state of the art.

---

## 4. v2 architecture

### 4.1 Three-way attribution (the core new capability)

v1 compares model vs oracle only, so a residual is unattributable. v2 adds an
INTERNAL TRUTH reference:

| model vs truth | truth vs oracle | verdict |
|---|---|---|
| differs | — | OUR PRICER has real numerical error |
| agrees | differs | CONVENTION or VENDOR-MODEL difference |
| agrees | agrees, model differs | HARNESS BUG (impossible otherwise) |
| model inside NBBO, oracle outside | — | ORACLE-SUSPECT, excluded from the ratchet |

The truth reference is NOT SpiderRock. Per regime:
- no discrete dividend: ALO at the (65,8,32) rung, validated against the golden
  premia in 3.2
- discrete dividend: Vellekoop-Nieuwenhuis at high step count, cross-checked
  against a 10,000-step CRR (the reference SpiderRock itself benchmarks against)
  and, for single-dividend American calls, against the HHL exact integral
- greeks: Richardson-extrapolated central differences of the truth pricer at the
  Wallner-Wystup bump sizes. No public reference set exists, so we build one.

This is what finally fills v1's unused `oracle_suspect_candidates` and
`market_evidence_status` keys: SpiderRock's own price error is ~5e-4, so residuals
at that scale are partly THEIR discretisation, and the market quote is the tiebreak.

### 4.2 The estimator layer (replaces scale-only conventions)

`ConventionMap` gains an ESTIMATOR spec per greek, not just a scale:

    GreekEstimator {
      kind:  analytic | adjoint | centered_fd | forward_fd | backward_fd | tree_node
      bump:  double            // in the natural unit of the argument
      clock: calendar | bus252 | vol_time | hybrid_srock
    }

Pinned starting hypotheses from section 2 — vendor-documented, so these are
reproduction targets, not guesses:

| greek | kind | bump | clock |
|---|---|---|---|
| vega | centered_fd | 0.01 | — |
| theta | forward_fd | 1/252 | vol_time |
| rho | forward_fd | 0.01 | — |
| phi | forward_fd | 0.01 | — |
| delta, gamma | analytic | — | — |
| volga, vanna | UNKNOWN — search | — | — |

This is the structural change that lets the loop close theta/rho/phi.

### 4.3 Coverage matrix (breadth)

v1 bands by DTE only, so a broken regime hides inside a global mean. v2 scores
every cell of

    moneyness x DTE x rate-sign x dividend-presence x vol-level x side x
    early-exercise-proximity

with per-cell n, and gates on WORST-CELL as well as aggregate. Regimes the
literature names as failure modes get mandatory cells: negative rate / double
boundary (q<r<0), deep ITM under low carry, very short dated, dividend just before
expiry for calls, T>=5y.

### 4.4 One schema source

v1 duplicates the 31-key map across five layers with `additionalProperties:false`
on each, so one new degree of freedom is a five-file change that fails LATE — after
a 17-minute sweep. v2 defines the schema ONCE, generates/imports it into C++,
PowerShell and JS, and adds a seconds-long conformance preflight proving all layers
agree BEFORE anything expensive runs.

### 4.5 Objective discipline

Every objective ships with a written adversarial proof that it cannot be gamed by
scale collapse, BEFORE any search uses it. v1 lost most of an iteration to a metric
whose denominator floor manufactured three of four apparent results. The symmetric
loss |m-o|/max(|m|,|o|,floor) is the criterion; the standard-relative array is
published beside it for charter comparability and is never gated.

Bounded regression (candidate <= baseline*1.01) with a PUBLISHED
`accepted_regressions` array cross-checked in BOTH directions is kept from v1 — it
is the only rule that is satisfiable when eleven objectives share one map.

### 4.6 Research gate

No hypothesis enters a build lane without either a cited reference implementation
or an explicit NOVEL marker with justification. `.agents/research/agent.md` already
defines the role (>=2/3 refute-votes to kill, source taxonomy, verified vs
inferential separation); v2 wires it in as a mandatory stage.

### 4.7 The loop

    Measure    3-way scorecard over the coverage matrix + speed
    Attribute  classify each residual cluster: pricer-error | convention |
               vendor-difference | oracle-suspect
    Research   cited design note for the top hypothesis (mandatory)
    Plan       file-disjoint lanes
    Build      parallel pool-leased broker lanes
    Review     adversarial, fresh, at exact SHA
    Gate       correctness vs truth | accuracy vs oracle | speed vs pin |
               determinism | coverage floor
    Ratchet    commit baseline, update memory, CAS canonical

---

## 5. Prerequisites (blocking)

| # | Item | Why blocking |
|---|---|---|
| P1 | Reconcile `oracle/canonical` with `main` | the probe reads receipts at canonical; the Stage 3 code is on main; a Stage 3 lane branches from a base lacking the sweep tool |
| P2 | Commit `iter-000.json` + speed pin | no ratchet baseline exists, so `residual_floor` and `convention_speed` cannot pass |
| P3 | Implement the CLI flags the ready gates invoke, or rewrite the commands | all six ready/ratchet gate commands die at argument parse |
| P4 | Rebuild the Improve stage broker-native | `vol-sprint` is a fail-closed stub; the ready path is commented out |
| P5 | Make pricing suites reachable | `AmericanGreeks.*`, `AndersenLake.*`, `CallGreeksAl.*` live in `atx_vol_slow` and the loop bans `-L` labels |
| P6 | Refresh stale docs | NORTHSTAR says conventions UNRESOLVED (they are pinned and merged); `vol-dag` SKILL.md dispatches the dead `vol-sprint`; TEMPLATES.md:72 contradicts cpp/agent.md:178 on clang-format |

---

## 6. Ordered work programme

Priority by measured evidence, not by appeal.

1. **Vellekoop-Nieuwenhuis spliced binomial.** The price residual is a MODEL
   mismatch. Escrowed error is measured at -8.1% ATM / -21.9% OTM for a late
   dividend, +43.9% for a dividend just before expiry, and it is structurally blind
   to ex-date timing. Put-call symmetry is invalid in its presence.
2. **Estimator layer, reproducing SpiderRock's finite differences.** Closes
   theta/rho/phi, which are definitional gaps. Fit the convention, then the number.
3. **Internal truth reference + the American greek benchmark set.** Unblocks
   three-way attribution; no public equivalent exists anywhere.
4. **Boundary amortisation across the strike ladder** (no-dividend regime; per
   inter-dividend interval otherwise). 4.6-5.7x measured.
5. **Boundary warm-start across sigma-Newton steps in IV inversion.** American IV
   costs 2.7x a price; this lever is unpublished.
6. **Mode B: price -> vol inversion.** Currently 0 bp BY IDENTITY because vol is an
   input. Half the stated goal is untested.
   European inversion is SOLVED — do not invent anything. Port Jaeckel's "Let's Be
   Rational" structure verbatim: normalise to OTM via put-call parity, enforce
   `intrinsic <= p < F` (call) / `< K` (put) BEFORE iterating, four-branch rational
   initial guess built off sigma_c = sqrt(2|x|), three-branch objective,
   Householder(3), **exactly two iterations**, relative termination on sigma.
   Critically, implement the FOUR-REGIME normalised Black function (n=17 divergent
   asymptotic series / 12th-order Taylor in t / Cody erfc / Cody erfcx), because
   Jaeckel's own analysis is that the Black function's smoothness — not the
   iteration count — sets the accuracy floor. Budget ~180 ns/call in optimised
   C++ (his own measurement, 12th-gen i5, >5.5M IV/s). Return SENTINELS, never
   NaN: -DBL_MAX below intrinsic, +DBL_MAX above max, 0 for subnormal prices.
   For AMERICAN inversion the lower bracket is the ZERO-VOLATILITY price
   (`loBound`), not intrinsic, and the boundary warm-start of item 5 belongs
   inside the sigma-Newton loop.
7. **Volga/vanna definition resolution.** Undocumented by the vendor; request
   technical notes, search estimators meanwhile.
8. **Speed ratchet toward SOTA.** The unoccupied claim is American GREEKS
   throughput.

---

## 7. Non-goals and traps

- Do NOT chase price residuals below ~5e-4. That is SpiderRock's own discretisation
  error against a 10,000-step CRR. Below it we fit their noise. Route such cells to
  the oracle-suspect list.
- Do NOT use BAW beyond a boundary seed: 30-60 cents of error at 3y (Ju & Zhong
  1999, Exhibit 5).
- Do NOT bump a binomial tree for gamma (Pelsser & Vorst 1994).
- Do NOT add Rannacher damping beyond R=2 hoping for accuracy; more is worse.
- Do NOT assume boundary-across-strike reuse is valid under discrete dividends.
  Homogeneity breaks there.
- Do NOT tune against holdout. Membership is frozen; only Ratchet may benchmark it.
- Do NOT state a greek accuracy claim against an external number. None exists
  publicly; the honest claim is against our own truth reference.

---

## 8. Open questions

1. SpiderRock's volga/vanna/charm definitions are undocumented. Their docs offer
   "technical notes provided upon request" — cheapest possible win, ask directly.
2. RESOLVED as of this revision — the hybrid clock formula is published; only
   `alpha` is not, and it is solvable from the `years` / `yearsC` pair already in
   our cohort. Do this first; it is cheap and it unblocks theta.
3. Their tree step count is adaptive; 301 documented for one example, rule not
   published. The `CalcPrecision` / `CalcEngine` enums exist but their step counts
   are not.
4. Their `sdiv` is a FITTED residual absorbing dividend error and borrow. Matching
   srPrc may require reproducing the sdiv calibration, not just the pricer.
5. ALO with discrete dividends has no literature; a spliced per-interval ALO would
   be original work with no prior art.
6. No published ALO analytic greeks exist; `adjoint_greeks.hpp` has no external
   benchmark except bump-and-revalue convergence plateaus.
7. Whether the rho/phi boundary-sensitivity correction matters as much as vega's is
   untested in the literature.
8. Spot-vs-forward IV convention for American options with discrete dividends
   appears to be a genuine gap in the academic literature.
