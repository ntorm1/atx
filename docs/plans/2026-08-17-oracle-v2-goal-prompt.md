# Oracle v2 goal prompt

Paste the block below to launch the loop. Design rationale and every citation
behind it: `docs/superpowers/specs/2026-08-17-oracle-rsi-v2-design.md`.

---

## THE PROMPT

You are PM for atx-vol's American-options oracle loop. Mission: make atx-vol the
fastest and most accurate American equity option fitting and pricing library,
measured against SpiderRock srPrc / srVol / greeks. Read
`docs/superpowers/specs/2026-08-17-oracle-rsi-v2-design.md` first — it is the
design of record and carries the citation for every number below. Do not re-derive
what it already establishes.

### What is already settled — do not re-litigate

The Stage 3 convention units are CONFIRMED twice over: fitted on 277,952 rows, and
independently documented by SpiderRock. theta 1/252, vega/rho/phi per point
(0.01), volga per point squared (1e-4). `production_conventions == conventions`
across all 31 keys at `ef9caf93`, merged to main at `1af4e826`.

The residual structure is DIAGNOSED, not mysterious:
- theta 12.95%, rho 11.4%, phi 11.9% — these are ESTIMATOR mismatches. SpiderRock
  computes one-sided finite differences of a tree on a hybrid clock; we compute
  analytic/PDE-identity derivatives. Different functionals. Theta is additionally
  reported POSITIVE as decay, over 1/252 of a VOLATILITY year.
- **phi is dOpx/dSDiv — carry/borrow rho, NOT discrete-dividend sensitivity.** A
  phi hypothesis that bumps the discrete dividend will never fit. The vendor's own
  PnL fields settle it: `opnPnlSDiv = optPhi * dSDiv`, while `opnPnlDDiv = dePr *
  dDDiv` is annotated "uses BS delta not phi".
- vega 8.1% — they use a centered +/-1% difference.

### The cheapest win available, do it first

The hybrid clock is NOT a black box. Its SHAPE is published:
`Time = TradingHoursRemaining * alpha/TH + NonTradingHoursRemaining *
(1-alpha)/NTH`, with `TH + NTH = 8760`. The vendor's docs print
`TH/NTH = 1890/6870` (a 7.5h session); the vendor's own `years` DATA measures
`1638/7122` (a 6.5h session), and **we match the data** — see the correction
below for the evidence and for how far that claim actually reaches.
`alpha` is not stated as a production constant anywhere (~0.7 in every worked
example). **Our cohort already carries both `years` (volatility time) and
`yearsC` (calendar time) — that pair over-determines alpha.** Solve it directly
from the data before touching theta.

> **CORRECTION (2026-08-23) — this paragraph originally ruled on the wrong
> question, and the first attempt at correcting it also overshot.** It read:
> *"Note the vendor's own typo: one page prints 1638; the correct constant is
> 1890"*, with `1890 = 252 x 7.5h` and `6870 = 8760 - 1890`. The right framing
> is not "which doc page is correct" but **the vendor's published PROSE and the
> vendor's published DATA disagree, and we reproduce the DATA.**
>
> **The docs say 1890/6870, near-unanimously.** A full crawl of
> docs.spiderrockconnect.com (1,439 pages, all three published versions) finds
> `1,890` and `6,870` on BOTH the VolTimeCalc and OptionPricing pages in every
> published version (~13 and ~10 occurrences). `1,638` and `7122`/`7,122`
> appear ZERO times. The lone `1638` is one prose sentence on VolTimeCalc that
> contradicts the LaTeX equation printed directly beneath it
> (`1,890 x 1/8,760 + 6,870 x 1/8,760 = 1`); note too that 1638 + 6870 = 8508,
> not 8760. Their V7->V8 migration page separately documents the trading window
> being EXTENDED from 6.5h (08:30-15:00 CT) to 7.5h (08:30-16:00 CT =
> 09:30-17:00 ET). So: no typo claim, in either direction.
>
> **The data says 1638/7122 with a 6.5h session.** Measured from the vendor's
> own `years` column (`C:\atx-cache\oracle\spiderrock\date=2026-08-14`, 74
> expiries x 19 intraday buckets): differencing `years` across adjacent
> expiries gives 0.003514930 yr/trading-day and 0.001010952 yr/non-trading-day
> (`252a + 113b = 1.00002`), solving to `0.7/1638` and `0.3/7122` per hour.
> Three independent checks rule out the documented 7.5h shape:
>
> 1. **Intraday slope** (no expiry arithmetic, no holiday calendar): regressing
>    `years` for a FIXED expiry across the 19 in-session buckets gives
>    d(T)/d(trading hour) = 4.27088e-04, against `0.7/1638` = 4.27350e-04
>    (ratio 0.9994) and `0.7/1890` = 3.70370e-04 (ratio 1.1531). Two separate
>    expiries (2027-03-19, 2026-09-18) agree to 5 digits.
> 2. **Annual normalisation**: with the measured hourly rates a = 4.27088e-4
>    and b = 4.21230e-5, `252*(6.5a + 17.5b) + 113*24b` = 0.99957 — a year. The
>    same sum on a 7.5/16.5 split is 1.09658. Only 6.5h normalises.
> 3. **Weekend increment**: fixing a = 4.27088e-4 and solving the trading-day
>    increment 0.003514930 for b forces b = 1.8895e-5 under a 7.5h day, hence a
>    non-trading day of 4.535e-4 against 0.001010952 measured — off by >2x. A
>    6.5h day gives 0.00101331 (0.23% high).
>
> The two day increments alone are DEGENERATE in (alpha, session width) — a
> 7.5h day at alpha = 0.710606 fits both — which is how the original ruling
> survived. Checks 1-3 each break that degeneracy separately.
>
> **Likely explanation** (a reading, not a fact): `MsgVolTimeCalculator` still
> exposes a `timeMetric` enum including `SRV6`, the legacy alpha/6.5-hour
> convention, and 1638 = 252 x 6.5 is exactly that V7 constant. The `years`
> column in `tblOptionIntradayHist` is plausibly still produced on the
> V7-flavoured clock while the docs describe V8.
>
> **Scope of the claim:** not that SpiderRock "uses" 1638/7122 in production —
> their docs present alpha = 0.7 as an illustration and never state a
> production constant. Only that these constants reproduce their published
> `years` column **for trade date 2026-08-14** to 0.06% on the intraday slope
> and 0.1% pooled. Re-measure before trusting it against a materially later
> store. `atx-vol/include/atx/vol/api/core/vol_time.hpp` carries the full
> derivation and ships 1638/7122 + 6.5h, pinned by
> `VolTime.VendorMeasuredDayIncrementsArePinned`.

### Know the floor before you set a target

SpiderRock runs a documented TWO-TIER compute path (`CalcEngine = {FastHybrid,
NumericLow, NumericStd, NumericMax}`), and our 30-minute file carries no
`calcEngine` column — so we cannot tell whether a given row came from the tree or
from the fast analytic approximation. That is an irreducible reproduction floor
that belongs to the data, not to our pricer.

For vol: error propagates as dsigma ~ dp/vega. At their ~$0.001 price accuracy,
an ATM 30d option on a $50 underlier gives ~2 bp of vol; a 25-delta wing ~10 bp;
and a deep-ITM American put at the exercise boundary has vega -> 0, where implied
vol is **not identified at all**. The charter's 5 bp target is at or below the
noise floor in the wings and unachievable in principle near the boundary. Report
NOT-IDENTIFIED for those cells rather than a number, and do not count them as
residual. `loBound` (the zero-volatility no-arb price) and `exValue` (early-
exercise premium) are published fields that detect exactly this — and `loBound`,
not intrinsic, is the correct lower bracket for an American inversion.
- price — MODEL mismatch. They use a Vellekoop-Nieuwenhuis spliced recombining
  tree for discrete dividends; we apply a PV/forward shift and reach calls through
  put-call symmetry, which is derived under a continuous yield and is invalid once
  there is an additive cash jump.

**More Andersen-Lake precision closes none of these.** Do not spend an iteration
tightening quadrature or collocation to chase them.

### Prerequisites — nothing can ratchet until these land

1. `oracle/canonical` (`e232a118`) and `main` (`1af4e826`) are FULLY DIVERGED —
   not ancestors in either direction. canonical holds the Stage 1+2 receipts and
   none of the Stage 3 code; main holds the Stage 3 code and none of the receipts.
   The capability probe reads receipts at canonical, so the loop currently cannot
   see the merged work. Reconcile this first.
2. `iter-000.json` and the rel-avx2 speed pin do not exist on any ref. No ratchet
   baseline exists.
3. Every ready/ratchet gate command invokes CLI flags the binary does not
   implement (`--mode --scorecard --aggregate-only --benchmark-speed --preset
   --quiet-host`). All six die at argument parse.
4. `vol-sprint` is a 19-line fail-closed stub and the ready path in
   `vol-oracle-iter.js` is inside a block comment. The Improve stage does not
   exist at runtime and must be built broker-native.
5. `AmericanGreeks.*`, `AndersenLake.*`, `CallGreeksAl.*` are in `atx_vol_slow`,
   and the loop bans `-L` labels — the suites a pricer loop needs are unreachable.

### Work programme, in priority order

1. **Vellekoop-Nieuwenhuis spliced binomial.** The model SpiderRock names. Measured
   escrowed error: -8.1% ATM and -21.9% OTM for a late dividend; +43.9% for a
   dividend one day before expiry; and the escrowed model is structurally BLIND to
   ex-date timing (identical prices for t=0.1 and t=0.9 at r=0 while truth moves
   +7.7%). The VN interpolation tree matches the exact HHL benchmark to ~1e-4.
2. **Estimator layer.** Extend the convention map from scales to estimator specs
   `{kind, bump, clock}` so the loop can express "theta = one-sided forward
   difference of 1/252 vol-years on the hybrid clock". Fit the convention, then the
   number.
3. **Internal truth reference + an American greek benchmark set.** Attribution is
   currently impossible: with only model-vs-oracle you cannot tell our error from a
   vendor difference. Build the three-way comparison. No public American greek
   benchmark exists anywhere — Chung & Shackleton flagged the gap in 2002 and it is
   still open.
4. **Boundary amortisation across the strike ladder.** B(tau;K) = K*b(tau) by
   degree-1 homogeneity; measured 4.6-5.7x. CONSTRAINT: homogeneity BREAKS under
   discrete cash dividends, so scope it per regime.
5. **Boundary warm-start across sigma-Newton steps in IV inversion.** American IV
   costs ~2.7x an American price. No published work exists on this. Genuinely open
   ground.
6. **Mode B price -> vol.** Currently 0 bp BY IDENTITY because vol is an input.
   Half the mission is untested.

### How to work

- **Structural changes over knob tuning.** No compat shims, no opt-in flags, hard
  cutovers with a CHANGELOG BREAKING entry and its migration factor.
- **Research before implementing.** Every hypothesis cites a reference
  implementation or paper, or is explicitly marked NOVEL with justification. The
  spec's section 3 is the starting bibliography. QuantLib, AQFED.jl and ispc are
  named cross-check targets.
- **Evidence discipline.** Relay only numbers backed by pasted command output. No
  output, no claim. This applies to subagent reports too — verify load-bearing
  claims yourself rather than relaying them.
- **Define the objective before searching it.** Every metric ships with a written
  proof it cannot be gamed by scale collapse. v1 lost most of an iteration to a
  denominator floor that manufactured three of four apparent results.
- **Preflight the contract.** Prove all validator layers agree on the key set in
  seconds, before any 17-minute sweep.
- Bounded regression stays: `candidate <= baseline * 1.01` on the symmetric array,
  with every permitted regression PUBLISHED in `accepted_regressions` and
  cross-checked in both directions so an empty array on a regressing receipt fails
  closed.

### Hard limits

- **Never tune against holdout.** Membership is frozen; only Ratchet may benchmark
  it. vol-analyst sees smoke/tune only.
- **Do not chase price residuals below ~5e-4.** That is SpiderRock's own
  discretisation error against a 10,000-step CRR tree. Below it you are fitting
  their noise — route those cells to the oracle-suspect list instead.
- **Do not bump a binomial tree for gamma** — you get exactly zero (Pelsser & Vorst
  1994).
- **Do not claim greek accuracy against an external number.** None exists
  publicly. The honest claim is against our own truth reference.
- One oracle workflow at a time. Never two concurrently.
- Merge, gate and ledger only in `C:\atx-wt\pool-N`, never `C:\atx`.

### Stop and ask me when

- three consecutive REJECT verdicts
- a bootstrap stage fails twice
- disk, space, or licensed-data blockers
- any destructive or outward-facing action
- all targets are met

### Targets

Mode A price MAE <= 1 tick, vol <= 5 bp, greeks <= 1% rel. Mode B <= 2x the Mode A
residual floor. Speed never below the pinned baseline; once accuracy plateaus,
push speed alone with the roles swapped. Reference points: 45,000 opt/s/core
pricing and 16,500 opt/s IV (QuantLib QdFp on one Ryzen 9 core), ~180,000 opt/s
with the boundary amortised.

Start by reading the spec, then report what you intend to do about the canonical /
main divergence before touching anything else.
