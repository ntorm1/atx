# Analytics contracts: declared answers on the live serve book (A1, r6)

Status: lane A1 (vol-sota r6, 2026-10-23; master plan Sprint 5 package A1, gate G6; deep
review F11 / F13). New: `include/atx/vol/api/analytics/served_analytics.hpp` (hedge Greeks,
scenarios, attribution, aggregation, the D1 cash route), `common_model_analytics.hpp`
(European-only outputs), `served_session.hpp` (a curve session served through the live
book), their `src/analytics/*.cpp`, the focused test target `atx-vol-served-analytics-tests`
(ctest prefix `served-analytics:`) and the bench `atx-vol-served-analytics-bench` (the A1
row). Nothing on a fitting or serving path changed: no kernel, fitter, serve-record or
serve-book file is touched, and `VolaSession` / `PricedSurface` / `chain_tick` are consumed
through their public APIs only.

The headers are the API; this file is the contract they cite (section numbers are stable).

## 1. One declared model, version, convention and clock per answer

Every answer carries an `AnalyticsDeclaration`:

| field | what it pins |
|---|---|
| `book_version` | the `ServeBookVersion::number` the reader was pinned to when it answered |
| `expiry_id`, `entry_tag` | the book entry |
| `record_fit_sha256` / `record_policy_sha256` / `record_input_sha256` | the record's identity (`ServeRecordVersion`) |
| `model` | `AnalyticsModel`: `QuotingRecord`, `ColdKernel`, `EventSettlement`, `DiscreteDividend`, `SessionCurve`, `CommonModelEuropean` |
| `greek_model` | the map the Greeks differentiate (`ServeGreekModel`, verbatim from the record) |
| `convention` | the hedge convention (section 2) |
| `clock` | `now_ns`, the record's `as_of_ns`, `expiry_ns`, `next_event_ns`, `tau` (variance clock) |
| `price_map_delta` | how far the price's map and the Greeks' map are apart (H0-A); 0 when one map |

A portfolio answer (scenarios, attribution) reads ONE book version for every leg: the
workers' readers record the versions they saw, and when a publication lands mid-run the
whole computation runs once more; a second move is `AnalyticsStatus::VersionMoved` with no
totals. `ScenarioResult::attempts` says which.

### Certified inputs (H1's statements, review-H1 F1 / F2)

`classify_served_answer` / `classify_served_statement` decide what a served answer may feed:

| class | when | may feed |
|---|---|---|
| `Certified` | status `Ok` or `Exercised`, not `sampled`, statement class `Exact`, `RefinementValidated` or `KernelTested` | certified totals |
| `Estimate` | answered, but `sampled` or statement class `None` (e.g. the piecewise-linear slice between nodes) | its own `estimate_*` totals, never certified ones |
| `PriceOnly` | C0's price-only node | the price and its typed reason; no Greek |
| `Refused` | no number | nothing; its route and premium are reported |

No output says `AnalyticsStatus::Ok` on an input that is not `Certified`. A served chain
contract whose statement is `Sampled` is `TickStatus::Fallback` (H1's rule) and classifies
as `Estimate`.

## 2. Hedge Greeks and conventions (F11)

`hedge_greeks(reader, request)` answers one contract under a declared convention (values
equal A0's `AlFamilyGreekHeldFixed`):

| convention | delta | gamma | vega, theta |
|---|---|---|---|
| `ContractSigma` | the record's | the record's | the record's |
| `StickyStrike` | the record's (a record is sticky strike by construction) | the record's | the record's |
| `StickyDeltaMoneyness` | record delta + vega x dsigma/dS, dsigma/dS = -sigma'(k) / S, sigma'(k) the fitted curve's central difference at k = ln(K / F_fit), h = `kSmileSlopeStep` (1e-4); `SmileSlopeClass::FittedCurveCentralDifference` | NaN, `NeedsVannaVolga` (exercised rows keep gamma 0) | sticky-strike numbers |
| `RefittedSurface` | NaN, `RequiresRefit` | NaN, `RequiresRefit` | NaN, `RequiresRefit` (the price is still served) |

Reasons 0-9 and 32-34 of `HedgeGreekReason` mirror `ServeGreekReason` (A0's near-edge band:
gamma and theta NaN with `NearExerciseEdge`; an exercised row's delta is exactly +/-1);
A1's own are `SmileSlopeUnavailable` (64), `NeedsVannaVolga` (65), `RequiresRefit` (66). No
convention returns a number it cannot back: a typed reason replaces it.

Theta is the record's continuation-region identity per year of the record's variance clock;
a roll through expiry, an earnings date or an ex-dividend date is an event operation
(section 3.3), never theta.

## 3. Scenarios (F13)

`served_scenarios(book, portfolio, spec)` evaluates a matrix `spot_rel x vol_abs x
time_years` (row-major in that order; `ScenarioResult::index`). Each unique contract (expiry,
strike, side) is answered once at the base spot; each cell is computed two ways, labelled and
never mixed. Scenarios hold the served volatility (`ContractSigma` / `StickyStrike`); the
other conventions are refused with `InvalidArgument` (a sticky-moneyness scenario is a smile
move, a refit is the fitter's).

### 3.1 Taylor and its remainder estimate

Taylor = delta dS + gamma dS^2 / 2 + vega dsigma + theta dt. Declared order: spot 2, vol 1,
time 1, no cross terms (`TaylorOrder`; no tier serves vanna or charm). Declared validity box
`TaylorBox` (default spot 3 %, vol 3 points, time 7 days): a cell outside it is
`OutsideTaylorBox` for Taylor only. Each term is gated on its own shock (a zero shock never
reads its Greek).

The remainder estimate is class **Sampled**: measured at sample points, never a
certificate. It is the sum of the terms the cell's non-zero shocks need, with
rx = |x| / b_spot, ry = |y| / b_vol, rt = dt / h:

| term | estimate | samples (per unique contract, only when the matrix has the axes) |
|---|---|---|
| spot | dS^2 / 2 x max \|gamma(S (1 +/- b)) - gamma(S)\| | 2 record serves at the spot box ends |
| vol | \|a\| \|y\| + \|c\| y^2, the residual P(sigma + y) - P - vega y fitted as a y + c y^2 at y = +/- b_vol | 2 cold solves |
| time | \|a\| dt + \|c\| dt^2, the residual P(T - t) - P - theta t fitted at t = h / 2 and h (h = the time box) | 2 cold solves |
| spot x vol | \|y\| x max \|vega(S (1 +/- b)) - vega(S)\| x rx | the spot-end serves |
| spot x time | dt x max \|theta(S (1 +/- b)) - theta(S)\| x rx | the spot-end serves |
| vol x time | the corner difference \|P(s + y, T - h) - P(s + y, T) - P(s, T - h) + P(s, T)\| x b_vol / \|y\|, max over y = +/- b_vol, x ry x rt | 2 cold solves |
| spot map | where the cell's exact route is the cold kernel (a spot shock with a vol or time shock): max \|[cold(S') - cold(S)] - [record(S') - record(S)]\| at the box ends x rx | 2 cold solves |

The vol and time residuals are measured on the exact route's map (the cold kernel) against
the record's Greek, so their linear part carries the Greek's map error; a volatility floored
at `kScenarioSigmaFloor` (a side shorter than b/4) is scaled linearly from the other side. A
term the cell needs and cannot measure (a Greek withheld at a box end - A0's band - or a
refused solve) excludes the leg from Taylor: `RemainderUnbounded`. Cost: up to 2 record
serves and 9 cold solves per unique contract (the A1 row: 2,982 cold solves for 334
contracts).

Measured coverage (section 7): spot 988/988, vol 507/516, time 252/252, mixed 5443/5444 leg
cells on the SPY chain. Of the six worst vol misses five are puts within 0.27 sd of the
exercise edge (largest excess 3.1e-2 per share, K 798 put at -1 point: 8.73e-2 against
5.65e-2, ratio 1.55) and one a far-from-edge call at ratio 1.03; the one mixed miss is the
K 797 put. Near the edge the price is not smooth in sigma, which is where the estimate's
smoothness assumption fails; the row prints the worst cells of every class.

### 3.2 Exact repricing: one map per cell

| cell | route | value |
|---|---|---|
| pure spot shock | `RecordTier` | record(S') - record(S), tiers `tier`..`tier_max` (Ladder..Family by default); class = the weaker statement of the two answers, `exact_certificate_usd` = their sum |
| any vol or time shock | `ColdKernel` | andersen_lake(S', K, T - dt, sigma + dsigma, record rate, node yield, `cold_opts`) - the same kernel at the base; off-node strikes are `OffNode` |
| dt >= T | `EventSettlement` | payoff(S') - served base price |
| dated cash dividend (section 3.4) | `DiscreteDividend` | D1's production lattice with D1's event roll |

### 3.3 Events

A roll that crosses the entry's `next_event_ns` (ex-dividend, earnings) is an event the
record cannot price: exact `EventRequiresRefit`, Taylor `EventOperation`. dt is on the
record's variance clock; the event instant is located by mapping dt linearly onto
[`now_ns`, `expiry_ns`] (without an expiry instant any pending event counts as crossed). A
roll through expiry is `EventSettlement` (exact) and `EventOperation` (Taylor). Taylor never
steps across an event.

### 3.4 Dated cash dividends (D1)

A record does not serve dated cash (`ServeCashDividends::UnsupportedInRecord`).
`cash_dividend_scenarios` runs such contracts on D1's production lattice
(`american_discrete_div_production_greeks` / `_price`): a time roll across an ex-date is D1's
event operation (the crossed cash comes off the shocked stock and off the schedule, later
ex-dates move closer), never Taylor; D1's unsupported Greeks keep D1's reasons. The remainder
estimate is section 3.1's model with every sample on the lattice (no spot-map term: one map)
and h at most half the time to the first ex-date (a longer roll inside the box is
`RemainderUnbounded`).

### 3.5 Failed exposure and like-for-like totals

Each cell reports, per `ScenarioExclusion` reason, the legs excluded and their base premium
|quantity x multiplier x price| in USD, for Taylor and Exact separately. `taylor_pnl` and
`exact_pnl` each sum their own included legs, so their difference mixes the Taylor error with
exposure one method excluded; `matched_taylor_pnl - matched_exact_pnl` over `matched_legs`
(legs in both) is the Taylor error, read against `matched_remainder`. On the A1 chain at spot
-1 %: 247 vs 258 legs, `taylor_pnl - exact_pnl` = -7,616 USD, matched difference +66 USD
against a 4,855 USD estimate (the 11 Taylor-excluded legs are puts at or past the exercise
edge, |delta| ~ 1).

## 4. Attribution and deterministic aggregation

`served_attribution(book, portfolio, move, spec)` attributes one move: per leg the
position-scaled delta, gamma, vega and theta terms, `explained` = their left-to-right sum
(bit-equal to the leg's Taylor value in the scenario cell), `actual` = the exact P&L,
`unexplained` = actual - explained. Totals cover the legs in both (= the cell's matched
totals, bit for bit).

Aggregation: unique contracts are evaluated on `ScenarioSpec::workers` `std::jthread`s (never
the pricing pool; one reader per worker; each worker writes its own slots), and every total
is a Neumaier (improved Kahan) compensated sum over the legs in **input order** on the
calling thread. Scaled terms are computed in separate statements so no product is contracted
into an FMA. Results are bit-identical for any worker count (tested at 1 / 2 / 4 on one
portfolio; the A1 row repeats it on the 300-leg book: 0 cells differ). Units: per share x
quantity x multiplier = USD. The workload is fixed before measuring (`ServedWorkload`:
contracts, uniques, Greek mask, cell counts, expiries, events in the horizon).

## 5. European-only outputs on a declared common model

`european_outputs(ref, request)` returns a density, its CDF, a variance strip and a local
volatility ONLY on a compiled Markov common model (`EuropeanSurfaceRef::common_model`;
`american_markov.hpp`). On the American quoting surface - a `SliceServeRecord`
(`quoting_record`) or a book expiry (`quoting_book_expiry`) - it refuses with
`EuropeanOutputRefusal::AmericanQuotingSurface` and no number: Breeden-Litzenberger on
Black-76 prices built from American quoting vols describes that construction, not a market
density. Nothing calibrates here; a caller that wants these numbers fits the common model
and passes it.

| output | definition on the compiled chain |
|---|---|
| density | the unit mass at the initial state pushed forward through every transition (`apply_american_markov_transition`, Forward), on the layer's states S_i = growth x X_i |
| cdf | the cumulative density, ascending S |
| variance strip | -2 E[ln(X_T / X_0)] of the log-martingale X = S / G; refused across a cash event (`CashDividendJump`) and when the terminal mass at 0 exceeds `kCommonModelMaxZeroMass` = 1e-12 (`AtomAtZero`) |
| local vol | the model's own interval state volatilities (`interval_data().state_volatilities`) |

Budgets are reported, not tuned: mass and mean errors of the density, the model's logical
rows / states / bytes, calibration none, scenario none (a common-model scenario is a
recompiled model). Tests: the constant-vol chain's CDF sits within 5e-4 of the lognormal
(measured 1.62e-4) and its strip within 1e-6 of sigma^2 T (measured 3.0e-7); the density's
mass and mean are 1 within 1e-12, and it reprices the model's own European control (backward
rollback) within 1e-10 relative.

## 6. Session wiring (`ServedSession`)

H1 deferred the session / `PricedSurface` / chain-tick wiring to A1 (H1 report, Deviations
3). It lives in a new translation unit, `served_session.cpp`, through public APIs only;
`session.cpp`, `priced_surface.*` and `chain_tick.*` are not modified.

- `ServedSession::bind(VolaSession | PricedSurface, shared_ptr<const ServeBook>,
  span<SessionBookExpiry{T, expiry_id}>)`: the caller names which of the session's expiries
  the book holds (same T within `kSessionBookTRelTol` = 1e-12 relative). Refuses a null
  book, a non-positive T and a duplicate binding.
- `serve(reader, K, T, side, spot, now_ns)`: a bound T is the book's answer (tiers
  Ladder..Family, four Greeks, `family_bound`), declared `QuotingRecord` with the record's
  identity and clock and classified (section 1); an unbound T is the session's own cold
  route at the requested spot - `american_price` and `american_greeks_fd` on the session's
  iv / rate / yield / method / preset, declared `SessionCurve`, `price_map_delta` =
  |FD-map price - price|, class `Estimate`.
- `serve_chain(reader, spot, now_ns, SessionChainView, ChainTickOutputs, statements,
  scratch)`: bound contracts go through `ServeBookReader::serve_chain` with their
  `ServedChainStatement`; unbound ones take the cold route as `TickStatus::Fallback` (another
  model, no certificate). All book contracts come from one version: the reader's version is
  checked after every contract, the chain is redone once on a move, and a second move is
  `ErrorCode::Unavailable`.

## 7. Measurement: the A1 row (2026-10-23)

Bench `atx-vol-served-analytics-bench` (vol-rel-avx2, workers 1) on the SPY 23-day slice of
the serve-record bench (`spy23-quotes.csv`; fit grade A, record 713,880 bytes, 167 nodes,
S_fit 772.99, T 0.068204), base spot S_fit x 1.0021, now inside the record's clock.
Evidence: `C:\atx-cache\quote-benchmark\A1-20261023-analytics\measure\a1-row-a1a5247a\`.
The host carried other lanes' load (cpu 82 % before, 34 % after the run): latencies are
indicative, not quiet-window rows.

**Workload.** `chain`: every node side of the slice, quantity +1 x 100 (334 contracts, 334
unique; 258 certified, 76 estimate). `p300`: the 150 node sides nearest the money, twice,
LCG quantities in [-10, 10] (300 legs, 150 unique, all certified). Matrix 5 spot (+/-1 %,
+/-2 %) x 3 vol (+/-1 point) x 2 time (0, 1 day) = 30 cells, box (3 %, 3 points, 7 days).

**Error domain** (per share, per unique x cell where both methods are included):

| class | n | p50 | p99 | max | estimate covers |
|---|---|---|---|---|---|
| spot only | 988 | 1.75e-2 | 0.720 | 1.67 | 988 |
| vol only | 516 | 8.53e-3 | 8.17e-2 | 9.01e-2 | 507 |
| time only | 252 | 8.65e-4 | 2.04e-3 | 2.05e-3 | 252 |
| mixed | 5444 | 5.45e-2 | 0.792 | 2.32 | 5443 |

**Failed exposure** (chain, every spot-shocked cell): Taylor excludes 6 legs `MissingGreek`
(15,558 USD premium; A0's band withholds gamma, and theta in the time cells) and 5
`RemainderUnbounded` (14,704 USD; a box-end gamma withheld); the 76 estimate legs (5,326
USD) are summed apart on both methods in every cell. Exact excludes none.

**Cost.** Per contract (30 cells, base answer and setup included): Taylor p50 1.50 ms / p99
1.84 ms, exact p50 3.42 ms / p99 5.82 ms (chain); p300 Taylor 1.13 / 1.80 ms, exact 3.46 /
5.29 ms. Whole matrix (median of 7): chain Taylor 409 ms (2,982 cold solves) vs exact 1,120
ms (8,684); p300 200 ms vs 497 ms. Taylor's cost is its remainder estimate (section 3.1).
Memory: the chain's full result with per-leg cells 828,176 bytes; working set 9.2 -> 10.3 MB.

**Determinism.** p300 at workers 1 / 2 / 4: 0 of 30 cells differ bitwise.

**Near-boundary table** (54 puts within 1 sd of the exercise edge or exercised; spot -3 %
.. +3 %, per share Taylor / exact):

| K | edge_sd | status | -3 % | -1 % | +1 % | +3 % |
|---|---|---|---|---|---|---|
| 790 | 0.632 | ok | +24.02 / +22.15 | +6.81 / +6.77 | -5.62 / -5.58 | -13.26 / -12.78 |
| 797 | 0.265 | ok | +25.91 / +23.06 | +7.66 / +7.57 | -6.69 / -6.65 | -17.15 / -16.21 |
| 799 | 0.174 | ok | missing_greek / +23.16 | missing_greek / +7.67 | missing_greek / -6.89 | missing_greek / -17.10 |
| 804 | -0.032 | exercised | +23.24 / +23.24 | +7.75 / +7.75 | -7.75 / -7.39 | -23.24 / -19.07 |
| 830 | -0.809 | exercised | remainder_unbounded / +23.24 | remainder_unbounded / +7.75 | remainder_unbounded / -7.75 | remainder_unbounded / -23.14 |
| 875 | -1.694 | exercised | +23.24 / +23.24 | +7.75 / +7.75 | -7.75 / -7.75 | -23.24 / -23.24 |

Exercised rows are exact on the exercise side (delta -1, gamma 0); moving back toward the
continuation region Taylor misses the regained time value, which the spot remainder (gamma
at the box end) carries. Portfolio cells (matched): -3 % +2,586 USD vs estimate 13,781; +3 %
-3,960 vs 13,781.

**Ex-dividend table** (12 near-money node sides at their record sigma, rate 0.044961,
residual yield 0, a DECLARED synthetic cash dividend 1.80 USD at T / 2 on the real contract
inputs; D1 opts Standard; 11.3 ms, 216 lattice solves). dt = T / 4 is inside the box and
before the ex-date; dt = 3T / 4 crosses it:

| K side | base | (0, T/4) Taylor / exact | (0, 3T/4) Taylor / exact |
|---|---|---|---|
| 773 C | 10.0534 | -1.5423 / -1.6254 | event_operation / -5.7682 (1 event) |
| 773 P | 9.6638 | -0.9629 / -1.0551 | event_operation / -4.1112 (1 event) |
| 770 C | 12.2195 | -1.6309 / -1.7236 | event_operation / -6.1259 (1 event) |
| 770 P | 8.7935 | -1.0492 / -1.1472 | event_operation / -4.4474 (1 event) |

Portfolio: (0, T/4) Taylor -1,518.95 vs exact -1,623.82 USD (estimate 123.30); every
3T/4 cell is Taylor-excluded `EventOperation` (12 legs) and priced exact through the roll.

## 8. What W2c-m / Q0 may assume

- Every analytics answer names its book version, record identity, model, Greek map,
  convention and clock; portfolio answers read one version or say `VersionMoved`.
- `Ok` is never reported on a Sampled, slice-only, price-only or refused input; estimates
  are totalled apart.
- Hedge conventions beyond sticky strike are typed: sticky delta/moneyness delta only (its
  gamma is `NeedsVannaVolga`), refitted surface `RequiresRefit`.
- Taylor values come with a Sampled remainder estimate that covers the cross terms and the
  record-vs-kernel map term; it is an estimate, not a bound (section 3.1's misses).
- Compare Taylor with exact on `matched_*` totals, never on `taylor_pnl - exact_pnl`.
- Events (expiry, ex-dividend, earnings) are operations, never theta; a record cannot price
  a roll through its next event (`EventRequiresRefit`); dated cash goes to D1's lattice.
- Density / CDF / strip / local vol exist only on a declared common model.
- `ServedSession` routes a session's bound expiries to the book and the rest to the
  session's cold route as `Fallback` / `Estimate`.
